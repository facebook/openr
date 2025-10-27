/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/fib/Fib.h>

namespace fb303 = facebook::fb303;

namespace openr {

namespace { // anonymous for local function definitions

void
logFibUpdateError(thrift::PlatformFibUpdateError const& error) {
  fb303::fbData->addStatValue(
      "fib.thrift.failure.fib_update_error", 1, fb303::COUNT);
  XLOG(ERR) << "Partially failed to update/delete following in FIB.";
  for (auto& [_, prefixes] : *error.vrf2failedAddUpdatePrefixes()) {
    for (auto& prefix : prefixes) {
      XLOG(ERR) << "  > " << toString(prefix) << " add/update";
    }
  }
  for (auto& [_, prefixes] : *error.vrf2failedDeletePrefixes()) {
    for (auto& prefix : prefixes) {
      XLOG(ERR) << "  > " << toString(prefix) << " delete";
    }
  }
}

} // namespace

Fib::Fib(
    std::shared_ptr<const Config> config,
    messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueue,
    messaging::ReplicateQueue<DecisionRouteUpdate>& fibRouteUpdatesQueue)
    : myNodeName_(*config->getConfig().node_name()),
      thriftPort_(*config->getConfig().fib_port()),
      dryrun_(config->isDryrun()),
      enableClearFibState_(*config->getConfig().enable_clear_fib_state()),
      routeDeleteDelay_(*config->getConfig().route_delete_delay_ms()),
      retryRoutesExpBackoff_(
          Constants::kFibInitialBackoff, Constants::kFibMaxBackoff, false),
      fibRouteUpdatesQueue_(fibRouteUpdatesQueue) {
  CHECK_GE(routeDeleteDelay_.count(), 0)
      << "Route delete duration must be >= 0ms";

  // On startup we do require routedb_sync so explicitly set the counter to 0
  fb303::fbData->setCounter("fib.synced", 0);

  //
  // Start RetryRoute fiber with stop signal.
  //
  addFiberTask([this]() mutable noexcept {
    XLOG(DBG1) << "Starting retryRoutes task";
    retryRoutesTask(retryRoutesStopSignal_);
    XLOG(DBG1) << "[Exit] RetryRoutes task finished";
  });

  //
  // Create KeepAlive task with stop signal. Signalling part consists of two
  // - Promise retained in state variable of Fib module. Fiber awaits on it. The
  //   promise is fulfilled in Fib::stop()
  // - SemiFuture is passed to fiber for awaiting
  //
  addFiberTask([this]() mutable noexcept {
    XLOG(DBG1) << "Starting keepAlive task";
    keepAliveTask(keepAliveStopSignal_);
    XLOG(DBG1) << "[Exit] KeepAlive task finished";
  });

  // Fiber to process route updates from Decision
  addFiberTask([q = std::move(routeUpdatesQueue), this]() mutable noexcept {
    XLOG(DBG1) << "Starting route-update task";
    while (true) {
      auto maybeThriftObj = q.get(); // perform read
      if (maybeThriftObj.hasError()) {
        break;
      }
      fb303::fbData->addStatValue("fib.process_route_db", 1, fb303::COUNT);
      processDecisionRouteUpdate(std::move(maybeThriftObj).value());
    }
    XLOG(DBG1) << "[Exit] Route-update task finished";
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
}

void
Fib::stop() {
  XLOG(DBG1) << fmt::format(
      "[Exit] Send terminiation signals to stop {} tasks.", getFiberTaskNum());

  /*
   * Send stop signal to internal fibers.
   *
   * Attention:
   * `retryRoutesStopSignal_` baton should be posted first before
   * `retryRoutesSemaphore_` to avoid race-condition of forever waiting inside
   * `retryRoutesTask()`.
   *
   * A potential race-condition:
   *
   * t0: retryRoutesSemaphore_.signal()
   *
   * ----------context-switch----------
   *
   * t1: retryRoutesSemaphore_.wait() in retryRoutesTask() got unblocked.
   * Finish execution till next loop.
   *
   * t2: while loop condition check, keepAliveStopSignal_ NOT yet posted.
   * Continue to go to be blocked at:
   *
   * retryRoutesSemaphore_.wait()
   *
   * ----------context-switch----------
   *
   * t3: retryRoutesStopSignal_.post() <------ NEVER get a chance to unblock
   * semaphore again.
   */
  keepAliveStopSignal_.post();
  retryRoutesStopSignal_.post();
  updateRoutesSemaphore_.signal();
  retryRoutesSemaphore_.signal();

  // Close socket/client created
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait(
      [this]() { client_.reset(); });
  XLOG(DBG1) << "[Exit] Closed client connection towards platform agent.";

  // Invoke stop method of super class
  OpenrEventBase::stop();
  XLOG(DBG1) << "[Exit] Successfully stopped Fib eventbase.";
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
    routeDb.thisNodeName() = myNodeName_;
    for (const auto& route : routeState_.unicastRoutes) {
      routeDb.unicastRoutes()->emplace_back(route.second.toThrift());
    }
    for (const auto& route : routeState_.mplsRoutes) {
      routeDb.mplsRoutes()->emplace_back(route.second.toThrift());
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
    routeDetailDb.thisNodeName() = myNodeName_;
    for (const auto& route : routeState_.unicastRoutes) {
      routeDetailDb.unicastRoutes()->emplace_back(
          route.second.toThriftDetail());
    }
    for (const auto& route : routeState_.mplsRoutes) {
      routeDetailDb.mplsRoutes()->emplace_back(route.second.toThriftDetail());
    }
    p.setValue(
        std::make_unique<thrift::RouteDatabaseDetail>(
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
        p.setValue(
            std::make_unique<std::vector<thrift::UnicastRoute>>(
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
        p.setValue(
            std::make_unique<std::vector<thrift::MplsRoute>>(
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
      XLOG(ERR) << "Invalid IP address as prefix: " << prefixStr;
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
    unicastRoutes.insert_or_assign(prefix, route);
  }

  // Delete unicast routes
  for (const auto& dest : routeUpdate.unicastRoutesToDelete) {
    unicastRoutes.erase(dest);
  }
}

DecisionRouteUpdate
Fib::RouteState::createUpdate() {
  DecisionRouteUpdate update;

  //
  // Case - First Sync
  // Return all updates
  //

  if (state == SYNCING && !isInitialSynced) {
    update.type = DecisionRouteUpdate::FULL_SYNC;
    update.unicastRoutesToUpdate = unicastRoutes;
    update.mplsRoutesToUpdate = mplsRoutes;
    return update;
  }

  //
  // Case - Subsequent Sync or re-programming of failed routes
  // Return updates based on dirty state
  //
  update.type = DecisionRouteUpdate::INCREMENTAL;
  auto const currentTime = std::chrono::steady_clock::now();

  // Populate unicast routes to add, update, or delete
  for (auto itrPrefixes = dirtyPrefixes.begin();
       itrPrefixes != dirtyPrefixes.end();) {
    if (currentTime < itrPrefixes->second) {
      ++itrPrefixes;
      continue; // Route is not yet ready for retry
    }
    auto iter = unicastRoutes.find(itrPrefixes->first);
    if (iter == unicastRoutes.end()) { // Delete
      update.unicastRoutesToDelete.emplace_back(itrPrefixes->first);
    } else { // Add or Update
      update.unicastRoutesToUpdate.emplace(itrPrefixes->first, iter->second);
    }
    // remove as we are creating a new update to program
    itrPrefixes = dirtyPrefixes.erase(itrPrefixes);
  }

  return update;
}

// Computes the minimum timestamp among unicast and mpls routes w.r.t
// current timestamp. In case there is a delete event which expired in the
// past, retry timer is scheduled immediately.
std::chrono::milliseconds
Fib::nextRetryDuration() const {
  // Schedule retry timer immediately if this is initial Fib sync, or delayed
  // deletion is not enabled, or if there is no pending (dirty) routes for
  // processing.
  if ((routeState_.state == RouteState::SYNCING) ||
      (routeState_.dirtyPrefixes.empty() && routeState_.dirtyLabels.empty())) {
    // Return backoff duration if any
    return retryRoutesExpBackoff_.getTimeRemainingUntilRetry();
  }

  auto const currTime = std::chrono::steady_clock::now();
  auto nextRetryTime =
      std::chrono::time_point<std::chrono::steady_clock>::max();

  for (auto& [prefix, addDeleteTime] : routeState_.dirtyPrefixes) {
    nextRetryTime = std::min(addDeleteTime, nextRetryTime);
  }

  for (auto& [label, addDeleteTime] : routeState_.dirtyLabels) {
    nextRetryTime = std::min(addDeleteTime, nextRetryTime);
  }

  return std::chrono::ceil<std::chrono::milliseconds>(
      std::max(nextRetryTime, currTime) - currTime);
}

void
Fib::RouteState::processFibUpdateError(
    thrift::PlatformFibUpdateError const& fibError,
    std::chrono::time_point<std::chrono::steady_clock> retryAt) {
  // Mark prefixes as dirty. All newly failed unicast routes are added into
  // dirtyPrefixes map. We can distinguish between add/update and delete updates
  // in createUpdate().
  for (auto& [_, prefixes] : *fibError.vrf2failedAddUpdatePrefixes()) {
    for (auto& prefix : prefixes) {
      dirtyPrefixes.insert_or_assign(toIPNetwork(prefix), retryAt);
    }
  }
  for (auto& [_, prefixes] : *fibError.vrf2failedDeletePrefixes()) {
    for (auto& prefix : prefixes) {
      dirtyPrefixes.insert_or_assign(toIPNetwork(prefix), retryAt);
    }
  }
}

// Process new route updates received from Decision module.
void
Fib::processDecisionRouteUpdate(DecisionRouteUpdate&& routeUpdate) {
  // Process state transition event
  transitionRouteState(RouteState::RIB_UPDATE);

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
      XLOG(INFO) << "Not installing route for prefix "
                 << folly::IPAddress::networkToString(iter->first);
      iter = routeUpdate.unicastRoutesToUpdate.erase(iter);
    } else {
      ++iter;
    }
  }

  updateRoutes(std::move(routeUpdate));
  if (routeState_.needsRetry()) {
    // Trigger initial Fib sync, or schedule retry routes timer if needed.
    retryRoutesSemaphore_.signal();
  }
}

thrift::PerfDatabase
Fib::dumpPerfDb() const {
  thrift::PerfDatabase perfDb;
  *perfDb.thisNodeName() = myNodeName_;
  for (auto const& perf : perfDb_) {
    perfDb.eventInfo()->emplace_back(perf);
  }
  return perfDb;
}

void
Fib::printUnicastRoutesAddUpdate(
    const std::vector<thrift::UnicastRoute>& unicastRoutesToUpdate) {
  if (!unicastRoutesToUpdate.size()) {
    return;
  }

  for (auto const& route : unicastRoutesToUpdate) {
    XLOG(DBG1) << fmt::format(
        "> {}, NextHopsCount = {}",
        toString(*route.dest()),
        route.nextHops()->size());
    for (auto const& nh : *route.nextHops()) {
      XLOG(DBG2) << fmt::format(" {}", toString(nh));
    }
  }
}

bool
Fib::updateUnicastRoutes(
    const bool useDeleteDelay,
    const std::chrono::time_point<std::chrono::steady_clock>& currentTime,
    const std::chrono::time_point<std::chrono::steady_clock>& retryAt,
    DecisionRouteUpdate& routeUpdate,
    thrift::RouteDatabaseDelta& routeDbDelta) {
  bool success{true};
  //
  // Delete Unicast routes
  //
  auto& unicastRoutesToDelete = *routeDbDelta.unicastRoutesToDelete();
  if (delayedDeletionEnabled() && useDeleteDelay) {
    // Clear the routes to delete
    unicastRoutesToDelete.clear();

    // Mark dirty state here & set
    for (auto& prefix : routeUpdate.unicastRoutesToDelete) {
      const auto [itr, _] = routeState_.dirtyPrefixes.insert_or_assign(
          prefix, currentTime + routeDeleteDelay_);
      XLOG(INFO) << "Will delete unicast route "
                 << folly::IPAddress::networkToString(prefix) << " after "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(
                        itr->second - currentTime)
                        .count()
                 << "ms";
    }
  }

  if (unicastRoutesToDelete.size()) {
    XLOG(INFO) << "Deleting " << unicastRoutesToDelete.size()
               << " unicast routes in FIB";
    for (auto const& prefix : unicastRoutesToDelete) {
      XLOG(DBG1) << "> " << toString(prefix);
    }
    if (dryrun_) {
      XLOG(INFO) << "Skipping deletion of unicast routes in dryrun ... ";
    } else {
      try {
        createFibClient(*getEvb(), client_, thriftPort_);
        client_->sync_deleteUnicastRoutes(kFibId_, unicastRoutesToDelete);
      } catch (std::exception& e) {
        success = false;
        client_.reset();
        fb303::fbData->addStatValue(
            "fib.thrift.failure.add_del_route", 1, fb303::COUNT);
        XLOG(ERR) << "Failed to delete unicast routes from FIB. Error: "
                  << folly::exceptionStr(e);
        // Marked all routes to be deleted as dirty. So we try to remove them
        // again from FIB.
        for (const auto& prefix : routeUpdate.unicastRoutesToDelete) {
          routeState_.dirtyPrefixes.insert_or_assign(prefix, retryAt);
        }
        // NOTE: We still want to advertise these prefixes as deleted
      }
    }
  }

  //
  // Update Unicast routes
  //
  auto const& unicastRoutesToUpdate = *routeDbDelta.unicastRoutesToUpdate();
  if (unicastRoutesToUpdate.size()) {
    XLOG(INFO) << "Adding/Updating " << unicastRoutesToUpdate.size()
               << " unicast routes in FIB";
    printUnicastRoutesAddUpdate(unicastRoutesToUpdate);
    if (dryrun_) {
      XLOG(INFO) << "Skipping add/update of unicast routes in dryrun ... ";
    } else {
      try {
        createFibClient(*getEvb(), client_, thriftPort_);
        client_->sync_addUnicastRoutes(kFibId_, unicastRoutesToUpdate);
      } catch (thrift::PlatformFibUpdateError const& fibUpdateError) {
        success = false;
        logFibUpdateError(fibUpdateError);
        // Remove failed routes from fibRouteUpdates
        routeUpdate.processFibUpdateError(fibUpdateError);
        // Mark failed routes as dirty in route state
        routeState_.processFibUpdateError(fibUpdateError, retryAt);
      } catch (std::exception const& e) {
        success = false;
        client_.reset();
        fb303::fbData->addStatValue(
            "fib.thrift.failure.add_del_route", 1, fb303::COUNT);
        XLOG(ERR) << "Failed to add/update unicast routes in FIB. Error: "
                  << folly::exceptionStr(e);
        // Mark routes we failed to update as dirty for retry. Also declare
        // these routes as deleted to client, because we failed to update them
        // Next retry should restore, but meanwhile clients can take appropriate
        // action because FIB state is unclear e.g. withdraw route from KvStore
        for (auto& [prefix, _] : routeUpdate.unicastRoutesToUpdate) {
          routeState_.dirtyPrefixes.insert_or_assign(prefix, retryAt);
          routeUpdate.unicastRoutesToDelete.emplace_back(prefix);
        }

        // We don't want to advertise failed route add/updates
        routeUpdate.unicastRoutesToUpdate.clear();
      }
    }
  }

  return success;
}

bool
Fib::updateRoutes(
    std::optional<DecisionRouteUpdate>&& maybeRouteUpdate,
    bool useDeleteDelay) {
  SCOPE_EXIT {
    updateRoutesSemaphore_.signal(); // Release when this function returns
  };
  updateRoutesSemaphore_.wait();

  DecisionRouteUpdate routeUpdate;
  if (maybeRouteUpdate.has_value()) {
    routeUpdate = *maybeRouteUpdate;
    XLOG(INFO) << "Processing route update from Decision";
  } else {
    routeUpdate = routeState_.createUpdate();
    XLOG(INFO) << "Retry programming of dirty route entries";
  }

  // Return if empty
  if (routeUpdate.empty()) {
    XLOG(INFO) << "No entries in route update";
    return true;
  }

  // Backup routes in routeState_. In case update routes failed, routes will be
  // programmed in later scheduled FIB sync.
  routeState_.update(routeUpdate);

  // Update flat counters here as they depend on routeState_ and its change
  updateGlobalCounters();

  // Skip route programming if we're in the SYNCING state. We only perform
  // incremental route programming in AWAITING or SYNCED state. In SYNCING
  // state we let `syncRoutes` do the work instead.
  if (routeState_.state == RouteState::SYNCING) {
    XLOG(INFO) << "Skip route programming in SYNCING state";
    return true;
  }

  XLOG(INFO) << "Updating routes in FIB";
  auto const currentTime = std::chrono::steady_clock::now();
  auto const retryAt =
      currentTime + retryRoutesExpBackoff_.getTimeRemainingUntilRetry();
  bool success{true};

  // Convert DecisionRouteUpdate to RouteDatabaseDelta to use UnicastRoute
  // with the FibService client APIs
  auto routeDbDelta = routeUpdate.toThrift();

  success &= updateUnicastRoutes(
      useDeleteDelay, currentTime, retryAt, routeUpdate, routeDbDelta);

  // Log statistics
  const auto elapsedTime = std::chrono::ceil<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - currentTime);
  XLOG(INFO) << fmt::format(
      "It took {} ms to update routes in FIB", elapsedTime.count());
  fb303::fbData->addStatValue(
      "fib.route_programming.time_ms", elapsedTime.count(), fb303::AVG);
  fb303::fbData->addStatValue(
      "fib.num_of_route_updates", routeUpdate.size(), fb303::SUM);

  // Publish the route update.
  routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
  fibRouteUpdatesQueue_.push(std::move(routeUpdate));

  return success;
}

bool
Fib::syncRoutes() {
  SCOPE_EXIT {
    updateRoutesSemaphore_.signal(); // Release when this function returns
  };
  updateRoutesSemaphore_.wait();

  // Create set of routes to sync in thrift format
  const auto& unicastRoutes =
      createUnicastRoutesFromMap(routeState_.unicastRoutes);
  const auto currentTime = std::chrono::steady_clock::now();
  const auto retryAt =
      currentTime + retryRoutesExpBackoff_.getTimeRemainingUntilRetry();
  fb303::fbData->addStatValue("fib.sync_fib_calls", 1, fb303::COUNT);

  // Create DecisionRouteUpdate that'll be published after successful sync. On
  // partial failures we remove routes from this update.
  auto fibRouteUpdates = routeState_.createUpdate();

  // update flat counters here as they depend on routeState_ and its change
  updateGlobalCounters();

  //
  // Sync Unicast routes
  //
  XLOG(INFO)
      << fmt::format("Syncing {} unicast routes in FIB", unicastRoutes.size());

  printUnicastRoutesAddUpdate(unicastRoutes);

  if (dryrun_) {
    /*
     * NOTE: when `enable_clear_fib_state` knob is set, Open/R will clean up the
     * fib routes programmed by its previous incarnation. This handles Open/R
     * rollback from write mode(dryrun_=false) to shadow mode(dryrun_=true).
     */
    if (enableClearFibState_ && (!isUnicastRoutesCleared_)) {
      try {
        auto emptyRoutes = std::vector<thrift::UnicastRoute>{};
        createFibClient(*getEvb(), client_, thriftPort_);
        client_->sync_syncFib(kFibId_, emptyRoutes);
      } catch (std::exception const& e) {
        client_.reset();
        fb303::fbData->addStatValue(
            "fib.thrift.failure.sync_fib", 1, fb303::COUNT);
        XLOG(ERR) << "Failed to sync 0 unicast routes in FIB. Error: "
                  << folly::exceptionStr(e);
        return false;
      }
      // set one-time flag once
      isUnicastRoutesCleared_ = true;

      XLOG(INFO) << "Synced 0 unicast routes to clean up the stale routes.";
    } else {
      XLOG(INFO) << fmt::format(
          "Skipping programming of {} unicast routes.", unicastRoutes.size());
    }
  } else {
    CHECK(!isUnicastRoutesCleared_)
        << "flag should ONLY be set in dry_run mode";

    try {
      createFibClient(*getEvb(), client_, thriftPort_);
      client_->sync_syncFib(kFibId_, unicastRoutes);
    } catch (thrift::PlatformFibUpdateError const& fibUpdateError) {
      logFibUpdateError(fibUpdateError);
      // Remove failed routes from fibRouteUpdates
      fibRouteUpdates.processFibUpdateError(fibUpdateError);
      // Mark failed routes as dirty in route state
      routeState_.processFibUpdateError(fibUpdateError, retryAt);
    } catch (std::exception const& e) {
      client_.reset();
      fb303::fbData->addStatValue(
          "fib.thrift.failure.sync_fib", 1, fb303::COUNT);
      XLOG(ERR) << "Failed to sync unicast routes in FIB. Error: "
                << folly::exceptionStr(e);
      return false;
    }
  }

  // NOTE: We set counter for sync time as it is one time event. We report the
  // value of last sync duration
  const auto elapsedTime = std::chrono::ceil<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - currentTime);
  XLOG(INFO) << "It took " << elapsedTime.count() << "ms to sync routes in FIB";
  fb303::fbData->setCounter("fib.route_sync.time_ms", elapsedTime.count());

  // Publish route update. We'll do so only if sync is successful.
  // NOTE: even empty Fib sync will be published to fibRouteUpdatesQueue_.
  fibRouteUpdatesQueue_.push(std::move(fibRouteUpdates));

  // Transition state on successful sync. Also record our first sync
  transitionRouteState(RouteState::FIB_SYNCED);
  if (!routeState_.isInitialSynced) {
    routeState_.isInitialSynced = true;
    logInitializationEvent("Fib", thrift::InitializationEvent::FIB_SYNCED);
  }
  return true;
}

void
Fib::retryRoutesTask(folly::fibers::Baton& stopSignal) noexcept {
  auto timeout = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { retryRoutesSemaphore_.signal(); });

  // Repeat in loop
  while (true) {
    // Once stop signal is received, break to finish fiber task
    if (stopSignal.ready()) {
      break;
    }

    // Wait for signal & retry routes
    retryRoutesSemaphore_.wait();
    retryRoutes();

    // Add async sleep signal for next invocation. Add only if non zero wait
    if (routeState_.needsRetry()) {
      auto retryDuration = nextRetryDuration();
      XLOG(INFO) << "Scheduling timer after " << retryDuration.count() << "ms";
      timeout->scheduleTimeout(retryDuration);
    }
  } // while
}

void
Fib::retryRoutes() noexcept {
  bool success{false};
  retryRoutesExpBackoff_.reportError(); // We increase backoff on every retry

  XLOG(INFO) << fmt::format(
      "Increasing backoff {}ms",
      retryRoutesExpBackoff_.getCurrentBackoff().count());

  /*
   * ATTN: the following route updating/programming will run into CRITICAL
   * SECTION and needs to be guarded by lock. Fib module maintains `routeState_`
   * as the snapshot of routes to be programmed;
   *
   * A few facts:
   * - `routeState_` will be updated via `update()` API;
   * - `routeState_` should NOT be modified when `updateRoutes()` is invoked;
   * - `routeState_` should honor the latest update from Decision module no
   *    matter what the order of update/retry fiber tasks are invoked;
   */
  if (routeState_.state == RouteState::SYNCING) {
    /*
     * [CRITICAL SECTION]
     *
     * SYNC routes if we've RIB snapshot from Decision
     */
    success |= syncRoutes();
  } else {
    /*
     * [CRITICAL SECTION]
     *
     * Retry incremental update of routes based on dirty state in AWAITING
     * & SYNCED states
     */
    success |= updateRoutes(
        std::nullopt /* indicate retry */, false /* useDeleteDelay */);
  }

  // Clear backoff if programming is successful
  if (success) {
    XLOG(INFO) << "Clearing backoff";
    retryRoutesExpBackoff_.reportSuccess();
  }

  // Set sync state
  fb303::fbData->setCounter(
      "fib.synced", routeState_.state == RouteState::SYNCED);
}

void
Fib::keepAliveTask(folly::fibers::Baton& stopSignal) noexcept {
  while (true) { // Break when stop signal is ready
    keepAlive();
    // Wait for a second. Will terminate if wait completes or signal is ready
    if (stopSignal.try_wait_for(Constants::kKeepAliveCheckInterval)) {
      break; // Baton was posted
    } else {
      stopSignal.reset(); // Baton experienced timeout
    }
  } // while
}

void
Fib::keepAlive() noexcept {
  int64_t aliveSince{0};
  if (!dryrun_) {
    try {
      createFibClient(*getEvb(), client_, thriftPort_);
      aliveSince = client_->sync_aliveSince();
    } catch (const std::exception& e) {
      fb303::fbData->addStatValue(
          "fib.thrift.failure.keepalive", 1, fb303::COUNT);
      client_.reset();
      XLOG(ERR) << "Failed to make thrift call to Switch Agent. Error: "
                << folly::exceptionStr(e);
    }
  }
  // Check if switch agent has restarted or not. Applicable only if we have
  // initialized alive-since
  if (latestAliveSince_ != 0 && aliveSince != latestAliveSince_) {
    XLOG(WARNING) << "FibAgent seems to have restarted. "
                  << "Performing full route DB sync ...";
    // FibAgent has restarted. Enforce full sync
    transitionRouteState(RouteState::FIB_CONNECTED);
    retryRoutesExpBackoff_.reportSuccess();
    retryRoutesSemaphore_.signal();
  }
  latestAliveSince_ = aliveSince;
}

void
Fib::createFibClient(
    folly::EventBase& evb,
    std::unique_ptr<apache::thrift::Client<thrift::FibService>>& client,
    int32_t port) {
  if (client) {
    auto channel = (apache::thrift::RocketClientChannel*)client->getChannel();
    if (channel && channel->good()) {
      // Do not create new client if one exists already with a good channel
      return;
    }
    // Reset client if channel is not good
    client.reset();
  }

  // Create socket to thrift server and set some connection parameters
  auto newSocket = folly::AsyncSocket::newSocket(
      &evb,
      Constants::kPlatformHost.toString(),
      port,
      Constants::kPlatformConnTimeout.count());

  // Create channel and set timeout
  auto channel =
      apache::thrift::RocketClientChannel::newChannel(std::move(newSocket));
  channel->setTimeout(Constants::kPlatformProcTimeout.count());

  // Set client
  client = std::make_unique<apache::thrift::Client<thrift::FibService>>(
      std::move(channel));
}

void
Fib::updateGlobalCounters() {
  if (routeState_.state == RouteState::AWAITING) {
    return;
  }

  // Set some flat counters
  fb303::fbData->setCounter("fib.num_routes", routeState_.unicastRoutes.size());
  fb303::fbData->setCounter(
      "fib.num_unicast_routes", routeState_.unicastRoutes.size());
}

std::string
Fib::RouteState::toStr(RouteState::State state) {
  switch (state) {
  case State::AWAITING:
    return "AWAITING";
  case State::SYNCING:
    return "SYNCING";
  case State::SYNCED:
    return "SYNCED";
  default:
    return "UNKNOWN";
  }
}

void
Fib::transitionRouteState(const RouteState::Event event) {
  // Static matrix representing state transition. Here we handle all events
  // across all states. First index represent the current state, second level
  // index represents the event. Value represents the new state.
  static const std::array<std::array<std::optional<RouteState::State>, 4>, 3>
      stateMap = {
          {{
               /**
                * Index-0, State=AWAITING
                */
               RouteState::SYNCING, // on Event=RIB_UPDATE
               RouteState::AWAITING, // on Event=FIB_CONNECTED,
               std::nullopt // on Event=FIB_SYNCED
           },
           {
               /**
                * Index-1, State=SYNCING
                */
               RouteState::SYNCING, // on Event=RIB_UPDATE
               RouteState::SYNCING, // on Event=FIB_CONNECTED,
               RouteState::SYNCED // on Event=FIB_SYNCED
           },
           {
               /**
                * Index-2, State=SYNCED
                */
               RouteState::SYNCED, // on Event=RIB_UPDATE
               RouteState::SYNCING, // on Event=FIB_CONNECTED,
               std::nullopt // on Event=FIB_SYNCED
           }}};
  const auto prevState = routeState_.state;
  const auto nextState = stateMap.at(prevState).at(event);
  CHECK(nextState.has_value()) << "Next state is 'UNDEFINED'";
  if (prevState != nextState) {
    XLOG(INFO) << "Route state transitions from "
               << routeState_.toStr(prevState) << " to "
               << routeState_.toStr(nextState.value());
  }

  // Update current state
  routeState_.state = nextState.value();

  // NOTE: Special processing
  // Clear all existing routes if we transition from AWAITING -> SYNCING
  // First RIB update is a SYNC and should be treated as source of truth. Any
  // previously installed static route should be ignored.
  if (prevState == RouteState::AWAITING && nextState == RouteState::SYNCING) {
    routeState_.unicastRoutes.clear();
  }
}

} // namespace openr
