/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/fib/Fib.h>

namespace fb303 = facebook::fb303;

namespace openr {

namespace { // anonymous for local function definitions

void
logFibUpdateError(thrift::PlatformFibUpdateError const& error) {
  fb303::fbData->addStatValue(
      "fib.thrift.failure.fib_update_error", 1, fb303::COUNT);
  XLOG(ERR) << "Partially failed to update/delete following in FIB.";
  for (auto& [_, prefixes] : *error.vrf2failedAddUpdatePrefixes_ref()) {
    for (auto& prefix : prefixes) {
      XLOG(ERR) << "  > " << toString(prefix) << " add/update";
    }
  }
  for (auto& [_, prefixes] : *error.vrf2failedDeletePrefixes_ref()) {
    for (auto& prefix : prefixes) {
      XLOG(ERR) << "  > " << toString(prefix) << " delete";
    }
  }
  for (auto& label : *error.failedAddUpdateMplsLabels_ref()) {
    XLOG(ERR) << "  > " << label << " add/update";
  }
  for (auto& label : *error.failedDeleteMplsLabels_ref()) {
    XLOG(ERR) << "  > " << label << " delete";
  }
}

} // namespace

Fib::Fib(
    std::shared_ptr<const Config> config,
    messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> staticRouteUpdatesQueue,
    messaging::ReplicateQueue<DecisionRouteUpdate>& fibRouteUpdatesQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue)
    : myNodeName_(config->getConfig().get_node_name()),
      thriftPort_(config->getConfig().get_fib_port()),
      dryrun_(config->getConfig().dryrun_ref().value_or(false)),
      enableSegmentRouting_(
          config->getConfig().enable_segment_routing_ref().value_or(false)),
      routeDeleteDelay_(*config->getConfig().route_delete_delay_ms_ref()),
      retryRoutesExpBackoff_(
          Constants::kFibInitialBackoff, Constants::kFibMaxBackoff, false),
      fibRouteUpdatesQueue_(fibRouteUpdatesQueue),
      logSampleQueue_(logSampleQueue) {
  CHECK_GE(routeDeleteDelay_.count(), 0)
      << "Route delete duration must be >= 0ms";

  // On startup we do require routedb_sync so explicitly set the counter to 0
  fb303::fbData->setCounter("fib.synced", 0);

  //
  // Start RetryRoute fiber with stop signal.
  //
  addFiberTask(
      [this]() mutable noexcept { retryRoutesTask(retryRoutesStopSignal_); });

  //
  // Create KeepAlive task with stop signal. Signalling part consists of two
  // - Promise retained in state variable of Fib module. Fiber awaits on it. The
  //   promise is fulfilled in Fib::stop()
  // - SemiFuture is passed to fiber for awaiting
  //
  addFiberTask(
      [this]() mutable noexcept { keepAliveTask(keepAliveStopSignal_); });

  // Fiber to process route updates from Decision
  addFiberTask([q = std::move(routeUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeThriftObj = q.get(); // perform read
      if (maybeThriftObj.hasError()) {
        XLOG(DBG1) << "Terminating route delta processing fiber";
        break;
      }
      fb303::fbData->addStatValue("fib.process_route_db", 1, fb303::COUNT);
      processDecisionRouteUpdate(std::move(maybeThriftObj).value());
    }
  });

  // Fiber to process and program static route updates.
  // - The routes are only programmed and updated but not deleted
  // - Updates arriving before first Decision RIB update will be processed. The
  //   fiber will terminate after FIB transition out of AWAITING state
  addFiberTask(
      [q = std::move(staticRouteUpdatesQueue), this]() mutable noexcept {
        XLOG(INFO) << "Starting static routes update processing fiber";
        while (true) {
          auto maybeThriftPub = q.get(); // perform read

          // Terminate if queue is closed or we've received RIB from Decision
          if (maybeThriftPub.hasError() or
              routeState_.state != RouteState::AWAITING) {
            XLOG(INFO) << "Terminating static routes update processing fiber";
            break;
          }
          fb303::fbData->addStatValue("fib.process_route_db", 1, fb303::COUNT);
          processStaticRouteUpdate(std::move(maybeThriftPub).value());
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
}

void
Fib::stop() {
  // Send stop signal to internal fibers
  keepAliveStopSignal_.post();
  retryRoutesStopSignal_.post();
  retryRoutesSignal_.signal();

  // Invoke stop method of super class
  OpenrEventBase::stop();
  XLOG(DBG1) << "Stopped FIB event base";
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
    routeDb.thisNodeName_ref() = myNodeName_;
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
    routeDetailDb.thisNodeName_ref() = myNodeName_;
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

  // Add mpls routes to update
  for (const auto& [label, route] : routeUpdate.mplsRoutesToUpdate) {
    mplsRoutes.insert_or_assign(label, route);
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

DecisionRouteUpdate
Fib::RouteState::createUpdate() {
  DecisionRouteUpdate update;

  //
  // Case - First Sync
  // Return all updates
  //

  if (state == SYNCING and not isInitialSynced) {
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

  // Populate mpls routes to add, update, or delete
  for (auto itrLabel = dirtyLabels.begin(); itrLabel != dirtyLabels.end();) {
    if (currentTime < itrLabel->second) {
      ++itrLabel;
      continue; // Route is not yet ready for retry
    }
    auto it = mplsRoutes.find(itrLabel->first);
    if (it == mplsRoutes.end()) { // Delete
      update.mplsRoutesToDelete.emplace_back(itrLabel->first);
    } else { // Add or Update
      update.mplsRoutesToUpdate.emplace(itrLabel->first, it->second);
    }
    // remove as we are creating a new update to program
    itrLabel = dirtyLabels.erase(itrLabel);
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
  if ((routeState_.state == RouteState::SYNCING) or
      (routeState_.dirtyPrefixes.empty() and routeState_.dirtyLabels.empty())) {
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
  for (auto& [_, prefixes] : *fibError.vrf2failedAddUpdatePrefixes_ref()) {
    for (auto& prefix : prefixes) {
      dirtyPrefixes.insert_or_assign(toIPNetwork(prefix), retryAt);
    }
  }
  for (auto& [_, prefixes] : *fibError.vrf2failedDeletePrefixes_ref()) {
    for (auto& prefix : prefixes) {
      dirtyPrefixes.insert_or_assign(toIPNetwork(prefix), retryAt);
    }
  }

  // Mark labels as dirty. All newly failed unicast routes are added into
  // dirtyPrefixes map. We can distinguish between add/update and delete updates
  // in createUpdate().
  for (auto& label : *fibError.failedAddUpdateMplsLabels_ref()) {
    dirtyLabels.insert_or_assign(label, retryAt);
  }

  for (auto& label : *fibError.failedDeleteMplsLabels_ref()) {
    dirtyLabels.insert_or_assign(label, retryAt);
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

  // Filter MPLS next-hops to unique action
  for (auto& [_, mplsRoute] : routeUpdate.mplsRoutesToUpdate) {
    mplsRoute.filterNexthopsToUniqueAction();
  }

  updateRoutes(std::move(routeUpdate));
  if (routeState_.needsRetry()) {
    // Trigger initial Fib sync, or schedule retry routes timer if needed.
    retryRoutesSignal_.signal();
  }
}

void
Fib::processStaticRouteUpdate(DecisionRouteUpdate&& routeUpdate) {
  // NOTE: We only process the static MPLS routes to add or update
  XLOG(INFO) << "Received static routes update";
  routeUpdate.unicastRoutesToUpdate.clear();
  routeUpdate.unicastRoutesToDelete.clear();
  routeUpdate.mplsRoutesToDelete.clear();

  // Static route can only be received in AWAITING state
  CHECK_EQ(routeState_.state, RouteState::AWAITING);

  // Program received static route updates
  updateRoutes(std::move(routeUpdate));
  if (routeState_.needsRetry()) {
    // Schedule retry routes timer if need be
    retryRoutesSignal_.signal();
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
    XLOG(DBG1) << "> " << toString(*route.dest_ref())
               << ", NextHopsCount = " << route.nextHops_ref()->size();
    for (auto const& nh : *route.nextHops_ref()) {
      XLOG(DBG1) << " " << toString(nh);
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
    XLOG(DBG1) << "> " << std::to_string(*route.topLabel_ref()) << ", "
               << " NextHopsCount = " << route.nextHops_ref()->size();
    for (auto const& nh : *route.nextHops_ref()) {
      XLOG(DBG1) << " " << toString(nh);
    }
  }
}

bool
Fib::updateRoutes(DecisionRouteUpdate&& routeUpdate, bool useDeleteDelay) {
  SCOPE_EXIT {
    updateRoutesSemaphore_.signal(); // Release when this function returns
  };
  updateRoutesSemaphore_.wait();

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
  // and MplsRoute with the FibService client APIs
  auto routeDbDelta = routeUpdate.toThrift();

  //
  // Delete Unicast routes
  //
  auto& unicastRoutesToDelete = *routeDbDelta.unicastRoutesToDelete_ref();
  if (delayedDeletionEnabled() and useDeleteDelay) {
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
        createFibClient(*getEvb(), socket_, client_, thriftPort_);
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
  auto const& unicastRoutesToUpdate = *routeDbDelta.unicastRoutesToUpdate_ref();
  if (unicastRoutesToUpdate.size()) {
    XLOG(INFO) << "Adding/Updating " << unicastRoutesToUpdate.size()
               << " unicast routes in FIB";
    printUnicastRoutesAddUpdate(unicastRoutesToUpdate);
    if (dryrun_) {
      XLOG(INFO) << "Skipping add/update of unicast routes in dryrun ... ";
    } else {
      try {
        createFibClient(*getEvb(), socket_, client_, thriftPort_);
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

  //
  // Delete Mpls routes
  //
  auto& mplsRoutesToDelete = *routeDbDelta.mplsRoutesToDelete_ref();
  if (enableSegmentRouting_ and useDeleteDelay and delayedDeletionEnabled()) {
    // Clear the routes to delete
    mplsRoutesToDelete.clear();

    // Mark dirty state here & set
    for (auto& mplsRoute : routeUpdate.mplsRoutesToDelete) {
      const auto [itr, _] = routeState_.dirtyLabels.insert_or_assign(
          mplsRoute, currentTime + routeDeleteDelay_);
      XLOG(INFO) << "Will delete mpls route " << mplsRoute << " after "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(
                        itr->second - currentTime)
                        .count()
                 << "ms";
    }
  }

  if (enableSegmentRouting_ and mplsRoutesToDelete.size()) {
    XLOG(INFO) << "Deleting " << mplsRoutesToDelete.size()
               << " mpls routes in FIB";
    for (auto const& topLabel : mplsRoutesToDelete) {
      XLOG(DBG1) << "> " << std::to_string(topLabel);
    }
    if (dryrun_) {
      XLOG(INFO) << "Skipping deletion of mpls routes in dryrun ... ";
    } else {
      try {
        createFibClient(*getEvb(), socket_, client_, thriftPort_);
        client_->sync_deleteMplsRoutes(kFibId_, mplsRoutesToDelete);
      } catch (std::exception const& e) {
        success = false;
        client_.reset();
        fb303::fbData->addStatValue(
            "fib.thrift.failure.add_del_route", 1, fb303::COUNT);
        XLOG(ERR) << "Failed to delete mpls routes from FIB. Error: "
                  << folly::exceptionStr(e);
        // Marked all routes to be deleted as dirty. So we try to remove them
        // again from FIB.
        for (const auto& label : routeUpdate.mplsRoutesToDelete) {
          routeState_.dirtyLabels.insert_or_assign(label, retryAt);
        }
        // NOTE: We still want to advertise these labels as deleted
      }
    }
  }

  //
  // Update Mpls routes
  //
  auto const& mplsRoutesToUpdate = *routeDbDelta.mplsRoutesToUpdate_ref();
  if (enableSegmentRouting_ and mplsRoutesToUpdate.size()) {
    XLOG(INFO) << "Adding/Updating " << mplsRoutesToUpdate.size()
               << " mpls routes in FIB";
    printMplsRoutesAddUpdate(mplsRoutesToUpdate);
    if (dryrun_) {
      XLOG(INFO) << "Skipping add/update of mpls routes in dryrun ... ";
    } else {
      try {
        createFibClient(*getEvb(), socket_, client_, thriftPort_);
        client_->sync_addMplsRoutes(kFibId_, mplsRoutesToUpdate);
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
        XLOG(ERR) << "Failed to add/update mpls routes in FIB. Error: "
                  << folly::exceptionStr(e);
        // Mark routes we failed to update as dirty for retry. Also declare
        // these routes as deleted to client, because we failed to update them
        // Next retry should restore, but meanwhile clients can take appropriate
        // action because FIB state is unclear e.g. withdraw route from KvStore
        for (auto& [label, _] : routeUpdate.mplsRoutesToUpdate) {
          routeState_.dirtyLabels.insert_or_assign(label, retryAt);
          routeUpdate.mplsRoutesToDelete.emplace_back(label);
        }

        // We don't want to advertise failed route add/updates
        routeUpdate.mplsRoutesToUpdate.clear();
      }
    }
  }

  // Log statistics
  const auto elapsedTime = std::chrono::ceil<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - currentTime);
  XLOG(INFO) << fmt::format(
      "It took {} ms to update routes in FIB", elapsedTime.count());
  fb303::fbData->addStatValue(
      "fib.route_programming.time_ms", elapsedTime.count(), fb303::AVG);
  fb303::fbData->addStatValue(
      "fib.num_of_route_updates", routeUpdate.size(), fb303::SUM);

  // Publish the route update. Clear MPLS routes if segment routing is disabled
  routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
  if (not enableSegmentRouting_) {
    routeUpdate.mplsRoutesToUpdate.clear();
    routeUpdate.mplsRoutesToDelete.clear();
  }
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
  const auto& mplsRoutes = createMplsRoutesFromMap(routeState_.mplsRoutes);
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
  XLOG(INFO) << "Syncing " << unicastRoutes.size() << " unicast routes in FIB";
  printUnicastRoutesAddUpdate(unicastRoutes);
  if (dryrun_) {
    XLOG(INFO) << "Skipping programming of unicast routes in dryrun ... ";
  } else {
    try {
      createFibClient(*getEvb(), socket_, client_, thriftPort_);
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

  //
  // Sync Mpls routes
  //
  if (enableSegmentRouting_) {
    XLOG(INFO) << "Syncing " << mplsRoutes.size() << " mpls routes in FIB";
    printMplsRoutesAddUpdate(mplsRoutes);
    if (dryrun_) {
      XLOG(INFO) << "Skipping programming of mpls routes in dryrun ...";
    } else {
      try {
        createFibClient(*getEvb(), socket_, client_, thriftPort_);
        client_->sync_syncMplsFib(kFibId_, mplsRoutes);
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
    } // else
  } // if enableSegmentRouting_

  // Some statistics
  // NOTE: We set counter for sync time as it is one time event. We report the
  // value of last sync duration
  const auto elapsedTime = std::chrono::ceil<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - currentTime);
  XLOG(INFO) << "It took " << elapsedTime.count() << "ms to sync routes in FIB";
  fb303::fbData->setCounter("fib.route_sync.time_ms", elapsedTime.count());

  // Publish route update. We'll do so only if sync is successful for both MPLS
  // and Unicast routes.
  // NOTE: even empty Fib sync will be published to fibRouteUpdatesQueue_.
  if (not enableSegmentRouting_) {
    fibRouteUpdates.mplsRoutesToUpdate.clear();
    fibRouteUpdates.mplsRoutesToDelete.clear();
  }
  fibRouteUpdatesQueue_.push(std::move(fibRouteUpdates));

  // Transition state on successful sync. Also record our first sync
  transitionRouteState(RouteState::FIB_SYNCED);
  if (not routeState_.isInitialSynced) {
    routeState_.isInitialSynced = true;
    logInitializationEvent("Fib", thrift::InitializationEvent::FIB_SYNCED);
  }
  return true;
}

void
Fib::retryRoutesTask(folly::fibers::Baton& stopSignal) noexcept {
  XLOG(INFO) << "Starting RetryRoutes fiber task";
  auto timeout = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { retryRoutesSignal_.signal(); });

  // Repeat in loop
  while (not stopSignal.ready()) {
    // Wait for signal & retry routes
    retryRoutesSignal_.wait();
    retryRoutes();

    // Add async sleep signal for next invocation. Add only if non zero wait
    if (routeState_.needsRetry()) {
      auto retryDuration = nextRetryDuration();
      XLOG(INFO) << "Scheduling timer after " << retryDuration.count() << "ms";
      timeout->scheduleTimeout(retryDuration);
    }
  } // while
  XLOG(INFO) << "RetryRoutes fiber task got stopped";
}

void
Fib::retryRoutes() noexcept {
  bool success{false};
  retryRoutesExpBackoff_.reportError(); // We increase backoff on every retry
  XLOG(INFO) << "Increasing backoff "
             << retryRoutesExpBackoff_.getCurrentBackoff().count() << "ms";
  if (routeState_.state == RouteState::SYNCING) {
    // SYNC routes if we've RIB snapshot from Decision
    success |= syncRoutes();
  } else {
    // We retry incremental update of routes based on dirty state in AWAITING
    // & SYNCED states
    auto routeUpdate = routeState_.createUpdate();
    if (routeUpdate.empty()) {
      XLOG(INFO) << "Returning because of empty updates";
      return; // Do not process further as this incurred no change
    }
    XLOG(INFO) << "Retry programming of dirty route entries";
    success |= updateRoutes(std::move(routeUpdate), false /* useDeleteDelay */);
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
  XLOG(INFO) << "Starting KeepAlive fiber task";
  while (true) { // Break when stop signal is ready
    keepAlive();
    // Wait for a second. Will terminate if wait completes or signal is ready
    if (stopSignal.try_wait_for(Constants::kKeepAliveCheckInterval)) {
      break; // Baton was posted
    } else {
      stopSignal.reset(); // Baton experienced timeout
    }
  } // while
  XLOG(INFO) << "KeepAlive fiber task got stopped";
}

void
Fib::keepAlive() noexcept {
  int64_t aliveSince{0};
  if (not dryrun_) {
    try {
      createFibClient(*getEvb(), socket_, client_, thriftPort_);
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
    retryRoutesSignal_.signal();
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
  if (routeState_.state == RouteState::AWAITING) {
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
    XLOG(WARNING) << "Ignoring perf event with old create timestamp "
                  << *perfEvents->events_ref()[0].unixTs_ref()
                  << ", expected > " << recentPerfEventCreateTs_;
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
    XLOG(WARNING) << "Ignoring perf event with bad total duration "
                  << totalDuration.count() << "ms.";
    return;
  }

  // Log event
  auto eventStrs = sprintPerfEvents(*perfEvents);
  XLOG(INFO) << "OpenR convergence performance. "
             << "Duration=" << totalDuration.count();
  for (auto& str : eventStrs) {
    XLOG(DBG2) << "  " << str;
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
    routeState_.mplsRoutes.clear();
  }
}

} // namespace openr
