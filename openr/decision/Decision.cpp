/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>

#include <fb303/ServiceData.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <utility>

#ifndef NO_FOLLY_EXCEPTION_TRACER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif

#include <openr/common/Constants.h>
#include <openr/common/Flags.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/Decision.h>
#include <openr/decision/PrefixState.h>
#include <openr/decision/RibEntry.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>

namespace fb303 = facebook::fb303;

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
    // consumer queue
    messaging::RQueue<PeerEvent> peerUpdatesQueue,
    messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> staticRouteUpdatesQueue,
    // producer queue
    messaging::ReplicateQueue<DecisionRouteUpdate>& routeUpdatesQueue)
    : config_(config),
      routeUpdatesQueue_(routeUpdatesQueue),
      myNodeName_(*config->getConfig().node_name_ref()),
      pendingUpdates_(*config->getConfig().node_name_ref()),
      rebuildRoutesDebounced_(
          getEvb(),
          std::chrono::milliseconds(*config->getConfig()
                                         .decision_config_ref()
                                         ->debounce_min_ms_ref()),
          std::chrono::milliseconds(*config->getConfig()
                                         .decision_config_ref()
                                         ->debounce_max_ms_ref()),
          [this]() noexcept { rebuildRoutes("DECISION_DEBOUNCE"); }),
      saveRibPolicyDebounced_(
          getEvb(),
          std::chrono::milliseconds(*config->getConfig()
                                         .decision_config_ref()
                                         ->save_rib_policy_min_ms_ref()),
          std::chrono::milliseconds(*config->getConfig()
                                         .decision_config_ref()
                                         ->save_rib_policy_max_ms_ref()),
          [this]() noexcept { saveRibPolicy(); }) {
  spfSolver_ = std::make_unique<SpfSolver>(
      config->getNodeName(),
      config->isV4Enabled(),
      config->isSegmentRoutingEnabled(),
      config->isAdjacencyLabelsEnabled(),
      config->isBgpRouteProgrammingEnabled(),
      config->isBestRouteSelectionEnabled(),
      config->isV4OverV6NexthopEnabled());
  // Populate prefix types whose static routes Decision awaits before initial
  // RIB computation.
  if (config->isSegmentRoutingEnabled() and
      config->getConfig().get_enable_bgp_peering() and
      config->isBgpAddPathEnabled()) {
    // Static MPLS routes will be generated by BgpSpeaker for received BGP
    // prefixes.
    XLOG(INFO) << "[Initialization] Decision should wait for BGP type static "
                  "routes.";
    unreceivedRouteTypes_.emplace(thrift::PrefixType::BGP);
  }
  if (config->isVipServiceEnabled()) {
    // Static unicast routes will be generated by PrefixManager for received
    // VIPs.
    XLOG(INFO) << "[Initialization] Decision should wait for VIP type static "
                  "routes.";
    unreceivedRouteTypes_.emplace(thrift::PrefixType::VIP);
  }
  if (auto prefixes = config->getConfig().originated_prefixes_ref()) {
    // Static unicast routes will be generated by PrefixManager for config
    // originated prefixes with install_to_fib set.
    XLOG(INFO) << "[Initialization] Decision should wait for CONFIG type "
                  "static routes.";
    unreceivedRouteTypes_.emplace(thrift::PrefixType::CONFIG);
  }

  // TODO: Remove coldStartTimer_ and eor_time_s_ref from config after OpenR
  // initialization procedure.
  coldStartTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    pendingUpdates_.setNeedsFullRebuild();
    rebuildRoutes("COLD_START_UPDATE");
  });

  // Do not activate coldStartTimer_ if new OpenR initialization is enabled.
  if (not config_->getConfig().get_enable_initialization_process()) {
    auto eorTimeRef = config->getConfig().eor_time_s_ref();
    CHECK(eorTimeRef.has_value());
    coldStartTimer_->scheduleTimeout(std::chrono::seconds(*eorTimeRef));
  }

  // Schedule periodic timer for counter submission
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    updateGlobalCounters();
    // Schedule next counters update
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);

  // Add reader to process peer updates from LinkMonitor
  addFiberTask([q = std::move(peerUpdatesQueue), this]() mutable noexcept {
    XLOG(INFO) << "Starting peer updates processing fiber";
    while (true) {
      auto maybePeerUpdate = q.get(); // perform read
      XLOG(DBG3) << "Received peer update";
      if (maybePeerUpdate.hasError()) {
        XLOG(INFO) << "Terminating peer updates processing fiber";
        break;
      }
      processPeerUpdates(std::move(maybePeerUpdate).value());
    }
  });

  // Add reader to process publication from KvStore
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    // Block processing KvStore publication until initial peers are received.
    // This helps avoid missing KvStore adjacency publications for peers.
    if (*config_->getConfig().enable_ordered_adj_publication_ref()) {
      initialPeersReceivedBaton_.wait();
    }

    XLOG(INFO) << "Starting KvStore updates processing fiber";
    while (true) {
      auto maybePub = q.get(); // perform read
      XLOG(DBG3) << "Received KvStore update";
      if (maybePub.hasError()) {
        XLOG(INFO) << "Terminating KvStore updates processing fiber";
        break;
      }
      try {
        folly::variant_match(
            std::move(maybePub).value(),
            [this](thrift::Publication&& pub) {
              processPublication(std::move(pub));
              // Compute routes with exponential backoff timer if needed
              if (pendingUpdates_.needsRouteUpdate()) {
                rebuildRoutesDebounced_();
              }
            },
            [this](thrift::InitializationEvent&& event) {
              CHECK(event == thrift::InitializationEvent::KVSTORE_SYNCED)
                  << fmt::format(
                         "Unexpected initialization event: {}",
                         apache::thrift::util::enumNameSafe(event));

              // Received all initial KvStore publications.
              XLOG(INFO) << "[Initialization] All initial publications are "
                            "received from KvStore.";
              initialKvStoreSynced_ = true;
              triggerInitialBuildRoutes();
            });
      } catch (const std::exception& e) {
#ifndef NO_FOLLY_EXCEPTION_TRACER
        // collect stack strace then fail the process
        for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
          XLOG(ERR) << exInfo;
        }
#endif
        // FATAL to produce core dump
        XLOG(FATAL) << "Exception occured in Decision::processPublication - "
                    << folly::exceptionStr(e);
      }
    }
  });

  // Add reader to process static routes publication from prefix-manager
  addFiberTask(
      [q = std::move(staticRouteUpdatesQueue), this]() mutable noexcept {
        XLOG(INFO) << "Starting static routes update processing fiber";
        while (true) {
          auto maybeThriftPub = q.get(); // perform read
          if (maybeThriftPub.hasError()) {
            XLOG(INFO) << "Terminating static routes update processing fiber";
            break;
          }
          const auto prefixType = maybeThriftPub.value().prefixType;
          if (prefixType.has_value()) {
            XLOG(DBG2) << fmt::format(
                "Received static routes update of prefix type {}",
                apache::thrift::util::enumNameSafe<thrift::PrefixType>(
                    prefixType.value()));
          } else {
            XLOG(DBG2) << "Received static routes update";
          }
          processStaticRoutesUpdate(std::move(maybeThriftPub).value());
        }
      });

  // Read rib policy from saved file.
  addFiberTask([this]() mutable noexcept {
    XLOG(INFO) << "Starting readRibPolicy fiber";
    readRibPolicy();

    initialRibPolicyRead_ = true;
    triggerInitialBuildRoutes();
  });

  // Create RibPolicy timer to process routes on policy expiry
  ribPolicyTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    XLOG(WARNING) << "RibPolicy is expired";
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
          XLOG(ERR) << "Ignoring RibPolicy update with new instance because of "
                    << "staleness. Validity " << durationLeft.count() << "ms";
          return;
        }

        // Update local policy instance
        XLOG(INFO) << "Updating RibPolicy with new instance. Validity "
                   << durationLeft.count() << "ms";
        ribPolicy_ = std::move(ribPolicy);

        // Schedule timer for processing routes on expiry
        ribPolicyTimer_->scheduleTimeout(durationLeft);

        // Trigger route computation
        pendingUpdates_.setNeedsFullRebuild();
        rebuildRoutes("RIB_POLICY_UPDATE");

        // Save rib policy to file.
        saveRibPolicyDebounced_();

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
Decision::processPeerUpdates(PeerEvent&& event) {
  if (not initialPeersReceivedBaton_.try_wait()) {
    XLOG(INFO) << "[Initialization] Received initial PeerEvent.";
    // LinkMonitor publishes detected peers in one shot in Open/R initialization
    // process. Initial route computation will blocked until adjacence with all
    // peers are received.
    for (const auto& [area, areaPeerEvent] : event) {
      for (const auto& [peerName, _] : areaPeerEvent.peersToAdd) {
        // Both unidirectional adj are expected to be received.
        areaToPendingAdjacency_[area].insert(
            std::make_pair(peerName, myNodeName_));
        areaToPendingAdjacency_[area].insert(
            std::make_pair(myNodeName_, peerName));
        XLOG(INFO) << fmt::format(
            "[Initialization] Decision should wait for bi-directional "
            "adjacency with up peer {}",
            peerName);
      };
    };
    initialPeersReceivedBaton_.post();
    return;
  }

  // Incremental peer events.
  for (const auto& [area, areaPeerEvent] : event) {
    if (areaToPendingAdjacency_.count(area) == 0) {
      continue;
    }
    // Remove deleted peers from areaToPendingAdjacency_.
    for (const auto& peerName : areaPeerEvent.peersToDel) {
      if (areaToPendingAdjacency_[area].erase(
              std::make_pair(peerName, myNodeName_)) and
          areaToPendingAdjacency_[area].erase(
              std::make_pair(myNodeName_, peerName))) {
        XLOG(INFO) << "[Initialization] No need to wait for dual directional "
                      "adjacency with down peer "
                   << peerName;
      }
      if (areaToPendingAdjacency_[area].empty()) {
        areaToPendingAdjacency_.erase(area);
        if (areaToPendingAdjacency_.empty()) {
          triggerInitialBuildRoutes();
          return;
        }
      }
    }
    // Note: Incremental added peers are not added into
    // areaToPendingAdjacency_. This makes sure Decision could converge in
    // Open/R initialization process.
  }
}

void
Decision::filterUnuseableAdjacency(thrift::AdjacencyDatabase& adjacencyDb) {
  if (not *config_->getConfig().enable_ordered_adj_publication_ref()) {
    return;
  }
  // In order to make Open/R initialization process of cold boot node hitless,
  // we would like to have the cold node compute & program all required routes
  // ahead of peers sending traffic through it. There are two stages from
  // adjacency propagation perspective to achieve this.
  // Stage1:
  // - coldboot_node injects `adj:coldboot_node->peer_node`,
  // - peer_node injects `adj:peer_node->coldboot_node` with
  //   `adjOnlyUsedByOtherNode=true`
  // NOTE: This means `adj:peer_node<->coldboot_node` can only be used by
  // coldboot_node, As a result, coldboot_node compute & program routes first.
  //
  // Stage2:
  // - peer_node updates `adj:peer_node->coldboot_node`
  // In this way, peers start using coldboot_node for route computation &
  // programming.
  for (auto adjIter = adjacencyDb.adjacencies_ref()->begin();
       adjIter != adjacencyDb.adjacencies_ref()->end();) {
    if (*adjIter->adjOnlyUsedByOtherNode_ref() and
        *adjIter->otherNodeName_ref() != myNodeName_) {
      // Remove thrift::Adjacency if `adjOnlyUsedByOtherNode=true` but
      // `myNodeName_!=otherNodeName`
      adjacencyDb.adjacencies_ref()->erase(adjIter);
      XLOG(INFO) << fmt::format(
          "Filtered adjacency {}:{}->{}:{} since it cannot be used by {}.",
          *adjacencyDb.thisNodeName_ref(),
          *adjIter->ifName_ref(),
          *adjIter->otherNodeName_ref(),
          *adjIter->otherIfName_ref(),
          myNodeName_);
    } else {
      ++adjIter;
    }
  }
}

void
Decision::updatePendingAdjacency(
    const std::string& area, const thrift::AdjacencyDatabase& newAdjacencyDb) {
  // Update pending adjacency with up peers in Open/R initialization process.
  //
  // In case of two nodes (A, B) restart simultaneously,
  // - A will inject A->B adj (only used by B, `adjOnlyUsedByOtherNode=true`).
  // - B will inject B->A adj (only used by A, `adjOnlyUsedByOtherNode=true`).
  // In order to make sure initialization process could proceed at both nodes
  // without deadlock, here we will ignore `adjOnlyUsedByOtherNode` and remove
  // adj `A->B` and 'B->A' from pending list.
  for (const auto& adj : *newAdjacencyDb.adjacencies_ref()) {
    if (areaToPendingAdjacency_.count(area) == 0) {
      return;
    }
    const auto& node = *newAdjacencyDb.thisNodeName_ref();
    const auto& otherNode = *adj.otherNodeName_ref();
    if (areaToPendingAdjacency_[area].erase(std::make_pair(node, otherNode))) {
      XLOG(INFO) << fmt::format(
          "[Initialization] Received adjacency {}:{}->{}:{}.",
          node,
          *adj.ifName_ref(),
          *adj.otherNodeName_ref(),
          *adj.otherIfName_ref());
    }
    if (areaToPendingAdjacency_[area].empty()) {
      areaToPendingAdjacency_.erase(area);
      if (areaToPendingAdjacency_.empty()) {
        // Received adjacency with all peers, trigger initial route build.
        triggerInitialBuildRoutes();
        return;
      }
    }
  }
}

void
Decision::saveRibPolicy() {
  // Only save rib policy when Open/R initialization is enabled.
  if (not config_->getConfig().get_enable_initialization_process()) {
    return;
  }

  std::ofstream ribPolicyFile;
  ribPolicyFile.open(FLAGS_rib_policy_file, std::ios::out | std::ios::trunc);
  if (not ribPolicyFile.is_open()) {
    XLOG(ERR) << "Could not open rib policy file for writing";
    return;
  }

  const auto nowInSec = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

  // 1st line: epoch time
  ribPolicyFile << nowInSec << "\n";
  // 2nd line: ttl duration in milliseconds since epoch time
  ribPolicyFile << ribPolicy_->getTtlDuration().count() << "\n";
  // 3rd line: rib policy
  ribPolicyFile << apache::thrift::SimpleJSONSerializer::serialize<std::string>(
      ribPolicy_->toThrift());

  ribPolicyFile.close();
  XLOG(INFO) << "Saved rib policy to " << FLAGS_rib_policy_file;
}

void
Decision::readRibPolicy() {
  // Only save rib policy when Open/R initialization is enabled.
  if (not config_->getConfig().get_enable_initialization_process() or
      not config_->isRibPolicyEnabled()) {
    return;
  }

  std::ifstream ribPolicyFile;
  ribPolicyFile.open(FLAGS_rib_policy_file);
  if (not ribPolicyFile.is_open()) {
    XLOG(INFO) << "Could not open rib policy file " << FLAGS_rib_policy_file;
    return;
  }

  std::string line;
  std::vector<std::string> lines;
  while (getline(ribPolicyFile, line)) {
    lines.emplace_back(line);
  }
  ribPolicyFile.close();

  if (lines.size() != 3) {
    XLOG(ERR) << "Invalid lines size " << lines.size();
    return;
  }

  auto storeTimeInSec = std::stol(lines[0], nullptr, 10);
  auto storeTtlDurationMs = std::stol(lines[1], nullptr, 10);
  auto ribPolicyThrift =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::RibPolicy>(
          lines[2]);

  const auto nowInSec = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
  int64_t ttlDurationSec =
      storeTimeInSec + storeTtlDurationMs / 1000 - nowInSec;
  if (ttlDurationSec < 0) {
    XLOG(INFO) << "[Initialization] Skip loading expired rib policy file "
               << FLAGS_rib_policy_file;
    return;
  }

  ribPolicyThrift.ttl_secs_ref() = ttlDurationSec;
  ribPolicy_ = std::make_unique<RibPolicy>(ribPolicyThrift);
  XLOG(INFO) << fmt::format(
      "[Initialization] Read Rib policy successfully from {}, ttlDurationSec: "
      "{} seconds",
      FLAGS_rib_policy_file,
      ttlDurationSec);
  return;
}

void
Decision::processPublication(thrift::Publication&& thriftPub) {
  CHECK(not thriftPub.area_ref()->empty());
  auto const& area = *thriftPub.area_ref();

  auto it = areaLinkStates_.find(area);
  if (it == areaLinkStates_.end()) {
    it = areaLinkStates_.emplace(area, area).first;
  }
  auto& areaLinkState = it->second;

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

        // Process adjacency to unblock Open/R initialization.
        updatePendingAdjacency(area, adjacencyDb);

        // Filter adjacency that cannot be used by this node in route
        // computation.
        filterUnuseableAdjacency(adjacencyDb);

        auto& nodeName = adjacencyDb.get_thisNodeName();
        LinkStateMetric holdUpTtl = 0, holdDownTtl = 0;
        // TODO: can we directly use area in AdjacencyDatabase?
        adjacencyDb.area_ref() = area;

        // TODO: Is this useful?
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

        // We expect per prefix key, ignore if publication is still in old
        // format.
        if (1 != prefixDb.get_prefixEntries().size()) {
          XLOG(ERR)
              << "Expecting exactly one entry per prefix key, publication received from "
              << *prefixDb.thisNodeName_ref();
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
          XLOG(DBG2)
              << "Ignore self redistributed route reflection for prefix: "
              << key << " area_stack: " << folly::join(",", areaStack);
          continue;
        }

        // construct new prefix key with local publication area id
        PrefixKey prefixKey(
            prefixDb.get_thisNodeName(), toIPNetwork(entry.get_prefix()), area);

        // TODO: Is this useful?
        fb303::fbData->addStatValue(
            "decision.prefix_db_update", 1, fb303::COUNT);
        pendingUpdates_.applyPrefixStateChange(
            prefixDb.get_deletePrefix()
                ? prefixState_.deletePrefix(prefixKey)
                : prefixState_.updatePrefix(prefixKey, entry),
            prefixDb.perfEvents_ref());
      }
    } catch (const std::exception& e) {
      XLOG(ERR) << "Failed to deserialize info for key " << key
                << ". Exception: " << folly::exceptionStr(e);
    }
  }

  // LSDB deletion
  // TODO: avoid decoding from string by injecting data-structures
  // instead of raw strings into `expiredKeys` collection
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
      auto maybePrefixKey = PrefixKey::fromStr(key, area);
      if (maybePrefixKey.hasError()) {
        // this is bad format of key.
        XLOG(ERR) << fmt::format(
            "Unable to parse prefix key: {} with error: {}",
            key,
            maybePrefixKey.error());
        continue;
      }
      pendingUpdates_.applyPrefixStateChange(
          prefixState_.deletePrefix(maybePrefixKey.value()),
          thrift::PrefixDatabase().perfEvents_ref()); // Empty perf events
    }
  }
}

void
Decision::processStaticRoutesUpdate(DecisionRouteUpdate&& routeUpdate) {
  // update static unicast routes
  if (routeUpdate.unicastRoutesToUpdate.size() or
      routeUpdate.unicastRoutesToDelete.size()) {
    // store as local storage
    spfSolver_->updateStaticUnicastRoutes(
        routeUpdate.unicastRoutesToUpdate, routeUpdate.unicastRoutesToDelete);

    // Create set of changed prefixes
    std::unordered_set<folly::CIDRNetwork> changedPrefixes{
        routeUpdate.unicastRoutesToDelete.cbegin(),
        routeUpdate.unicastRoutesToDelete.cend()};
    for (const auto& [prefix, ribUnicastEntry] :
         routeUpdate.unicastRoutesToUpdate) {
      changedPrefixes.emplace(prefix);
    }

    // only apply prefix updates, no full DB rebuild
    pendingUpdates_.applyPrefixStateChange(
        std::move(changedPrefixes), thrift::PrefixDatabase().perfEvents_ref());
  }

  // update static MPLS routes
  if (routeUpdate.mplsRoutesToUpdate.size() or
      routeUpdate.mplsRoutesToDelete.size()) {
    spfSolver_->updateStaticMplsRoutes(
        routeUpdate.mplsRoutesToUpdate, routeUpdate.mplsRoutesToDelete);
    pendingUpdates_.setNeedsFullRebuild(); // Mark for full DB rebuild
  }
  rebuildRoutesDebounced_();

  auto prefixType = routeUpdate.prefixType;
  if (prefixType.has_value() and
      unreceivedRouteTypes_.erase(prefixType.value())) {
    // Received initial route of prefix type in OpenR initialization process.
    XLOG(INFO) << fmt::format(
        "[Initialization] Received {} static routes for prefix type {}.",
        routeUpdate.size(),
        apache::thrift::util::enumNameSafe<thrift::PrefixType>(
            prefixType.value()));
    triggerInitialBuildRoutes();
  }
}

void
Decision::rebuildRoutes(std::string const& event) {
  // If OpenR initialization procedure is enable, do not trigger initial route
  // computation until all conditions are fulfilled.
  // Otherwise, wait for coldStartTimer_ before initial route computation.
  if (config_->getConfig().get_enable_initialization_process()) {
    if (not unblockInitialRoutesBuild()) {
      return;
    }
  } else if (coldStartTimer_->isScheduled()) {
    return;
  }

  pendingUpdates_.addEvent(event);
  XLOG(DBG1) << "Decision: processing " << pendingUpdates_.getCount()
             << " accumulated updates. " << event;
  if (pendingUpdates_.perfEvents()) {
    if (auto expectedDuration = getDurationBetweenPerfEvents(
            *pendingUpdates_.perfEvents(),
            "DECISION_RECEIVED",
            "DECISION_DEBOUNCE")) {
      XLOG(DBG2) << "Debounced " << pendingUpdates_.getCount()
                 << " events over " << expectedDuration->count() << "ms.";
    }
  }

  DecisionRouteUpdate update;
  if (pendingUpdates_.needsFullRebuild()) {
    // if only static routes gets updated, we still need to update routes
    // because there maybe routes depended on static routes.
    auto maybeRouteDb =
        spfSolver_->buildRouteDb(myNodeName_, areaLinkStates_, prefixState_);
    XLOG_IF(WARNING, !maybeRouteDb)
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
    update.type = DecisionRouteUpdate::FULL_SYNC;
  } else {
    // process prefixes update from `prefixState_`
    for (auto const& prefix : pendingUpdates_.updatedPrefixes()) {
      if (auto maybeRibEntry = spfSolver_->createRouteForPrefixOrGetStaticRoute(
              myNodeName_, areaLinkStates_, prefixState_, prefix)) {
        update.addRouteToUpdate(std::move(maybeRibEntry).value());
      } else if (routeDb_.unicastRoutes.count(prefix) > 0) {
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

bool
Decision::unblockInitialRoutesBuild() {
  bool adjReceivedForPeers{true};
  if (config_->getConfig().get_enable_ordered_adj_publication()) {
    // Wait till receiving initial peers and dual directional adjacencies with
    // initial peers.
    adjReceivedForPeers = initialPeersReceivedBaton_.try_wait() and
        areaToPendingAdjacency_.empty();
  }
  // Initial routes build will be unblocked if all following conditions are
  // fulfilled,
  // - Received all types of static routes, aka, unreceivedRouteTypes_ is empty
  // - Received initial KvStore publication, aka, initialKvStoreSynced_ is true
  // - Read persisted Rib policy
  // - Received adjacency with initial live peers, aka, adjReceivedForPeers is
  //   true.
  return unreceivedRouteTypes_.empty() and initialKvStoreSynced_ and
      initialRibPolicyRead_ and adjReceivedForPeers;
}

void
Decision::triggerInitialBuildRoutes() {
  if (not config_->getConfig().get_enable_initialization_process()) {
    return;
  }

  if (not unblockInitialRoutesBuild()) {
    return;
  }

  // Trigger initial RIB computation, after receiving routes of all expected
  // prefix types and inital publications from KvStore.
  rebuildRoutesDebounced_.cancelScheduledTimeout();
  pendingUpdates_.setNeedsFullRebuild();
  rebuildRoutes("INITIALIZATION");
  logInitializationEvent("Decision", thrift::InitializationEvent::RIB_COMPUTED);
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
    XLOG(WARNING) << "Prefix " << folly::IPAddress::networkToString(prefix)
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
