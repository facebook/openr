/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <algorithm>
#include <exception>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/fib/Fib.h>

namespace fb303 = facebook::fb303;

namespace openr {

Fib::Fib(
    std::shared_ptr<const Config> config,
    int32_t thriftPort,
    std::chrono::seconds coldStartDuration,
    messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> staticRouteUpdatesQueue,
    messaging::ReplicateQueue<DecisionRouteUpdate>& fibRouteUpdatesQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue)
    : myNodeName_(*config->getConfig().node_name_ref()),
      thriftPort_(thriftPort),
      syncRoutesExpBackoff_(
          Constants::kFibInitialBackoff, Constants::kFibMaxBackoff, false),
      syncStaticRoutesExpBackoff_(
          Constants::kFibInitialBackoff, Constants::kFibMaxBackoff, false),
      fibRouteUpdatesQueue_(fibRouteUpdatesQueue),
      logSampleQueue_(logSampleQueue) {
  auto& tConfig = config->getConfig();

  dryrun_ = tConfig.dryrun_ref().value_or(false);
  enableSegmentRouting_ = tConfig.enable_segment_routing_ref().value_or(false);

  syncRoutesTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    if (routeState_.hasRoutesFromDecision) {
      if (syncRouteDb()) {
        hasSyncedFib_ = true;
        syncRoutesExpBackoff_.reportSuccess();
      } else {
        // Apply exponential backoff and schedule next run
        syncRoutesExpBackoff_.reportError();
        const auto period = syncRoutesExpBackoff_.getTimeRemainingUntilRetry();
        LOG(INFO) << "Scheduling fib sync after " << period.count() << "ms";
        syncRoutesTimer_->scheduleTimeout(period);
      }
    }
    fb303::fbData->setCounter(
        "fib.synced", syncRoutesTimer_->isScheduled() ? 0 : 1);
  });
  // On startup we do require routedb_sync so explicitly set the counter to 0
  fb303::fbData->setCounter("fib.synced", 0);

  if (not tConfig.eor_time_s_ref()) {
    routeState_.hasRoutesFromDecision = true;
    LOG(INFO)
        << "EOR time is not configured; schedule fib sync of routeDb with cold-start duration "
        << coldStartDuration.count() << "secs";
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
      if (maybeThriftObj.hasError()) {
        VLOG(1) << "Terminating route delta processing fiber";
        break;
      }
      processRouteUpdates(std::move(maybeThriftObj).value());
    }
  });

  syncStaticRoutesTimer_ =
      folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
        if (routeState_.hasRoutesFromDecision or
            not routeState_.hasStaticMplsRoutes) {
          LOG(INFO) << "Terminating syncStaticRoutesTimer_.";
          return;
        }

        DecisionRouteUpdate allMplsRoutes;
        std::transform(
            routeState_.mplsRoutes.cbegin(),
            routeState_.mplsRoutes.cend(),
            std::back_inserter(allMplsRoutes.mplsRoutesToUpdate),
            [](auto& iter) { return iter.second; });

        if (updateRoutes(std::move(allMplsRoutes), true /* static routes */)) {
          syncStaticRoutesExpBackoff_.reportSuccess();
        } else {
          // Apply exponential backoff and schedule next run
          syncStaticRoutesExpBackoff_.reportError();
          const auto period =
              syncStaticRoutesExpBackoff_.getTimeRemainingUntilRetry();
          LOG(INFO) << "Scheduling re-programming static MPLS routes after "
                    << period.count() << "ms";
          syncStaticRoutesTimer_->scheduleTimeout(period);
        }
      });

  // Fiber to process and program static route updates.
  // - The routes are only programmed and updated but not deleted
  // - Updates arriving before first Decision RIB update will be processed. The
  //   fiber will terminate after that.
  addFiberTask([q = std::move(staticRouteUpdatesQueue),
                this]() mutable noexcept {
    LOG(INFO) << "Starting static routes update processing fiber";
    while (true) {
      auto maybeThriftPub = q.get(); // perform read

      // Terminate if queue is closed or we've received RIB from Decision
      if (maybeThriftPub.hasError() or routeState_.hasRoutesFromDecision) {
        LOG(INFO) << "Terminating static routes update processing fiber";
        break;
      }

      // NOTE: We only process the static MPLS routes to add or update
      LOG(INFO) << "Received static routes update";
      maybeThriftPub->unicastRoutesToUpdate.clear();
      maybeThriftPub->unicastRoutesToDelete.clear();
      maybeThriftPub->mplsRoutesToDelete.clear();

      // Backup static MPLS routes. In case of update Routes failed, later
      // scheduled syncStaticRoutesTimer_ will retry programming the routes.
      routeState_.hasStaticMplsRoutes = true;
      routeState_.update(maybeThriftPub.value());

      // Program received static route updates
      if (not updateRoutes(
              std::move(maybeThriftPub).value(), true /* static routes */)) {
        // If failed, trigger syncStaticRoutesTimer_ to retry programming of
        // static MPLS routes.
        syncStaticRoutesTimer_->scheduleTimeout(Constants::kFibInitialBackoff);
      }
    }
  });

  // Initialize stats keys
  fb303::fbData->addStatExportType("fib.convergence_time_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "fib.local_route_program_time_ms", fb303::AVG);
  fb303::fbData->addStatExportType("fib.num_of_route_updates", fb303::SUM);
  fb303::fbData->addStatExportType("fib.process_route_db", fb303::COUNT);
  fb303::fbData->addStatExportType("fib.sync_fib_calls", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "fib.thrift.failure.add_del_route", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "fib.thrift.failure.keepalive", fb303::COUNT);
  fb303::fbData->addStatExportType("fib.thrift.failure.sync_fib", fb303::COUNT);
  fb303::fbData->addStatExportType("fib.route_programming.time_ms", fb303::AVG);
  fb303::fbData->addStatExportType("fib.route_sync.time_ms", fb303::AVG);
}

void
Fib::stop() {
  // Invoke stop method of super class
  OpenrEventBase::stop();
  VLOG(1) << "Stopped FIB event base";
}

std::optional<folly::CIDRNetwork>
Fib::longestPrefixMatch(
    const folly::CIDRNetwork& inputPrefix,
    const std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>&
        unicastRoutes) {
  std::optional<folly::CIDRNetwork> matchedPrefix;
  int maxMask = -1;
  const auto& inputIP = inputPrefix.first;
  const auto& inputMask = inputPrefix.second;

  // longest prefix matching
  for (const auto& route : unicastRoutes) {
    const auto& dbIP = route.first.first;
    const auto& dbMask = route.first.second;

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
      routeDb.unicastRoutes_ref()->emplace_back(route.second.toThrift());
    }
    for (const auto& route : routeState_.mplsRoutes) {
      routeDb.mplsRoutes_ref()->emplace_back(route.second.toThrift());
    }
    p.setValue(std::make_unique<thrift::RouteDatabase>(std::move(routeDb)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabaseDetail>>
Fib::getRouteDetailDb() {
  folly::Promise<std::unique_ptr<thrift::RouteDatabaseDetail>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), this]() mutable {
    thrift::RouteDatabaseDetail routeDetailDb;
    *routeDetailDb.thisNodeName_ref() = myNodeName_;
    for (const auto& route : routeState_.unicastRoutes) {
      routeDetailDb.unicastRoutes_ref()->emplace_back(
          route.second.toThriftDetail());
    }
    for (const auto& route : routeState_.mplsRoutes) {
      routeDetailDb.mplsRoutes_ref()->emplace_back(
          route.second.toThriftDetail());
    }
    p.setValue(std::make_unique<thrift::RouteDatabaseDetail>(
        std::move(routeDetailDb)));
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
  std::set<folly::CIDRNetwork> matchPrefixSet;

  // if the params is empty, return all routes
  if (prefixes.empty()) {
    for (const auto& routes : routeState_.unicastRoutes) {
      retRouteVec.emplace_back(routes.second.toThrift());
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
    retRouteVec.emplace_back(routeState_.unicastRoutes.at(prefix).toThrift());
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
      retRouteVec.emplace_back(routes.second.toThrift());
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
      retRouteVec.emplace_back(routes.second.toThrift());
    }
  }

  return retRouteVec;
}

messaging::RQueue<DecisionRouteUpdate>
Fib::getFibUpdatesReader() {
  return fibRouteUpdatesQueue_.getReader();
}

void
Fib::RouteState::update(const DecisionRouteUpdate& routeUpdate) {
  // Add/Update unicast routes to update
  for (const auto& [prefix, route] : routeUpdate.unicastRoutesToUpdate) {
    auto it = unicastRoutes.find(prefix);
    if (it != unicastRoutes.end()) {
      it->second = route;
    } else {
      unicastRoutes.emplace(prefix, route);
    }
  }

  // Add mpls routes to update
  for (const auto& route : routeUpdate.mplsRoutesToUpdate) {
    auto it = mplsRoutes.find(route.label);
    if (it != mplsRoutes.end()) {
      it->second = route;
    } else {
      mplsRoutes.emplace(route.label, route);
    }
  }

  // Delete unicast routes
  for (const auto& dest : routeUpdate.unicastRoutesToDelete) {
    unicastRoutes.erase(dest);
  }

  // Delete mpls routes
  for (const auto& topLabel : routeUpdate.mplsRoutesToDelete) {
    mplsRoutes.erase(topLabel);
  }
}

// Process new route updates received from Decision module.
void
Fib::processRouteUpdates(DecisionRouteUpdate&& routeUpdate) {
  routeState_.hasRoutesFromDecision = true;

  // Update perfEvents_ .. We replace existing perf events with new one as
  // convergence is going to be based on new data, not the old.
  if (routeUpdate.perfEvents.has_value()) {
    addPerfEvent(
        routeUpdate.perfEvents.value(), myNodeName_, "FIB_ROUTE_DB_RECVD");
  }

  // Before anything, get rid of doNotInstall routes
  auto iter = routeUpdate.unicastRoutesToUpdate.cbegin();
  while (iter != routeUpdate.unicastRoutesToUpdate.cend()) {
    if (iter->second.doNotInstall) {
      LOG(INFO) << "Not installing route for prefix "
                << folly::IPAddress::networkToString(iter->first);
      iter = routeUpdate.unicastRoutesToUpdate.erase(iter);
    } else {
      ++iter;
    }
  }

  // Backup routes in routeState_. In case update routes failed, routes will be
  // programmed in later scheduled FIB sync.
  routeState_.update(routeUpdate);

  // Add some counters
  fb303::fbData->addStatValue("fib.process_route_db", 1, fb303::COUNT);

  if (updateRoutes(std::move(routeUpdate), false /* static routes */)) {
    routeState_.dirtyRouteDb = false;
  } else {
    routeState_.dirtyRouteDb = true;
    syncRouteDbDebounced(); // Schedule future full sync of route DB
  }
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
Fib::printUnicastRoutesAddUpdate(
    const std::vector<thrift::UnicastRoute>& unicastRoutesToUpdate) {
  if (not unicastRoutesToUpdate.size()) {
    return;
  }

  for (auto const& route : unicastRoutesToUpdate) {
    VLOG(1) << "> " << toString(*route.dest_ref())
            << ", NextHopsCount = " << route.nextHops_ref()->size();
    for (auto const& nh : *route.nextHops_ref()) {
      VLOG(1) << " " << toString(nh);
    }
  }
}

void
Fib::printMplsRoutesAddUpdate(
    const std::vector<thrift::MplsRoute>& mplsRoutesToUpdate) {
  if (not mplsRoutesToUpdate.size()) {
    return;
  }

  for (auto const& route : mplsRoutesToUpdate) {
    VLOG(1) << "> " << std::to_string(*route.topLabel_ref()) << ", "
            << " NextHopsCount = " << route.nextHops_ref()->size();
    for (auto const& nh : *route.nextHops_ref()) {
      VLOG(1) << " " << toString(nh);
    }
  }
}

bool
Fib::updateRoutes(DecisionRouteUpdate&& routeUpdate, bool isStaticRoutes) {
  SCOPE_EXIT {
    updateRoutesSemaphore_.signal(); // Release when this function returns
  };
  updateRoutesSemaphore_.wait();

  // update flat counters here as they depend on routeState_ and its change
  updateGlobalCounters();

  if (dryrun_) {
    // Do not program routes in case of dryrun
    LOG(INFO) << "Skipping programming of routes in dryrun ... ";
    logPerfEvents(routeUpdate.perfEvents);
    return true; // Treat route updates successful in dry run.
  }

  // For static routes we always update the provided routes immediately
  if (!isStaticRoutes && syncRoutesTimer_->isScheduled()) {
    // Check if there's any full sync scheduled,
    // if so, skip partial sync
    LOG(INFO) << "Pending full sync is scheduled, skip delta sync for now...";
    return true;
  } else if (!isStaticRoutes && (routeState_.dirtyRouteDb or !hasSyncedFib_)) {
    LOG_IF(INFO, routeState_.dirtyRouteDb)
        << "Previous route programming failed or, skip delta sync to enforce "
           "full fib sync...";
    LOG_IF(INFO, !hasSyncedFib_) << "Syncing fib on startup...";

    syncRouteDbDebounced();
    return true;
  }

  // Convert DecisionRouteUpdate to RouteDatabaseDelta to use UnicastRoute
  // and MplsRoute with the FibService client APIs
  auto routeDbDelta = routeUpdate.toThrift();
  auto const& unicastRoutesToUpdate = *routeDbDelta.unicastRoutesToUpdate_ref();
  auto const& mplsRoutesToUpdate = createMplsRoutesWithSelectedNextHops(
      *routeDbDelta.mplsRoutesToUpdate_ref());

  uint32_t numUnicastRoutesToDelete =
      routeDbDelta.unicastRoutesToDelete_ref()->size();
  uint32_t numUnicastRoutesToUpdate = unicastRoutesToUpdate.size();
  uint32_t numMplsRoutesToDelete =
      routeDbDelta.mplsRoutesToDelete_ref()->size();
  uint32_t numMplsRoutesToUpdate = mplsRoutesToUpdate.size();
  uint32_t numOfRouteUpdates = numUnicastRoutesToDelete +
      numUnicastRoutesToUpdate + numMplsRoutesToDelete + numMplsRoutesToUpdate;

  if (numOfRouteUpdates == 0) {
    return true;
  }

  // Make thrift calls to do real programming
  try {
    LOG(INFO) << "Updating routes in FIB";
    const auto startTime = std::chrono::steady_clock::now();

    // Create FIB client if doesn't exists
    createFibClient(evb_, socket_, client_, thriftPort_);

    // Delete unicast routes
    if (numUnicastRoutesToDelete) {
      LOG(INFO) << "Deleting " << numUnicastRoutesToDelete
                << " unicast routes in FIB";
      for (auto const& prefix : *routeDbDelta.unicastRoutesToDelete_ref()) {
        VLOG(1) << "> " << toString(prefix);
      }

      client_->sync_deleteUnicastRoutes(
          kFibId_, *routeDbDelta.unicastRoutesToDelete_ref());
    }

    // Add unicast routes
    if (numUnicastRoutesToUpdate) {
      LOG(INFO) << "Adding/Updating " << numUnicastRoutesToUpdate
                << " unicast routes in FIB";
      printUnicastRoutesAddUpdate(unicastRoutesToUpdate);

      client_->sync_addUnicastRoutes(kFibId_, unicastRoutesToUpdate);
    }

    // Successfully synced routes.
    auto syncedRoutes = std::move(routeUpdate);
    syncedRoutes.type = DecisionRouteUpdate::INCREMENTAL;

    if (enableSegmentRouting_) {
      // Delete mpls routes
      if (numMplsRoutesToDelete) {
        LOG(INFO) << "Deleting " << numMplsRoutesToDelete
                  << " mpls routes in FIB";
        for (auto const& topLabel : *routeDbDelta.mplsRoutesToDelete_ref()) {
          VLOG(1) << "> " << std::to_string(topLabel);
        }

        client_->sync_deleteMplsRoutes(
            kFibId_, *routeDbDelta.mplsRoutesToDelete_ref());
      }

      // Add mpls routes
      if (numMplsRoutesToUpdate) {
        LOG(INFO) << "Adding/Updating " << numMplsRoutesToUpdate
                  << " mpls routes in FIB";
        printMplsRoutesAddUpdate(mplsRoutesToUpdate);

        client_->sync_addMplsRoutes(kFibId_, mplsRoutesToUpdate);
      }
    } else {
      // Clear MPLS routes if segment routes is disabled.
      syncedRoutes.mplsRoutesToDelete.clear();
      syncedRoutes.mplsRoutesToUpdate.clear();
    }

    // Send synced routes into fibRouteUpdatesQueue_.
    fibRouteUpdatesQueue_.push(std::move(syncedRoutes));

    const auto elapsedTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);

    LOG(INFO) << fmt::format(
        "It took {} ms to update {} routes in FIB",
        elapsedTime.count(),
        numOfRouteUpdates);

    fb303::fbData->addStatValue(
        "fib.route_programming.time_ms", elapsedTime.count(), fb303::AVG);
    fb303::fbData->addStatValue(
        "fib.num_of_route_updates", numOfRouteUpdates, fb303::SUM);
    return true;
  } catch (const std::exception& e) {
    client_.reset();
    fb303::fbData->addStatValue(
        "fib.thrift.failure.add_del_route", 1, fb303::COUNT);
    LOG(ERROR) << "Failed to update routes in FIB. Error: "
               << folly::exceptionStr(e);
    return false;
  }
}

bool
Fib::syncRouteDb() {
  const auto& unicastRoutes =
      createUnicastRoutesFromMap(routeState_.unicastRoutes);
  const auto& mplsRoutes =
      createMplsRoutesWithSelectedNextHopsMap(routeState_.mplsRoutes);

  // update flat counters here as they depend on routeState_ and its change
  updateGlobalCounters();

  // In dry run we just print the routes. No real action
  if (dryrun_) {
    LOG(INFO) << "Skipping programming of routes in dryrun ... ";

    LOG(INFO) << "Syncing " << unicastRoutes.size() << " unicast routes";
    printUnicastRoutesAddUpdate(unicastRoutes);

    LOG(INFO) << "Syncing " << mplsRoutes.size() << " mpls routes";
    printMplsRoutesAddUpdate(mplsRoutes);
    return true;
  }

  try {
    LOG(INFO) << "Syncing routes in FIB";
    auto startTime = std::chrono::steady_clock::now();

    createFibClient(evb_, socket_, client_, thriftPort_);
    fb303::fbData->addStatValue("fib.sync_fib_calls", 1, fb303::COUNT);

    DecisionRouteUpdate syncedRoutes;
    // Set type as FULL_SYNC for first Fib sync after restarts.
    // Followup Fib sync are triggered by either route program failures or reset
    // of connection with switch agent.
    syncedRoutes.type = (not hasSyncedFib_)
        ? DecisionRouteUpdate::FULL_SYNC
        : DecisionRouteUpdate::FULL_SYNC_AFTER_FIB_FAILURES;
    // Sync unicast routes
    LOG(INFO) << "Syncing " << unicastRoutes.size() << " unicast routes in FIB";
    client_->sync_syncFib(kFibId_, unicastRoutes);
    syncedRoutes.unicastRoutesToUpdate = routeState_.unicastRoutes;
    printUnicastRoutesAddUpdate(unicastRoutes);

    // Sync mpls routes
    if (enableSegmentRouting_) {
      LOG(INFO) << "Syncing " << mplsRoutes.size() << " mpls routes in FIB";
      client_->sync_syncMplsFib(kFibId_, mplsRoutes);
      std::transform(
          routeState_.mplsRoutes.cbegin(),
          routeState_.mplsRoutes.cend(),
          std::back_inserter(syncedRoutes.mplsRoutesToUpdate),
          [](auto& iter) { return iter.second; });
      printMplsRoutesAddUpdate(mplsRoutes);
    }
    // Send synced routes into fibRouteUpdatesQueue_.
    if (not syncedRoutes.empty()) {
      fibRouteUpdatesQueue_.push(std::move(syncedRoutes));
    }

    const auto elapsedTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);
    LOG(INFO) << "It took " << elapsedTime.count()
              << "ms to sync routes in FIB";
    fb303::fbData->addStatValue(
        "fib.route_sync.time_ms", elapsedTime.count(), fb303::AVG);
    routeState_.dirtyRouteDb = false;
    return true;
  } catch (std::exception const& e) {
    fb303::fbData->addStatValue("fib.thrift.failure.sync_fib", 1, fb303::COUNT);
    LOG(ERROR) << "Failed to sync routes in FIB. Error: "
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
  // Check if switch agent has restarted or not
  if (aliveSince != latestAliveSince_) {
    LOG(WARNING) << "FibAgent seems to have restarted. "
                 << "Performing full route DB sync ...";
    // set dirty flag
    routeState_.dirtyRouteDb = true;
    syncRoutesExpBackoff_.reportSuccess();
    syncRouteDbDebounced();
  }
  latestAliveSince_ = aliveSince;
}

void
Fib::createFibClient(
    folly::EventBase& evb,
    folly::AsyncSocket*& socket,
    std::unique_ptr<thrift::FibServiceAsyncClient>& client,
    int32_t port) {
  if (!client) {
    socket = nullptr;
  }
  // Reset client if channel is not good
  if (socket && (!socket->good() || socket->hangup())) {
    client.reset();
    socket = nullptr;
  }

  // Do not create new client if one exists already
  if (client) {
    return;
  }

  // Create socket to thrift server and set some connection parameters
  auto newSocket = folly::AsyncSocket::newSocket(
      &evb,
      Constants::kPlatformHost.toString(),
      port,
      Constants::kPlatformConnTimeout.count());
  socket = newSocket.get();

  // Create channel and set timeout
  auto channel = apache::thrift::HeaderClientChannel::newChannel(
      std::move(newSocket),
      apache::thrift::HeaderClientChannel::Options()
          .setClientType(THRIFT_FRAMED_DEPRECATED)
          .setProtocolId(apache::thrift::protocol::T_BINARY_PROTOCOL));
  channel->setTimeout(Constants::kPlatformRoutesProcTimeout.count());

  // Reset client_
  client = std::make_unique<thrift::FibServiceAsyncClient>(std::move(channel));
}

void
Fib::updateGlobalCounters() {
  if (not routeState_.hasRoutesFromDecision) {
    return;
  }

  // Set some flat counters
  fb303::fbData->setCounter(
      "fib.num_routes",
      routeState_.unicastRoutes.size() + routeState_.mplsRoutes.size());
  fb303::fbData->setCounter(
      "fib.num_unicast_routes", routeState_.unicastRoutes.size());
  fb303::fbData->setCounter(
      "fib.num_mpls_routes", routeState_.mplsRoutes.size());
}

void
Fib::logPerfEvents(std::optional<thrift::PerfEvents>& perfEvents) {
  if (not perfEvents.has_value() or not perfEvents->events_ref()->size()) {
    return;
  }

  // Ignore bad perf event sample if creation time of first event is
  // less than creation time of our recently logged perf events.
  if (recentPerfEventCreateTs_ >=
      *perfEvents->events_ref()->at(0).unixTs_ref()) {
    LOG(WARNING) << "Ignoring perf event with old create timestamp "
                 << *perfEvents->events_ref()[0].unixTs_ref() << ", expected > "
                 << recentPerfEventCreateTs_;
    return;
  } else {
    recentPerfEventCreateTs_ = *perfEvents->events_ref()->at(0).unixTs_ref();
  }

  // Add latest event information (this function is meant to be called after
  // routeDb has synced)
  addPerfEvent(*perfEvents, myNodeName_, "OPENR_FIB_ROUTES_PROGRAMMED");

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

  // Add event logs
  LogSample sample{};
  sample.addString("event", "ROUTE_CONVERGENCE");
  sample.addStringVector("perf_events", eventStrs);
  sample.addInt("duration_ms", totalDuration.count());
  logSampleQueue_.push(sample);
}

} // namespace openr
