/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/fib/Fib.h>

namespace fb303 = facebook::fb303;

namespace openr {

Fib::Fib(
    std::shared_ptr<const Config> config,
    int32_t thriftPort,
    std::chrono::seconds coldStartDuration,
    messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> staticRoutesUpdateQueue,
    messaging::ReplicateQueue<DecisionRouteUpdate>& fibUpdatesQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue,
    KvStore* kvStore)
    : myNodeName_(*config->getConfig().node_name_ref()),
      thriftPort_(thriftPort),
      expBackoff_(
          Constants::kFibSyncInitialBackoff,
          Constants::kFibSyncMaxBackoff,
          false),
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
        const auto period = expBackoff_.getTimeRemainingUntilRetry();
        LOG(INFO) << "Scheduling fib sync after " << period.count() << "ms";
        syncRoutesTimer_->scheduleTimeout(period);
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
      VLOG(1) << "Received route updates";
      if (maybeThriftObj.hasError()) {
        LOG(INFO) << "Terminating route delta processing fiber";
        break;
      }

      processRouteUpdates(std::move(maybeThriftObj).value());
    }
  });

  // Fiber to process and program static route updates.
  // - The routes are only programmed and updated but not deleted
  // - Updates arriving before first Decision RIB update will be processed. The
  //   fiber will terminate after that.
  addFiberTask([q = std::move(staticRoutesUpdateQueue),
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

      // Program received static route updates
      updateRoutes(std::move(maybeThriftPub).value(), true /* static routes */);
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
  // Stop KvStoreClient first
  if (kvStoreClient_) {
    kvStoreClient_->stop();
    LOG(INFO) << "KvStoreClient successfully stopped.";
  }

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

std::optional<folly::CIDRNetwork>
Fib::longestPrefixMatch(
    const folly::CIDRNetwork& inputPrefix,
    const std::unordered_map<folly::CIDRNetwork, thrift::UnicastRouteDetail>&
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
      routeDb.unicastRoutes_ref()->emplace_back(
          *route.second.unicastRoute_ref());
    }
    for (const auto& route : routeState_.mplsRoutes) {
      routeDb.mplsRoutes_ref()->emplace_back(*route.second.mplsRoute_ref());
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
      routeDetailDb.unicastRoutes_ref()->emplace_back(route.second);
    }
    for (const auto& route : routeState_.mplsRoutes) {
      routeDetailDb.mplsRoutes_ref()->emplace_back(route.second);
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
      retRouteVec.emplace_back(*routes.second.unicastRoute_ref());
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
    retRouteVec.emplace_back(
        *routeState_.unicastRoutes.at(prefix).unicastRoute_ref());
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
      retRouteVec.emplace_back(*routes.second.mplsRoute_ref());
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
      retRouteVec.emplace_back(*routes.second.mplsRoute_ref());
    }
  }

  return retRouteVec;
}

messaging::RQueue<DecisionRouteUpdate>
Fib::getFibUpdatesReader() {
  return fibUpdatesQueue_.getReader();
}

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
  auto i = routeUpdate.unicastRoutesToUpdate.begin();
  while (i != routeUpdate.unicastRoutesToUpdate.end()) {
    if (i->second.doNotInstall) {
      LOG(INFO) << "Not installing route for prefix "
                << folly::IPAddress::networkToString(i->first);
      i = routeUpdate.unicastRoutesToUpdate.erase(i);
    } else {
      ++i;
    }
  }

  // Add/Update unicast routes to update
  for (const auto& [prefix, route] : routeUpdate.unicastRoutesToUpdate) {
    routeState_.unicastRoutes[prefix] = route.toThriftDetail();
  }

  // Add mpls routes to update
  for (const auto& route : routeUpdate.mplsRoutesToUpdate) {
    routeState_.mplsRoutes[route.label] = route.toThriftDetail();
  }

  // Delete unicast routes
  for (const auto& dest : routeUpdate.unicastRoutesToDelete) {
    routeState_.unicastRoutes.erase(dest);
  }

  // Delete mpls routes
  for (const auto& topLabel : routeUpdate.mplsRoutesToDelete) {
    routeState_.mplsRoutes.erase(topLabel);
  }

  // Add some counters
  fb303::fbData->addStatValue("fib.process_route_db", 1, fb303::COUNT);
  // Send request to agent
  updateRoutes(std::move(routeUpdate), false /* static routes */);
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
      VLOG(2) << " " << toString(nh);
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
      VLOG(2) << " " << toString(nh);
    }
  }
}

void
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
    return;
  }

  // Publish the fib streaming routes after considering donotinstall
  // and dryrun logic.
  if (not routeUpdate.unicastRoutesToUpdate.empty() ||
      not routeUpdate.unicastRoutesToDelete.empty() ||
      not routeUpdate.mplsRoutesToUpdate.empty() ||
      not routeUpdate.mplsRoutesToDelete.empty()) {
    // Due to donotinstall logic it's possible to have empty change,
    // no need to publish empty updates.
    fibUpdatesQueue_.push(routeUpdate);
  }

  // For static routes we always update the provided routes immediately
  if (!isStaticRoutes && syncRoutesTimer_->isScheduled()) {
    // Check if there's any full sync scheduled,
    // if so, skip partial sync
    LOG(INFO) << "Pending full sync is scheduled, skip delta sync for now...";
    return;
  } else if (!isStaticRoutes && (routeState_.dirtyRouteDb or !hasSyncedFib_)) {
    if (hasSyncedFib_) {
      LOG(INFO) << "Previous route programming failed or, skip delta sync to "
                << "enforce full fib sync...";
    } else {
      LOG(INFO) << "Syncing fib on startup...";
    }
    syncRouteDbDebounced();
    return;
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
    return;
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
    }

    const auto elapsedTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);
    LOG(INFO) << "It took " << elapsedTime.count() << "ms to update "
              << "routes in FIB";

    routeState_.dirtyRouteDb = false;
    fb303::fbData->addStatValue(
        "fib.route_programming.time_ms", elapsedTime.count(), fb303::AVG);
    fb303::fbData->addStatValue(
        "fib.num_of_route_updates", numOfRouteUpdates, fb303::SUM);
  } catch (const std::exception& e) {
    fb303::fbData->addStatValue(
        "fib.thrift.failure.add_del_route", 1, fb303::COUNT);
    client_.reset();
    routeState_.dirtyRouteDb = true;
    syncRouteDbDebounced(); // Schedule future full sync of route DB
    LOG(ERROR) << "Failed to update routes in FIB. Error: "
               << folly::exceptionStr(e);
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

    // Sync unicast routes
    LOG(INFO) << "Syncing " << unicastRoutes.size() << " unicast routes in FIB";
    client_->sync_syncFib(kFibId_, unicastRoutes);
    printUnicastRoutesAddUpdate(unicastRoutes);

    // Sync mpls routes
    if (enableSegmentRouting_) {
      LOG(INFO) << "Syncing " << mplsRoutes.size() << " mpls routes in FIB";
      client_->sync_syncMplsFib(kFibId_, mplsRoutes);
      printMplsRoutesAddUpdate(mplsRoutes);
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
          AreaId{openr::thrift::Types_constants::kDefaultArea()},
          Constants::kFibTimeMarker.toString() + myNodeName_,
          std::to_string(fb303::fbData->getCounters().at(
              "fib.local_route_program_time_ms.avg.60")),
          Constants::kTtlInfInterval);
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

  // Add event logs
  LogSample sample{};
  sample.addString("event", "ROUTE_CONVERGENCE");
  sample.addStringVector("perf_events", eventStrs);
  sample.addInt("duration_ms", totalDuration.count());
  logSampleQueue_.push(sample);
}

} // namespace openr
