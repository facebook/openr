/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Decision.h"

#include <chrono>
#include <set>
#include <string>
#include <unordered_set>

#include <fb303/ServiceData.h>
#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#ifndef NO_FOLLY_EXCEPTION_TRACER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/PrefixState.h>
#include <openr/decision/RibEntry.h>

namespace fb303 = facebook::fb303;

using apache::thrift::can_throw;
using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace openr {

namespace detail {

void
DecisionPendingUpdates::applyLinkStateChange(
    std::string const& nodeName,
    LinkState::LinkStateChange const& change,
    apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents) {
  needsFullRebuild_ |=
      (change.topologyChanged || change.nodeLabelChanged ||
       // we only need a full rebuild if link attributes change locally
       // this would be a nexthop or link label change
       (change.linkAttributesChanged && nodeName == myNodeName_));
  addUpdate(perfEvents);
}

void
DecisionPendingUpdates::applyPrefixStateChange(
    std::unordered_set<folly::CIDRNetwork>&& change,
    apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents) {
  updatedPrefixes_.merge(std::move(change));
  addUpdate(perfEvents);
}

void
DecisionPendingUpdates::reset() {
  count_ = 0;
  perfEvents_ = std::nullopt;
  needsFullRebuild_ = false;
  updatedPrefixes_.clear();
}

void
DecisionPendingUpdates::addEvent(std::string const& eventDescription) {
  if (perfEvents_) {
    addPerfEvent(*perfEvents_, myNodeName_, eventDescription);
  }
}

std::optional<thrift::PerfEvents>
DecisionPendingUpdates::moveOutEvents() {
  std::optional<thrift::PerfEvents> events = std::move(perfEvents_);
  perfEvents_ = std::nullopt;
  return events;
}
void
DecisionPendingUpdates::addUpdate(
    apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents) {
  ++count_;

  // Update local copy of perf evens if it is newer than the one to be added
  // We do debounce (batch updates) for recomputing routes and in order to
  // measure convergence performance, it is better to use event which is
  // oldest.
  if (!perfEvents_ ||
      (perfEvents &&
       *perfEvents_->events_ref()->front().unixTs_ref() >
           *perfEvents->events_ref()->front().unixTs_ref())) {
    // if we don't have any perf events for this batch and this update also
    // doesn't have anything, let's start building the event list from now
    perfEvents_ = perfEvents ? *perfEvents : thrift::PerfEvents{};
    addPerfEvent(*perfEvents_, myNodeName_, "DECISION_RECEIVED");
  }
}
} // namespace detail

//
// Decision class implementation
//

Decision::Decision(
    std::shared_ptr<const Config> config,
    // TODO: migrate argument list flags to OpenrConfig
    bool bgpDryRun,
    std::chrono::milliseconds debounceMinDur,
    std::chrono::milliseconds debounceMaxDur,
    // consumer queue
    messaging::RQueue<thrift::Publication> kvStoreUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> staticRouteUpdatesQueue,
    // producer queue
    messaging::ReplicateQueue<DecisionRouteUpdate>& routeUpdatesQueue)
    : config_(config),
      routeUpdatesQueue_(routeUpdatesQueue),
      myNodeName_(*config->getConfig().node_name_ref()),
      pendingUpdates_(*config->getConfig().node_name_ref()),
      rebuildRoutesDebounced_(
          getEvb(), debounceMinDur, debounceMaxDur, [this]() noexcept {
            rebuildRoutes("DECISION_DEBOUNCE");
          }) {
  spfSolver_ = std::make_unique<SpfSolver>(
      config->getNodeName(),
      config->isV4Enabled(),
      config->isNodeSegmentLabelEnabled(),
      config->isAdjacencyLabelsEnabled(),
      bgpDryRun,
      config->isBestRouteSelectionEnabled(),
      config->isV4OverV6NexthopEnabled());

  coldStartTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    pendingUpdates_.setNeedsFullRebuild();
    rebuildRoutes("COLD_START_UPDATE");
  });
  if (auto eor = config->getConfig().eor_time_s_ref()) {
    coldStartTimer_->scheduleTimeout(std::chrono::seconds(*eor));
  }

  // Schedule periodic timer for counter submission
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    updateGlobalCounters();
    // Schedule next counters update
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);

  // Add reader to process publication from KvStore
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    LOG(INFO) << "Starting KvStore updates processing fiber";
    while (true) {
      auto maybeThriftPub = q.get(); // perform read
      VLOG(2) << "Received KvStore update";
      if (maybeThriftPub.hasError()) {
        LOG(INFO) << "Terminating KvStore updates processing fiber";
        break;
      }
      try {
        processPublication(std::move(maybeThriftPub).value());
      } catch (const std::exception& e) {
#ifndef NO_FOLLY_EXCEPTION_TRACER
        // collect stack strace then fail the process
        for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
          LOG(ERROR) << exInfo;
        }
#endif
        // FATAL to produce core dump
        LOG(FATAL) << "Exception occured in Decision::processPublication - "
                   << folly::exceptionStr(e);
      }
      // compute routes with exponential backoff timer if needed
      if (pendingUpdates_.needsRouteUpdate()) {
        rebuildRoutesDebounced_();
      }
    }
  });

  // Add reader to process static routes publication from prefix-manager
  addFiberTask(
      [q = std::move(staticRouteUpdatesQueue), this]() mutable noexcept {
        LOG(INFO) << "Starting static routes update processing fiber";
        while (true) {
          auto maybeThriftPub = q.get(); // perform read
          VLOG(2) << "Received static routes update";
          if (maybeThriftPub.hasError()) {
            LOG(INFO) << "Terminating static routes update processing fiber";
            break;
          }
          processStaticRoutesUpdate(std::move(maybeThriftPub).value());
        }
      });

  // Create RibPolicy timer to process routes on policy expiry
  ribPolicyTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    LOG(WARNING) << "RibPolicy is expired";
    pendingUpdates_.setNeedsFullRebuild();
    rebuildRoutes("RIB_POLICY_EXPIRED");
  });

  // Initialize some stat keys
  fb303::fbData->addStatExportType(
      "decision.rib_policy_processing.time_ms", fb303::AVG);
}

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
Decision::getDecisionRouteDb(std::string nodeName) {
  folly::Promise<std::unique_ptr<thrift::RouteDatabase>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), nodeName, this]() mutable {
    thrift::RouteDatabase routeDb;

    if (nodeName.empty()) {
      nodeName = myNodeName_;
    }
    auto maybeRouteDb =
        spfSolver_->buildRouteDb(nodeName, areaLinkStates_, prefixState_);
    if (maybeRouteDb.has_value()) {
      routeDb = maybeRouteDb->toThrift();
    }

    *routeDb.thisNodeName_ref() = nodeName;
    p.setValue(std::make_unique<thrift::RouteDatabase>(std::move(routeDb)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
Decision::getDecisionAdjacenciesFiltered(thrift::AdjacenciesFilter filter) {
  folly::Promise<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [p = std::move(p), filter = std::move(filter), this]() mutable {
        auto res = std::make_unique<std::vector<thrift::AdjacencyDatabase>>();
        for (auto const& [area, linkState] : areaLinkStates_) {
          if (filter.get_selectAreas().empty() ||
              filter.get_selectAreas().count(area)) {
            for (auto const& [_, db] : linkState.getAdjacencyDatabases()) {
              res->push_back(db);
            }
          }
        }
        p.setValue(std::move(res));
      });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>
Decision::getReceivedRoutesFiltered(thrift::ReceivedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable noexcept {
        try {
          // Get route details
          auto routes = prefixState_.getReceivedRoutesFiltered(filter);

          // Add best path result to this
          auto const& bestRoutesCache = spfSolver_->getBestRoutesCache();
          for (auto& route : routes) {
            auto const& bestRoutesIt =
                bestRoutesCache.find(toIPNetwork(*route.prefix_ref()));
            if (bestRoutesIt != bestRoutesCache.end()) {
              auto const& bestRoutes = bestRoutesIt->second;
              // Set all selected node-area
              for (auto const& [node, area] : bestRoutes.allNodeAreas) {
                route.bestKeys_ref()->emplace_back();
                auto& key = route.bestKeys_ref()->back();
                key.node_ref() = node;
                key.area_ref() = area;
              }
              // Set best node-area
              route.bestKey_ref()->node_ref() = bestRoutes.bestNodeArea.first;
              route.bestKey_ref()->area_ref() = bestRoutes.bestNodeArea.second;
            }
          }

          // Set the promise
          p.setValue(std::make_unique<std::vector<thrift::ReceivedRouteDetail>>(
              std::move(routes)));
        } catch (const thrift::OpenrError& e) {
          p.setException(e);
        }
      });
  return std::move(sf);
}

folly::SemiFuture<folly::Unit>
Decision::clearRibPolicy() {
  auto [p, sf] = folly::makePromiseContract<folly::Unit>();
  if (not config_->isRibPolicyEnabled()) {
    thrift::OpenrError error;
    error.message_ref() = "RibPolicy feature is not enabled";
    p.setException(error);
    return std::move(sf);
  }

  runInEventBaseThread([this, p = std::move(p)]() mutable {
    if (not ribPolicy_) {
      thrift::OpenrError error;
      error.message_ref() = "No RIB policy configured";
      p.setException(error);
    } else {
      ribPolicy_ = nullptr;
      // Trigger route computation
      pendingUpdates_.setNeedsFullRebuild();
      rebuildRoutes("RIB_POLICY_CLEARED");
      p.setValue();
    }
  });

  return std::move(sf);
}

folly::SemiFuture<folly::Unit>
Decision::setRibPolicy(thrift::RibPolicy const& ribPolicyThrift) {
  auto [p, sf] = folly::makePromiseContract<folly::Unit>();
  if (not config_->isRibPolicyEnabled()) {
    thrift::OpenrError error;
    error.message_ref() = "RibPolicy feature is not enabled";
    p.setException(error);
    return std::move(sf);
  }

  std::unique_ptr<RibPolicy> ribPolicy;
  try {
    ribPolicy = std::make_unique<RibPolicy>(ribPolicyThrift);
  } catch (thrift::OpenrError const& e) {
    p.setException(e);
    return std::move(sf);
  }

  runInEventBaseThread(
      [this, p = std::move(p), ribPolicy = std::move(ribPolicy)]() mutable {
        const auto durationLeft = ribPolicy->getTtlDuration();
        if (durationLeft.count() <= 0) {
          LOG(ERROR)
              << "Ignoring RibPolicy update with new instance because of "
              << "staleness. Validity " << durationLeft.count() << "ms";
          return;
        }

        // Update local policy instance
        LOG(INFO) << "Updating RibPolicy with new instance. Validity "
                  << durationLeft.count() << "ms";
        ribPolicy_ = std::move(ribPolicy);

        // Schedule timer for processing routes on expiry
        ribPolicyTimer_->scheduleTimeout(durationLeft);

        // Trigger route computation
        pendingUpdates_.setNeedsFullRebuild();
        rebuildRoutes("RIB_POLICY_UPDATE");

        // Mark the policy update request to be done
        p.setValue();
      });
  return std::move(sf);
}

folly::SemiFuture<thrift::RibPolicy>
Decision::getRibPolicy() {
  auto [p, sf] = folly::makePromiseContract<thrift::RibPolicy>();
  if (not config_->isRibPolicyEnabled()) {
    thrift::OpenrError error;
    error.message_ref() = "RibPolicy feature is not enabled";
    p.setException(error);
    return std::move(sf);
  }

  runInEventBaseThread([this, p = std::move(p)]() mutable {
    if (ribPolicy_) {
      p.setValue(ribPolicy_->toThrift());
    } else {
      thrift::OpenrError e;
      e.message_ref() = "RibPolicy is not configured";
      p.setException(e);
    }
  });
  return std::move(sf);
}

void
Decision::processPublication(thrift::Publication&& thriftPub) {
  CHECK(not thriftPub.area_ref()->empty());
  auto const& area = *thriftPub.area_ref();

  if (!areaLinkStates_.count(area)) {
    areaLinkStates_.emplace(area, area);
  }
  auto& areaLinkState = areaLinkStates_.at(area);

  // Nothing to process if no adj/prefix db changes
  if (thriftPub.keyVals_ref()->empty() and
      thriftPub.expiredKeys_ref()->empty()) {
    return;
  }

  // LSDB addition/update
  for (const auto& [key, rawVal] : *thriftPub.keyVals_ref()) {
    if (not rawVal.value_ref().has_value()) {
      // skip TTL update
      DCHECK(*rawVal.ttlVersion_ref() > 0);
      continue;
    }

    try {
      if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
        // adjacencyDb: update keys starting with "adj:"
        auto adjacencyDb = readThriftObjStr<thrift::AdjacencyDatabase>(
            rawVal.value_ref().value(), serializer_);
        auto& nodeName = adjacencyDb.get_thisNodeName();
        LinkStateMetric holdUpTtl = 0, holdDownTtl = 0;
        adjacencyDb.area_ref() = area;

        fb303::fbData->addStatValue("decision.adj_db_update", 1, fb303::COUNT);
        pendingUpdates_.applyLinkStateChange(
            nodeName,
            areaLinkState.updateAdjacencyDatabase(
                adjacencyDb, holdUpTtl, holdDownTtl),
            adjacencyDb.perfEvents_ref());
      } else if (key.find(Constants::kPrefixDbMarker.toString()) == 0) {
        // prefixDb: update keys starting with "prefix:"
        auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
            rawVal.value_ref().value(), serializer_);
        if (1 != prefixDb.get_prefixEntries().size()) {
          LOG(ERROR) << "Expecting exactly one entry per prefix key";
          fb303::fbData->addStatValue("decision.error", 1, fb303::COUNT);
          continue;
        }
        auto const& entry = prefixDb.get_prefixEntries().front();
        auto const& areaStack = entry.get_area_stack();
        // Ignore self redistributed route reflection
        // These routes are programmed by Decision,
        // re-origintaed by me to areas that do not have the best prefix entry
        if (prefixDb.get_thisNodeName() == myNodeName_ &&
            areaStack.size() > 0 && areaLinkStates_.count(areaStack.back())) {
          VLOG(2) << "Ignore self redistributed route reflection for prefix: "
                  << key << " area_stack: " << folly::join(",", areaStack);
          continue;
        }

        // construct new prefix key with local publication area id
        PrefixKey prefixKey(
            prefixDb.get_thisNodeName(), toIPNetwork(entry.get_prefix()), area);

        fb303::fbData->addStatValue(
            "decision.prefix_db_update", 1, fb303::COUNT);
        pendingUpdates_.applyPrefixStateChange(
            prefixDb.get_deletePrefix()
                ? prefixState_.deletePrefix(prefixKey)
                : prefixState_.updatePrefix(prefixKey, entry),
            prefixDb.perfEvents_ref());
      }
    } catch (const std::exception& e) {
      LOG(ERROR) << "Failed to deserialize info for key " << key
                 << ". Exception: " << folly::exceptionStr(e);
    }
  }

  // LSDB deletion
  for (const auto& key : *thriftPub.expiredKeys_ref()) {
    std::string nodeName = getNodeNameFromKey(key);

    if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
      // adjacencyDb: delete keys starting with "adj:"
      pendingUpdates_.applyLinkStateChange(
          nodeName,
          areaLinkState.deleteAdjacencyDatabase(nodeName),
          thrift::PrefixDatabase().perfEvents_ref()); // Empty perf events
    } else if (key.find(Constants::kPrefixDbMarker.toString()) == 0) {
      // prefixDb: delete keys starting with "prefix:"
      // TODO: avoid decoding from string
      auto maybePrefixKey = PrefixKey::fromStr(key);
      if (maybePrefixKey.hasError()) {
        LOG(ERROR) << "Unable to parse prefix key: " << key << ". Skipping.";
        continue;
      }
      // construct new prefix key with local publication area id
      PrefixKey prefixKey(
          maybePrefixKey.value().getNodeName(),
          maybePrefixKey.value().getCIDRNetwork(),
          area);
      pendingUpdates_.applyPrefixStateChange(
          prefixState_.deletePrefix(prefixKey),
          thrift::PrefixDatabase().perfEvents_ref()); // Empty perf events
    }
  }
}

void
Decision::processStaticRoutesUpdate(DecisionRouteUpdate&& routeUpdate) {
  // update static unicast routes
  if (routeUpdate.unicastRoutesToUpdate.size() or
      routeUpdate.unicastRoutesToDelete.size()) {
    std::vector<RibUnicastEntry> unicastRoutesToUpdate{};
    std::unordered_set<folly::CIDRNetwork> addedPrefixes{};
    for (const auto& [prefix, ribUnicastEntry] :
         routeUpdate.unicastRoutesToUpdate) {
      unicastRoutesToUpdate.emplace_back(ribUnicastEntry);
      addedPrefixes.emplace(prefix);
    }

    // store as local storage
    spfSolver_->updateStaticUnicastRoutes(
        unicastRoutesToUpdate, routeUpdate.unicastRoutesToDelete);

    // only apply prefix updates, no full DB rebuild
    // TODO: remove std::unordered_set usage
    pendingUpdates_.applyPrefixStateChange(
        std::move(addedPrefixes), thrift::PrefixDatabase().perfEvents_ref());
    pendingUpdates_.applyPrefixStateChange(
        std::unordered_set<folly::CIDRNetwork>{
            routeUpdate.unicastRoutesToDelete.cbegin(),
            routeUpdate.unicastRoutesToDelete.cend()},
        thrift::PrefixDatabase().perfEvents_ref());
  }

  // update static MPLS routes
  if (routeUpdate.mplsRoutesToUpdate.size() or
      routeUpdate.mplsRoutesToDelete.size()) {
    spfSolver_->updateStaticMplsRoutes(
        routeUpdate.mplsRoutesToUpdate, routeUpdate.mplsRoutesToDelete);
    pendingUpdates_.setNeedsFullRebuild(); // Mark for full DB rebuild
  }
  rebuildRoutesDebounced_();
}

void
Decision::rebuildRoutes(std::string const& event) {
  if (coldStartTimer_->isScheduled()) {
    return;
  }

  pendingUpdates_.addEvent(event);
  VLOG(1) << "Decision: processing " << pendingUpdates_.getCount()
          << " accumulated updates. " << event;
  if (pendingUpdates_.perfEvents()) {
    if (auto expectedDuration = getDurationBetweenPerfEvents(
            *pendingUpdates_.perfEvents(),
            "DECISION_RECEIVED",
            "DECISION_DEBOUNCE")) {
      VLOG(2) << "Debounced " << pendingUpdates_.getCount() << " events over "
              << expectedDuration->count() << "ms.";
    }
  }

  DecisionRouteUpdate update;
  if (pendingUpdates_.needsFullRebuild()) {
    // if only static routes gets updated, we still need to update routes
    // because there maybe routes depended on static routes.
    auto maybeRouteDb =
        spfSolver_->buildRouteDb(myNodeName_, areaLinkStates_, prefixState_);
    LOG_IF(WARNING, !maybeRouteDb)
        << "SEVERE: full route rebuild resulted in no routes";
    auto db = maybeRouteDb.has_value() ? std::move(maybeRouteDb).value()
                                       : DecisionRouteDb{};
    if (ribPolicy_) {
      auto start = std::chrono::steady_clock::now();
      ribPolicy_->applyPolicy(db.unicastRoutes);
      updateCounters(
          "decision.rib_policy_processing.time_ms",
          start,
          std::chrono::steady_clock::now());
    }
    // update `DecisionRouteDb` cache and return delta as `update`
    update = routeDb_.calculateUpdate(std::move(db));
  } else {
    // process prefixes update from `prefixState_`
    for (auto const& prefix : pendingUpdates_.updatedPrefixes()) {
      if (auto maybeRibEntry = spfSolver_->createRouteForPrefixOrGetStaticRoute(
              myNodeName_, areaLinkStates_, prefixState_, prefix)) {
        update.addRouteToUpdate(std::move(maybeRibEntry).value());
      } else {
        update.unicastRoutesToDelete.emplace_back(prefix);
      }
    }
    if (ribPolicy_) {
      auto start = std::chrono::steady_clock::now();
      auto const changes =
          ribPolicy_->applyPolicy(update.unicastRoutesToUpdate);
      updateCounters(
          "decision.rib_policy_processing.time_ms",
          start,
          std::chrono::steady_clock::now());
      for (auto const& prefix : changes.deletedRoutes) {
        update.unicastRoutesToDelete.push_back(prefix);
      }
    }
  }

  routeDb_.update(update);
  pendingUpdates_.addEvent("ROUTE_UPDATE");
  update.perfEvents = pendingUpdates_.moveOutEvents();
  pendingUpdates_.reset();

  // send `DecisionRouteUpdate` to Fib/PrefixMgr
  routeUpdatesQueue_.push(std::move(update));
}

void
Decision::updateCounters(
    std::string key,
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) const {
  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  fb303::fbData->addStatValue(key, elapsedTime.count(), fb303::AVG);
}

void
Decision::updateGlobalCounters() const {
  size_t numAdjacencies = 0, numPartialAdjacencies = 0;
  std::unordered_set<std::string> nodeSet;
  for (auto const& [_, linkState] : areaLinkStates_) {
    numAdjacencies += linkState.numLinks();
    auto const& mySpfResult = linkState.getSpfResult(myNodeName_);
    for (auto const& kv : linkState.getAdjacencyDatabases()) {
      nodeSet.insert(kv.first);
      const auto& adjDb = kv.second;
      size_t numLinks = linkState.linksFromNode(kv.first).size();
      // Consider partial adjacency only iff node is reachable from current
      // node
      if (mySpfResult.count(*adjDb.thisNodeName_ref()) && 0 != numLinks) {
        // only add to the count if this node is not completely disconnected
        size_t diff = adjDb.adjacencies_ref()->size() - numLinks;
        // Number of links (bi-directional) must be <= number of adjacencies
        CHECK_GE(diff, 0);
        numPartialAdjacencies += diff;
      }
    }
  }

  size_t numConflictingPrefixes{0};
  for (const auto& [prefix, prefixEntries] : prefixState_.prefixes()) {
    if (not PrefixState::hasConflictingForwardingInfo(prefixEntries)) {
      continue;
    }
    LOG(WARNING) << "Prefix " << folly::IPAddress::networkToString(prefix)
                 << " has conflicting "
                 << "forwarding algorithm or type.";
    numConflictingPrefixes += 1;
  }

  // Add custom counters
  fb303::fbData->setCounter(
      "decision.num_conflicting_prefixes", numConflictingPrefixes);
  fb303::fbData->setCounter(
      "decision.num_partial_adjacencies", numPartialAdjacencies);
  fb303::fbData->setCounter(
      "decision.num_complete_adjacencies", numAdjacencies);
  // When node has no adjacencies then linkState reports 0
  fb303::fbData->setCounter(
      "decision.num_nodes", std::max(nodeSet.size(), static_cast<size_t>(1ul)));
  fb303::fbData->setCounter(
      "decision.num_prefixes", prefixState_.prefixes().size());
}

} // namespace openr
