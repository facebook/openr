/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Fib.h"

#include <fb303/ServiceData.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/MapUtil.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>

namespace fb303 = facebook::fb303;

namespace openr {

Fib::Fib(
    std::shared_ptr<const Config> config,
    int32_t thriftPort,
    std::chrono::seconds coldStartDuration,
    messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueue,
    messaging::RQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue,
    messaging::ReplicateQueue<thrift::RouteDatabaseDelta>& fibUpdatesQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue,
    const MonitorSubmitUrl& monitorSubmitUrl,
    KvStore* kvStore,
    fbzmq::Context& zmqContext)
    : myNodeName_(*config->getConfig().node_name_ref()),
      thriftPort_(thriftPort),
      expBackoff_(
          std::chrono::milliseconds(8), std::chrono::milliseconds(4096)),
      kvStore_(kvStore),
      fibUpdatesQueue_(fibUpdatesQueue),
      logSampleQueue_(logSampleQueue) {
  auto tConfig = config->getConfig();

  dryrun_ = config->getConfig().dryrun_ref().value_or(false);
  enableSegmentRouting_ =
      config->getConfig().enable_segment_routing_ref().value_or(false);
  enableOrderedFib_ =
      config->getConfig().enable_ordered_fib_programming_ref().value_or(false);

  syncRoutesTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    if (routeState_.hasRoutesFromDecision) {
      if (syncRouteDb()) {
        hasSyncedFib_ = true;
        expBackoff_.reportSuccess();
      } else {
        // Apply exponential backoff and schedule next run
        expBackoff_.reportError();
        syncRoutesTimer_->scheduleTimeout(
            expBackoff_.getTimeRemainingUntilRetry());
      }
    }
    fb303::fbData->setCounter(
        "fib.synced", syncRoutesTimer_->isScheduled() ? 0 : 1);
  });
  // On startup we do require routedb_sync so explicitly set the counter to 0
  fb303::fbData->setCounter("fib.synced", 0);

  if (enableOrderedFib_) {
    // check non-empty module ptr
    CHECK(kvStore_);
    kvStoreClient_ =
        std::make_unique<KvStoreClientInternal>(this, myNodeName_, kvStore_);
  }

  if (not tConfig.eor_time_s_ref()) {
    routeState_.hasRoutesFromDecision = true;
    syncRoutesTimer_->scheduleTimeout(coldStartDuration);
  }

  keepAliveTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    // Make thrift calls to do real programming
    try {
      keepAliveCheck();
    } catch (const std::exception& e) {
      fb303::fbData->addStatValue(
          "fib.thrift.failure.keepalive", 1, fb303::COUNT);
      client_.reset();
      LOG(ERROR) << "Failed to make thrift call to Switch Agent. Error: "
                 << folly::exceptionStr(e);
    }
    // schedule periodically
    keepAliveTimer_->scheduleTimeout(Constants::kKeepAliveCheckInterval);
  });

  // Only schedule health checker in non dry run mode
  if (not dryrun_) {
    keepAliveTimer_->scheduleTimeout(Constants::kKeepAliveCheckInterval);
  }

  // Fiber to process route updates from Decision
  addFiberTask([q = std::move(routeUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeThriftObj = q.get(); // perform read
      VLOG(1) << "Received route updates";
      if (maybeThriftObj.hasError()) {
        LOG(INFO) << "Terminating route delta processing fiber";
        break;
      }

      processRouteUpdates(std::move(maybeThriftObj).value().toThrift());
    }
  });

  // Fiber to process interface updates from LinkMonitor
  addFiberTask([q = std::move(interfaceUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeThriftObj = q.get(); // perform read
      VLOG(1) << "Received interface updates";
      if (maybeThriftObj.hasError()) {
        LOG(INFO) << "Terminating interface update processing fiber";
        break;
      }

      CHECK_EQ(myNodeName_, *maybeThriftObj.value().thisNodeName_ref());
      processInterfaceDb(std::move(maybeThriftObj).value());
    }
  });

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Initialize stats keys
  fb303::fbData->addStatExportType("fib.convergence_time_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "fib.local_route_program_time_ms", fb303::AVG);
  fb303::fbData->addStatExportType("fib.num_of_route_updates", fb303::SUM);
  fb303::fbData->addStatExportType("fib.process_interface_db", fb303::COUNT);
  fb303::fbData->addStatExportType("fib.process_route_db", fb303::COUNT);
  fb303::fbData->addStatExportType("fib.sync_fib_calls", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "fib.thrift.failure.add_del_route", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "fib.thrift.failure.keepalive", fb303::COUNT);
  fb303::fbData->addStatExportType("fib.thrift.failure.sync_fib", fb303::COUNT);
}

void
Fib::stop() {
  // Stop KvStoreClient first
  if (kvStoreClient_) {
    kvStoreClient_->stop();
  }

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

std::optional<thrift::IpPrefix>
Fib::longestPrefixMatch(
    const folly::CIDRNetwork& inputPrefix,
    const std::unordered_map<thrift::IpPrefix, thrift::UnicastRoute>&
        unicastRoutes) {
  std::optional<thrift::IpPrefix> matchedPrefix;
  int maxMask = -1;
  const auto& inputIP = inputPrefix.first;
  const auto& inputMask = inputPrefix.second;

  // longest prefix matching
  for (const auto& route : unicastRoutes) {
    const auto& dbIP = toIPAddress(*route.first.prefixAddress_ref());
    const auto& dbMask = *route.first.prefixLength_ref();

    if (maxMask < dbMask && inputMask >= dbMask &&
        inputIP.mask(dbMask) == dbIP) {
      maxMask = dbMask;
      matchedPrefix = route.first;
    }
  }
  return matchedPrefix;
}

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
Fib::getRouteDb() {
  folly::Promise<std::unique_ptr<thrift::RouteDatabase>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), this]() mutable {
    thrift::RouteDatabase routeDb;
    *routeDb.thisNodeName_ref() = myNodeName_;
    for (const auto& route : routeState_.unicastRoutes) {
      routeDb.unicastRoutes_ref()->emplace_back(route.second);
    }
    for (const auto& route : routeState_.mplsRoutes) {
      routeDb.mplsRoutes_ref()->emplace_back(route.second);
    }
    p.setValue(std::make_unique<thrift::RouteDatabase>(std::move(routeDb)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
Fib::getUnicastRoutes(std::vector<std::string> prefixes) {
  folly::Promise<std::unique_ptr<std::vector<thrift::UnicastRoute>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [p = std::move(p), prefixes = std::move(prefixes), this]() mutable {
        p.setValue(std::make_unique<std::vector<thrift::UnicastRoute>>(
            getUnicastRoutesFiltered(std::move(prefixes))));
      });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::MplsRoute>>>
Fib::getMplsRoutes(std::vector<int32_t> labels) {
  folly::Promise<std::unique_ptr<std::vector<thrift::MplsRoute>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [p = std::move(p), labels = std::move(labels), this]() mutable {
        p.setValue(std::make_unique<std::vector<thrift::MplsRoute>>(
            getMplsRoutesFiltered(std::move(labels))));
      });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::PerfDatabase>>
Fib::getPerfDb() {
  folly::Promise<std::unique_ptr<thrift::PerfDatabase>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), this]() mutable {
    p.setValue(std::make_unique<thrift::PerfDatabase>(dumpPerfDb()));
  });
  return sf;
}

std::vector<thrift::UnicastRoute>
Fib::getUnicastRoutesFiltered(std::vector<std::string> prefixes) {
  // return and send the vector<thrift::UnicastRoute>
  std::vector<thrift::UnicastRoute> retRouteVec;
  // the matched prefix after longest prefix matching and avoid duplicates
  std::set<thrift::IpPrefix> matchPrefixSet;

  // if the params is empty, return all routes
  if (prefixes.empty()) {
    for (const auto& routes : routeState_.unicastRoutes) {
      retRouteVec.emplace_back(routes.second);
    }
    return retRouteVec;
  }

  // longest prefix matching for each input string
  for (const auto& prefixStr : prefixes) {
    // try to convert the string prefix into CIDRNetwork
    const auto maybePrefix =
        folly::IPAddress::tryCreateNetwork(prefixStr, -1, true);
    if (maybePrefix.hasError()) {
      LOG(ERROR) << "Invalid IP address as prefix: " << prefixStr;
      return retRouteVec;
    }
    const auto inputPrefix = maybePrefix.value();

    // do longest prefix match, add the matched prefix to the result set
    const auto& matchedPrefix =
        Fib::longestPrefixMatch(inputPrefix, routeState_.unicastRoutes);
    if (matchedPrefix.has_value()) {
      matchPrefixSet.insert(matchedPrefix.value());
    }
  }

  // get the routes from the prefix set
  for (const auto& prefix : matchPrefixSet) {
    retRouteVec.emplace_back(routeState_.unicastRoutes.at(prefix));
  }

  return retRouteVec;
}

std::vector<thrift::MplsRoute>
Fib::getMplsRoutesFiltered(std::vector<int32_t> labels) {
  // return and send the vector<thrift::MplsRoute>
  std::vector<thrift::MplsRoute> retRouteVec;

  // if the params is empty, return all MPLS routes
  if (labels.empty()) {
    for (const auto& routes : routeState_.mplsRoutes) {
      retRouteVec.emplace_back(routes.second);
    }
    return retRouteVec;
  }

  // get the params: list of MPLS label filters -> set of MPLS label filters
  std::set<int32_t> labelFilterSet;
  for (const auto& label : labels) {
    labelFilterSet.insert(label);
  }

  // get the filtered MPLS routes and avoid duplicates
  for (const auto& routes : routeState_.mplsRoutes) {
    if (labelFilterSet.find(routes.first) != labelFilterSet.end()) {
      retRouteVec.emplace_back(routes.second);
    }
  }

  return retRouteVec;
}

messaging::RQueue<thrift::RouteDatabaseDelta>
Fib::getFibUpdatesReader() {
  return fibUpdatesQueue_.getReader();
}

void
Fib::processRouteUpdates(thrift::RouteDatabaseDelta&& routeDelta) {
  routeState_.hasRoutesFromDecision = true;
  // Update perfEvents_ .. We replace existing perf events with new one as
  // convergence is going to be based on new data, not the old.
  if (routeDelta.perfEvents_ref()) {
    addPerfEvent(
        *routeDelta.perfEvents_ref(), myNodeName_, "FIB_ROUTE_DB_RECVD");
  }

  // Before anything, get rid of doNotInstall routes
  auto i = routeDelta.unicastRoutesToUpdate_ref()->begin();
  while (i != routeDelta.unicastRoutesToUpdate_ref()->end()) {
    if (*i->doNotInstall_ref()) {
      LOG(INFO) << "Not installing route for prefix " << toString(i->dest);
      i = routeDelta.unicastRoutesToUpdate_ref()->erase(i);
    } else {
      ++i;
    }
  }

  // Add/Update unicast routes to update
  for (const auto& route : *routeDelta.unicastRoutesToUpdate_ref()) {
    routeState_.unicastRoutes[route.dest] = route;
    routeState_.dirtyPrefixes.erase(route.dest);
  }

  // Add mpls routes to update
  for (const auto& route : *routeDelta.mplsRoutesToUpdate_ref()) {
    routeState_.mplsRoutes[route.topLabel] = route;
    routeState_.dirtyLabels.erase(route.topLabel);
  }

  // Delete unicast routes
  for (const auto& dest : *routeDelta.unicastRoutesToDelete_ref()) {
    routeState_.unicastRoutes.erase(dest);
    routeState_.dirtyPrefixes.erase(dest);
  }

  // Delete mpls routes
  for (const auto& topLabel : *routeDelta.mplsRoutesToDelete_ref()) {
    routeState_.mplsRoutes.erase(topLabel);
    routeState_.dirtyLabels.erase(topLabel);
  }

  // Add some counters
  fb303::fbData->addStatValue("fib.process_route_db", 1, fb303::COUNT);
  // Send request to agent
  updateRoutes(routeDelta);
}

void
Fib::processInterfaceDb(thrift::InterfaceDatabase&& interfaceDb) {
  fb303::fbData->addStatValue("fib.process_interface_db", 1, fb303::COUNT);

  if (interfaceDb.perfEvents_ref()) {
    addPerfEvent(
        *interfaceDb.perfEvents_ref(), myNodeName_, "FIB_INTF_DB_RECEIVED");
  }

  //
  // Update interface states
  //
  for (auto const& kv : *interfaceDb.interfaces_ref()) {
    const auto& ifName = kv.first;
    const auto isUp = kv.second.isUp;
    const auto wasUp = folly::get_default(interfaceStatusDb_, ifName, false);

    // UP -> DOWN transition
    if (wasUp and not isUp) {
      LOG(INFO) << "Interface " << ifName << " transitioned from UP -> DOWN";
    }
    // DOWN -> UP transition
    if (not wasUp and isUp) {
      LOG(INFO) << "Interface " << ifName << " transitioned from DOWN -> UP";
    }

    // Update new status
    interfaceStatusDb_[ifName] = isUp;
  }

  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.perfEvents_ref().move_from(interfaceDb.perfEvents_ref());

  //
  // Compute unicast route changes
  //
  for (auto const& kv : routeState_.unicastRoutes) {
    auto const& route = kv.second;

    // Find previous best nexthops
    const auto& prevNextHops = *route.nextHops_ref();

    // Find valid nexthops for route
    std::vector<thrift::NextHopThrift> validNextHops;
    for (auto const& nextHop : *route.nextHops_ref()) {
      const auto ifName = nextHop.address_ref()->ifName_ref();
      if (not ifName.has_value() ||
          (folly::get_default(interfaceStatusDb_, *ifName, false))) {
        validNextHops.emplace_back(nextHop);
      }
    } // end for ... kv.second

    // Remove route if no valid nexthops
    if (not validNextHops.size()) {
      VLOG(1) << "Removing prefix " << toString(route.dest)
              << " because of no valid nextHops.";
      routeDbDelta.unicastRoutesToDelete_ref()->emplace_back(*route.dest_ref());
      routeState_.dirtyPrefixes.emplace(route.dest); // Mark prefix as dirty
      continue; // Skip rest
    }

    if (validNextHops != prevNextHops) {
      // Nexthop group shrink
      VLOG(1) << "bestPaths group resize for prefix: " << toString(route.dest)
              << ", old: " << prevNextHops.size()
              << ", new: " << validNextHops.size();
      thrift::UnicastRoute newRoute;
      newRoute.dest = route.dest;
      *newRoute.nextHops_ref() = std::move(validNextHops);
      routeDbDelta.unicastRoutesToUpdate_ref()->emplace_back(
          std::move(newRoute));
      routeState_.dirtyPrefixes.emplace(route.dest); // Mark prefix as dirty
    } else if (routeState_.dirtyPrefixes.count(route.dest)) {
      // Nexthop group restore - previously best
      routeDbDelta.unicastRoutesToUpdate_ref()->emplace_back(route);
      routeState_.dirtyPrefixes.erase(route.dest); // Remove from dirty list
    }
  } // end for ... routeDb_.unicastRoutes

  //
  // Compute MPLS route changes
  //
  for (const auto& kv : routeState_.mplsRoutes) {
    const auto& route = kv.second;

    // Find valid nexthops for route
    std::vector<thrift::NextHopThrift> validNextHops;
    for (auto const& nextHop : *route.nextHops_ref()) {
      // We don't have ifName for `POP_AND_LOOKUP` mpls action
      auto const ifName = nextHop.address_ref()->ifName_ref();
      if (not ifName.has_value() or
          folly::get_default(interfaceStatusDb_, *ifName, false)) {
        validNextHops.emplace_back(nextHop);
      }
    }

    // Find previous best nexthops
    auto prevBestNextHops = selectMplsNextHops(*route.nextHops_ref());

    // Find new valid best nexthops
    auto validBestNextHops = selectMplsNextHops(validNextHops);

    // Remove route if no valid nexthops
    if (not validBestNextHops.size()) {
      VLOG(1) << "Removing label route " << *route.topLabel_ref()
              << " because of no valid nextHops.";
      routeDbDelta.mplsRoutesToDelete_ref()->emplace_back(
          *route.topLabel_ref());
      routeState_.dirtyLabels.emplace(*route.topLabel_ref()); // Mark prefix as
                                                              // dirty
      continue; // Skip rest
    }

    if (validBestNextHops != prevBestNextHops) {
      // Nexthop group shrink
      VLOG(1) << "bestPaths group resize for label: " << route.topLabel
              << ", old: " << prevBestNextHops.size()
              << ", new: " << validBestNextHops.size();
      thrift::MplsRoute newRoute;
      newRoute.topLabel = route.topLabel;
      *newRoute.nextHops_ref() = std::move(validBestNextHops);
      routeDbDelta.mplsRoutesToUpdate_ref()->emplace_back(std::move(newRoute));
      routeState_.dirtyLabels.emplace(route.topLabel);
    } else if (routeState_.dirtyLabels.count(route.topLabel)) {
      // Nexthop group restore - previously best
      routeDbDelta.mplsRoutesToUpdate_ref()->emplace_back(route);
      routeState_.dirtyLabels.erase(route.topLabel); // Remove from dirty list
    }
  } // end for ... routeDb_.mplsRoutes

  updateRoutes(routeDbDelta);
}

thrift::PerfDatabase
Fib::dumpPerfDb() const {
  thrift::PerfDatabase perfDb;
  *perfDb.thisNodeName_ref() = myNodeName_;
  for (auto const& perf : perfDb_) {
    perfDb.eventInfo_ref()->emplace_back(perf);
  }
  return perfDb;
}

void
Fib::updateRoutes(const thrift::RouteDatabaseDelta& routeDbDelta) {
  SCOPE_EXIT {
    updateRoutesSemaphore_.signal(); // Release when this function returns
  };
  updateRoutesSemaphore_.wait();

  LOG(INFO) << "Processing route add/update for "
            << routeDbDelta.unicastRoutesToUpdate_ref()->size() << " unicast, "
            << routeDbDelta.mplsRoutesToUpdate_ref()->size() << " mpls, "
            << "and route delete for "
            << routeDbDelta.unicastRoutesToDelete_ref()->size() << "-unicast, "
            << routeDbDelta.mplsRoutesToDelete_ref()->size() << "-mpls, ";

  // update flat counters here as they depend on routeState_ and its change
  updateGlobalCounters();

  // Only for backward compatibility
  auto const& unicastRoutesToUpdate = *routeDbDelta.unicastRoutesToUpdate_ref();

  auto const& mplsRoutesToUpdate = createMplsRoutesWithSelectedNextHops(
      *routeDbDelta.mplsRoutesToUpdate_ref());

  VLOG(2) << "Unicast routes to add/update";
  for (auto const& route : unicastRoutesToUpdate) {
    VLOG(2) << "> " << toString(*route.dest_ref()) << ", "
            << route.nextHops_ref()->size();
    for (auto const& nh : *route.nextHops_ref()) {
      VLOG(2) << "  " << toString(nh);
    }
  }

  VLOG(2) << "";
  VLOG(2) << "Unicast routes to delete";
  for (auto const& prefix : *routeDbDelta.unicastRoutesToDelete_ref()) {
    VLOG(2) << "> " << toString(prefix);
  }

  VLOG(2) << "";
  VLOG(2) << "Mpls routes to add/update";
  for (auto const& route : mplsRoutesToUpdate) {
    VLOG(2) << "> " << std::to_string(route.topLabel) << ", "
            << route.nextHops_ref()->size();
    for (auto const& nh : *route.nextHops_ref()) {
      VLOG(2) << "  " << toString(nh);
    }
  }

  VLOG(2) << "";
  VLOG(2) << "MPLS routes to delete";
  for (auto const& topLabel : *routeDbDelta.mplsRoutesToDelete_ref()) {
    VLOG(2) << "> " << std::to_string(topLabel);
  }

  if (dryrun_) {
    // Do not program routes in case of dryrun
    LOG(INFO) << "Skipping programming of routes in dryrun ... ";
    logPerfEvents(castToStd(routeDbDelta.perfEvents_ref()));
    return;
  }

  // Publish the fib streaming routes after considering donotinstall
  // and dryrun logic.
  if (not(*routeDbDelta.unicastRoutesToUpdate_ref()).empty() ||
      not(*routeDbDelta.unicastRoutesToDelete_ref()).empty() ||
      not(*routeDbDelta.mplsRoutesToUpdate_ref()).empty() ||
      not(*routeDbDelta.mplsRoutesToDelete_ref()).empty()) {
    // Due to donotinstall logic it's possible to have emtpy change,
    // no need to publish empty updates.
    fibUpdatesQueue_.push(routeDbDelta);
  }

  if (syncRoutesTimer_->isScheduled()) {
    // Check if there's any full sync scheduled,
    // if so, skip partial sync
    LOG(INFO) << "Pending full sync is scheduled, skip delta sync for now...";
    return;
  } else if (routeState_.dirtyRouteDb or not hasSyncedFib_) {
    if (hasSyncedFib_) {
      LOG(INFO) << "Previous route programming failed or, skip delta sync to "
                << "enforce full fib sync...";
    } else {
      LOG(INFO) << "Syncing fib on startup...";
    }
    syncRouteDbDebounced();
    return;
  }

  // Make thrift calls to do real programming
  try {
    uint32_t numOfRouteUpdates = 0;
    createFibClient(evb_, socket_, client_, thriftPort_);
    if (routeDbDelta.unicastRoutesToDelete_ref()->size()) {
      numOfRouteUpdates += routeDbDelta.unicastRoutesToDelete_ref()->size();
      client_->sync_deleteUnicastRoutes(
          kFibId_, *routeDbDelta.unicastRoutesToDelete_ref());
    }
    if (unicastRoutesToUpdate.size()) {
      numOfRouteUpdates += unicastRoutesToUpdate.size();
      client_->sync_addUnicastRoutes(kFibId_, unicastRoutesToUpdate);
    }
    if (enableSegmentRouting_ &&
        routeDbDelta.mplsRoutesToDelete_ref()->size()) {
      numOfRouteUpdates += routeDbDelta.mplsRoutesToDelete_ref()->size();
      client_->sync_deleteMplsRoutes(
          kFibId_, *routeDbDelta.mplsRoutesToDelete_ref());
    }
    if (enableSegmentRouting_ && mplsRoutesToUpdate.size()) {
      numOfRouteUpdates += mplsRoutesToUpdate.size();
      client_->sync_addMplsRoutes(kFibId_, mplsRoutesToUpdate);
    }
    fb303::fbData->addStatValue(
        "fib.num_of_route_updates", numOfRouteUpdates, fb303::SUM);
    routeState_.dirtyRouteDb = false;
    logPerfEvents(castToStd(routeDbDelta.perfEvents_ref()));
    LOG(INFO) << "Done processing route add/update";
  } catch (const std::exception& e) {
    fb303::fbData->addStatValue(
        "fib.thrift.failure.add_del_route", 1, fb303::COUNT);
    client_.reset();
    routeState_.dirtyRouteDb = true;
    syncRouteDbDebounced(); // Schedule future full sync of route DB
    LOG(ERROR) << "Failed to make thrift call to FibAgent. Error: "
               << folly::exceptionStr(e);
  }
}

bool
Fib::syncRouteDb() {
  LOG(INFO) << "Syncing latest routeDb with fib-agent with "
            << routeState_.unicastRoutes.size() << " routes";

  const auto& unicastRoutes =
      createUnicastRoutesFromMap(routeState_.unicastRoutes);
  const auto& mplsRoutes =
      createMplsRoutesWithSelectedNextHopsMap(routeState_.mplsRoutes);

  // In dry run we just print the routes. No real action
  if (dryrun_) {
    LOG(INFO) << "Skipping programming of routes in dryrun ... ";
    VLOG(2) << "Unicast routes to add/update";
    for (auto const& route : unicastRoutes) {
      VLOG(2) << "> " << toString(*route.dest_ref()) << ", "
              << route.nextHops_ref()->size();
      for (auto const& nh : *route.nextHops_ref()) {
        VLOG(2) << "  " << toString(nh);
      }
    }

    VLOG(2) << "";
    VLOG(2) << "Mpls routes to add/update";
    for (auto const& route : mplsRoutes) {
      VLOG(2) << "> " << std::to_string(route.topLabel) << ", "
              << route.nextHops_ref()->size();
      for (auto const& nh : *route.nextHops_ref()) {
        VLOG(2) << "  " << toString(nh);
      }
    }

    return true;
  }

  try {
    createFibClient(evb_, socket_, client_, thriftPort_);
    fb303::fbData->addStatValue("fib.sync_fib_calls", 1, fb303::COUNT);

    // Sync unicast routes
    client_->sync_syncFib(kFibId_, unicastRoutes);
    routeState_.dirtyPrefixes.clear();

    // Sync mpls routes
    if (enableSegmentRouting_) {
      client_->sync_syncMplsFib(kFibId_, mplsRoutes);
    }
    routeState_.dirtyLabels.clear();

    routeState_.dirtyRouteDb = false;
    LOG(INFO) << "Done syncing latest routeDb with fib-agent";
    return true;
  } catch (std::exception const& e) {
    fb303::fbData->addStatValue("fib.thrift.failure.sync_fib", 1, fb303::COUNT);
    LOG(ERROR) << "Failed to sync routeDb with switch FIB agent. Error: "
               << folly::exceptionStr(e);
    routeState_.dirtyRouteDb = true;
    client_.reset();
    return false;
  }
}

void
Fib::syncRouteDbDebounced() {
  if (!syncRoutesTimer_->isScheduled()) {
    // Schedule an immediate run if previous one is not scheduled
    syncRoutesTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

void
Fib::keepAliveCheck() {
  createFibClient(evb_, socket_, client_, thriftPort_);
  int64_t aliveSince = client_->sync_aliveSince();
  // Check if FIB has restarted or not
  if (aliveSince != latestAliveSince_) {
    LOG(WARNING) << "FibAgent seems to have restarted. "
                 << "Performing full route DB sync ...";
    // set dirty flag
    routeState_.dirtyRouteDb = true;
    expBackoff_.reportSuccess();
    syncRouteDbDebounced();
  }
  latestAliveSince_ = aliveSince;
}

void
Fib::createFibClient(
    folly::EventBase& evb,
    std::shared_ptr<folly::AsyncSocket>& socket,
    std::unique_ptr<thrift::FibServiceAsyncClient>& client,
    int32_t port) {
  // Reset client if channel is not good
  if (socket && (!socket->good() || socket->hangup())) {
    client.reset();
    socket.reset();
  }

  // Do not create new client if one exists already
  if (client) {
    return;
  }

  // Create socket to thrift server and set some connection parameters
  socket = folly::AsyncSocket::newSocket(
      &evb,
      Constants::kPlatformHost.toString(),
      port,
      Constants::kPlatformConnTimeout.count());

  // Create channel and set timeout
  auto channel = apache::thrift::HeaderClientChannel::newChannel(socket);
  channel->setTimeout(Constants::kPlatformRoutesProcTimeout.count());

  // Set BinaryProtocol and Framed client type for talkiing with thrift1 server
  channel->setProtocolId(apache::thrift::protocol::T_BINARY_PROTOCOL);
  channel->setClientType(THRIFT_FRAMED_DEPRECATED);

  // Reset client_
  client = std::make_unique<thrift::FibServiceAsyncClient>(std::move(channel));
}

void
Fib::updateGlobalCounters() {
  // Set some flat counters
  fb303::fbData->setCounter(
      "fib.num_routes",
      routeState_.unicastRoutes.size() + routeState_.mplsRoutes.size());
  fb303::fbData->setCounter(
      "fib.num_unicast_routes", routeState_.unicastRoutes.size());
  fb303::fbData->setCounter(
      "fib.num_mpls_routes", routeState_.mplsRoutes.size());
  fb303::fbData->setCounter(
      "fib.num_dirty_prefixes", routeState_.dirtyPrefixes.size());
  fb303::fbData->setCounter(
      "fib.num_dirty_labels", routeState_.dirtyLabels.size());

  // Count the number of bgp routes
  int64_t bgpCounter = 0;
  for (const auto& route : routeState_.unicastRoutes) {
    if (route.second.bestNexthop_ref().has_value()) {
      bgpCounter++;
    }
  }
  fb303::fbData->setCounter("fib.num_routes.BGP", bgpCounter);
}

void
Fib::logPerfEvents(std::optional<thrift::PerfEvents> perfEvents) {
  if (not perfEvents.has_value() or not perfEvents->events_ref()->size()) {
    return;
  }

  // Ignore bad perf event sample if creation time of first event is
  // less than creation time of our recently logged perf events.
  if (recentPerfEventCreateTs_ >= *perfEvents->events_ref()[0].unixTs_ref()) {
    LOG(WARNING) << "Ignoring perf event with old create timestamp "
                 << *perfEvents->events_ref()[0].unixTs_ref() << ", expected > "
                 << recentPerfEventCreateTs_;
    return;
  } else {
    recentPerfEventCreateTs_ = *perfEvents->events_ref()[0].unixTs_ref();
  }

  // Add latest event information (this function is meant to be called after
  // routeDb has synced)
  addPerfEvent(*perfEvents, myNodeName_, "OPENR_FIB_ROUTES_PROGRAMMED");

  if (enableOrderedFib_) {
    // Export convergence duration counter
    // this is the local time it takes to program a route after the hold expired
    // we are using this for ordered fib programming
    auto localDuration = getDurationBetweenPerfEvents(
        *perfEvents,
        "ORDERED_FIB_HOLDS_EXPIRED",
        "OPENR_FIB_ROUTES_PROGRAMMED");
    if (localDuration.hasError()) {
      LOG(WARNING) << "Ignoring perf event with bad local duration "
                   << localDuration.error();
    } else if (*localDuration <= Constants::kConvergenceMaxDuration) {
      fb303::fbData->addStatValue(
          "fib.local_route_program_time_ms",
          localDuration->count(),
          fb303::AVG);
      kvStoreClient_->persistKey(
          Constants::kFibTimeMarker.toString() + myNodeName_,
          std::to_string(fb303::fbData->getCounters().at(
              "fib.local_route_program_time_ms.avg.60")),
          Constants::kTtlInfInterval,
          openr::thrift::KvStore_constants::kDefaultArea());
    }
  }

  // Ignore perf events with very off total duration
  auto totalDuration = getTotalPerfEventsDuration(*perfEvents);
  if (totalDuration.count() < 0 or
      totalDuration > Constants::kConvergenceMaxDuration) {
    LOG(WARNING) << "Ignoring perf event with bad total duration "
                 << totalDuration.count() << "ms.";
    return;
  }

  // Log event
  auto eventStrs = sprintPerfEvents(*perfEvents);
  LOG(INFO) << "OpenR convergence performance. "
            << "Duration=" << totalDuration.count();
  for (auto& str : eventStrs) {
    VLOG(2) << "  " << str;
  }

  // Add new entry to perf DB and purge extra entries
  perfDb_.push_back(std::move(perfEvents).value());
  while (perfDb_.size() >= Constants::kPerfBufferSize) {
    perfDb_.pop_front();
  }

  // Export convergence duration counter
  fb303::fbData->addStatValue(
      "fib.convergence_time_ms", totalDuration.count(), fb303::AVG);

  // Log via zmq monitor
  LogSample sample{};
  sample.addString("event", "ROUTE_CONVERGENCE");
  sample.addStringVector("perf_events", eventStrs);
  sample.addInt("duration_ms", totalDuration.count());
  logSampleQueue_.push(sample);
}

} // namespace openr
