/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Fib.h"

#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/MapUtil.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>

namespace openr {

Fib::Fib(
    std::string myNodeName,
    int32_t thriftPort,
    bool dryrun,
    std::chrono::seconds coldStartDuration,
    const DecisionPubUrl& decisionPubUrl,
    const FibCmdUrl& fibRepUrl,
    const LinkMonitorGlobalPubUrl& linkMonPubUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    fbzmq::Context& zmqContext)
    : myNodeName_(std::move(myNodeName)),
      thriftPort_(thriftPort),
      dryrun_(dryrun),
      coldStartDuration_(coldStartDuration),
      decisionSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      fibRep_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      linkMonSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      decisionPubUrl_(std::move(decisionPubUrl)),
      fibRepUrl_(std::move(fibRepUrl)),
      linkMonPubUrl_(std::move(linkMonPubUrl)),
      expBackoff_(
          std::chrono::milliseconds(8), std::chrono::milliseconds(4096)) {
  syncRoutesTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    auto success = syncRouteDb();
    if (success) {
      expBackoff_.reportSuccess();
    } else {
      // Apply exponential backoff and schedule next run
      expBackoff_.reportError();
      syncRoutesTimer_->scheduleTimeout(
          expBackoff_.getTimeRemainingUntilRetry());
    }
  });

  syncRoutesTimer_->scheduleTimeout(coldStartDuration_);

  healthChecker_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    // Make thrift calls to do real programming
    try {
      keepAliveCheck();
    } catch (const std::exception& e) {
      tData_.addStatValue("fib.thrift.failure.keepalive", 1, fbzmq::COUNT);
      agentRoutes_.clear();
      client_.reset();
      LOG(ERROR) << "Failed to make thrift call to Switch Agent. Error: "
                 << folly::exceptionStr(e);
    }
  });

  // Only schedule health checker in non dry run mode
  if (not dryrun_) {
    healthChecker_->scheduleTimeout(
        Constants::kHealthCheckInterval, true /* schedule periodically */);
  }

  prepare();

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);
}

void
Fib::prepare() noexcept {
  VLOG(2) << "Fib: Subscribing to decision module '" << decisionPubUrl_ << "'";
  const auto decisionSubConnect =
      decisionSub_.connect(fbzmq::SocketUrl{decisionPubUrl_});
  if (decisionSubConnect.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << decisionPubUrl_ << "' "
               << decisionSubConnect.error();
  }
  const auto decisionSubOpt = decisionSub_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (decisionSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << decisionSubOpt.error();
  }

  VLOG(2) << "Fib: Binding to rep url '" << fibRepUrl_ << "'";
  const auto repBind = fibRep_.bind(fbzmq::SocketUrl{fibRepUrl_});
  if (repBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << fibRepUrl_ << "' "
               << repBind.error();
  }

  VLOG(2) << "Fib: Subscribing to Link Monitor module pub url '"
          << linkMonPubUrl_ << "'";
  const auto lmSubConnect =
      linkMonSub_.connect(fbzmq::SocketUrl{linkMonPubUrl_});
  if (lmSubConnect.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << linkMonPubUrl_ << "' "
               << lmSubConnect.error();
  }
  const auto linkSubOpt = linkMonSub_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (linkSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << linkSubOpt.error();
  }

  LOG(INFO) << "Fib thread attaching socket/timeout callbacks...";

  // Schedule periodic timer for submission to monitor
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);

  // Received publication from Decision module
  addSocket(
      fbzmq::RawZmqSocketPtr{*decisionSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(1) << "Fib: publication received ...";
        auto maybeThriftObj = decisionSub_.recvThriftObj<thrift::RouteDatabase>(
            serializer_, Constants::kReadTimeout);
        if (maybeThriftObj.hasError()) {
          LOG(ERROR) << "Error processing decision publication: "
                     << maybeThriftObj.error();
          return;
        }
        auto& thriftRouteDb = maybeThriftObj.value();

        if (thriftRouteDb.thisNodeName != myNodeName_) {
          LOG(ERROR) << "Received publication from unknown node "
                     << thriftRouteDb.thisNodeName;
        } else {
          processRouteDb(std::move(thriftRouteDb));
        }
      });

  // Received FibRequest
  addSocket(fbzmq::RawZmqSocketPtr{*fibRep_}, ZMQ_POLLIN, [this](int) noexcept {
    auto maybeThriftObj = fibRep_.recvThriftObj<thrift::FibRequest>(
        serializer_, Constants::kReadTimeout);
    if (maybeThriftObj.hasError()) {
      LOG(ERROR) << "Error processing Fib Request: " << maybeThriftObj.error();
      fibRep_.sendOne(fbzmq::Message::from(Constants::kErrorResponse).value());
      return;
    }

    auto& thriftReq = maybeThriftObj.value();
    VLOG(1) << "Fib: Request command: `"
            << apache::thrift::TEnumTraits<thrift::FibCommand>::findName(
                   thriftReq.cmd)
            << "` received";
    switch (thriftReq.cmd) {
    case thrift::FibCommand::ROUTE_DB_GET:
      VLOG(2) << "Fib: RouteDb requested";
      // send the thrift::RouteDatabase
      fibRep_.sendThriftObj(dumpRouteDb(), serializer_);
      break;
    case thrift::FibCommand::PERF_DB_GET:
      VLOG(2) << "Fib: PerfDb requested";
      // send the thrift::PerfDatabase
      fibRep_.sendThriftObj(dumpPerfDb(), serializer_);
      break;
    default:
      LOG(ERROR) << "Unknown command received";
      fibRep_.sendOne(fbzmq::Message::from(Constants::kErrorResponse).value());
    }
  });

  // We have received Interface status publication from LinkMonitor
  addSocket(
      fbzmq::RawZmqSocketPtr{*linkMonSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(1) << "Fib: interface status publication received ...";
        auto maybeThriftObj =
            linkMonSub_.recvThriftObj<thrift::InterfaceDatabase>(
                serializer_, Constants::kReadTimeout);
        if (maybeThriftObj.hasError()) {
          LOG(ERROR) << "Error processing link monitor publication"
                     << maybeThriftObj.error();
          return;
        }

        auto& thriftInterfaceDb = maybeThriftObj.value();
        if (thriftInterfaceDb.thisNodeName != myNodeName_) {
          LOG(ERROR) << "Received interface updates from unknown node "
                     << thriftInterfaceDb.thisNodeName;
        } else {
          processInterfaceDb(std::move(thriftInterfaceDb));
        }
      });
}

void
Fib::processRouteDb(thrift::RouteDatabase&& routeDb) {
  VLOG(2) << "Processing route database ... " << routeDb.routes.size()
          << " entries";

  // Update perfEvents_ .. We replace existing perf events with new one as
  // convergence is going to be based on new data, not the old.
  if (routeDb.perfEvents) {
    maybePerfEvents_ = routeDb.perfEvents;
    addPerfEvent(*maybePerfEvents_, myNodeName_, "FIB_ROUTE_DB_RECVD");
  }

  // Clear and build new local maps
  routeDb_.clear();
  for (auto const& route : routeDb.routes) {
    if (route.paths.size() > 0) { // Extra sanity checks
      routeDb_.emplace(route.prefix, route.paths);
    } else {
      LOG(WARNING) << "Received prefix with no paths.";
    }
  }

  // Add some counters
  tData_.addStatValue("fib.process_route_db", 1, fbzmq::COUNT);
  logEvent("ROUTE_UPDATE");

  // Sync new routes on switch agent. On failure set next retry
  syncRouteDbDebounced();
}

void
Fib::processInterfaceDb(thrift::InterfaceDatabase&& interfaceDb) {
  tData_.addStatValue("fib.process_interface_db", 1, fbzmq::COUNT);
  logEvent("IFACE_UPDATE");

  if (interfaceDb.perfEvents) {
    maybePerfEvents_.assign(std::move(interfaceDb.perfEvents));
    addPerfEvent(*maybePerfEvents_, myNodeName_, "FIB_INTF_DB_RECEIVED");
  }

  // Find interfaces which were up before and we detected them down
  std::unordered_set<std::string> affectedInterfaces;
  for (auto const& kv : interfaceDb.interfaces) {
    const auto& ifName = kv.first;
    const auto isUp = kv.second.isUp;

    const auto wasUp = folly::get_default(interfaceStatusDb_, ifName, false);
    interfaceStatusDb_[ifName] = isUp; // Add new status to the map

    if (wasUp and not isUp) {
      affectedInterfaces.insert(ifName);
      LOG(INFO) << "Interface " << ifName << " went DOWN from UP state.";
    }
  }
  for (auto it = routeDb_.begin(); it != routeDb_.end();) {
    // Find valid paths
    std::vector<thrift::Path> validPaths;
    std::vector<thrift::BinaryAddress> validNexthops;
    for (auto const& path : it->second) {
      if (affectedInterfaces.count(path.ifName) == 0) {
        validPaths.push_back(path);
        validNexthops.push_back(path.nextHop);
        auto& nexthop = validNexthops.back();
        nexthop.ifName = path.ifName;
      }
    } // end for ... kv.second

    // Add to affected routes only if something has changed and also reflect
    // changes in routeDb_
    if (validPaths.size() && validPaths.size() != it->second.size()) {
      VLOG(1) << "Nexthop group resize for prefix: " << toString(it->first)
              << ", old: " << it->second.size()
              << ", new: " << validPaths.size();
      it->second = validPaths;
    }

    // Remove route if no valid paths
    if (validPaths.size() == 0) {
      VLOG(1) << "Removing prefix " << toString(it->first)
              << " because of no valid nexthops.";
      it = routeDb_.erase(it);
    } else {
      ++it;
    }
  } // end for ... routeDb_
  syncRouteDbDebounced();
}

thrift::RouteDatabase
Fib::dumpRouteDb() const {
  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = myNodeName_;
  for (auto const& kv : routeDb_) {
    routeDb.routes.emplace_back(
        thrift::Route(apache::thrift::FRAGILE, kv.first, kv.second));
  }
  return routeDb;
}

thrift::PerfDatabase
Fib::dumpPerfDb() const {
  thrift::PerfDatabase perfDb;
  perfDb.thisNodeName = myNodeName_;
  for (auto const& perf : perfDb_) {
    perfDb.eventInfo.emplace_back(perf);
  }
  return perfDb;
}

bool
Fib::syncRouteDb() {
  LOG(INFO) << "Syncing latest routeDb with fib-agent ... ";

  // In dry run we just print the routes. No real action
  if (dryrun_) {
    LOG(INFO) << "Skipping programing of routes in dryrun ... ";
    for (auto const& kv : routeDb_) {
      VLOG(1) << "> " << toIPAddress(kv.first.prefixAddress) << "/"
              << kv.first.prefixLength;
      for (auto const& path : kv.second) {
        VLOG(1) << "via " << toIPAddress(path.nextHop) << "@" << path.ifName
                << " metric " << path.metric;
      }
      VLOG(1) << "";
    }
    logPerfEvents();
    return true;
  }

  // Build routes to be programmed.
  std::set<thrift::UnicastRoute> newRoutes;

  for (auto const& kv : routeDb_) {
    DCHECK(kv.second.size() > 0);

    auto bestPaths = getBestPaths(kv.second);

    std::vector<thrift::BinaryAddress> nexthops;
    for (auto const& path : bestPaths) {
      nexthops.push_back(path.nextHop);
      auto& nexthop = nexthops.back();
      nexthop.ifName = path.ifName;
    }

    // Create thrift::UnicastRoute object in-place
    newRoutes.emplace(
        apache::thrift::FRAGILE, kv.first /* prefix */, std::move(nexthops));
  } // for ... routeDb_

  std::vector<thrift::UnicastRoute> routesToAdd;
  std::set_difference(
    newRoutes.begin(), newRoutes.end(),
    agentRoutes_.begin(), agentRoutes_.end(),
    std::inserter(routesToAdd, routesToAdd.begin()));
  std::vector<thrift::UnicastRoute> routesToRemoveOrUpdate;
  std::set_difference(
    agentRoutes_.begin(), agentRoutes_.end(),
    newRoutes.begin(), newRoutes.end(),
    std::inserter(routesToRemoveOrUpdate, routesToRemoveOrUpdate.begin()));
  std::set<thrift::IpPrefix> prefixesToRemove;
  for (const auto& route : routesToRemoveOrUpdate) {
    prefixesToRemove.emplace(route.dest);
  }
  for (const auto& route : routesToAdd) {
    prefixesToRemove.erase(route.dest);
  }

  try {
    if (maybePerfEvents_) {
      addPerfEvent(*maybePerfEvents_, myNodeName_, "FIB_DEBOUNCE");
    }
    createFibClient();
    if (agentRoutes_.empty()) {
      tData_.addStatValue("fib.sync_fib_calls", 1, fbzmq::COUNT);
      client_->sync_syncFib(kFibId_, routesToAdd);
    } else {
      if (!routesToAdd.empty()) {
        tData_.addStatValue("fib.add_routes_calls", 1, fbzmq::COUNT);
        client_->sync_addUnicastRoutes(kFibId_, routesToAdd);
      }
      if (!prefixesToRemove.empty()) {
        tData_.addStatValue("fib.delete_routes_calls", 1, fbzmq::COUNT);
        client_->sync_deleteUnicastRoutes(kFibId_,
          {prefixesToRemove.begin(), prefixesToRemove.end()});
      }
    }
    agentRoutes_ = std::move(newRoutes);
    logPerfEvents();
    return true;
  } catch (std::exception const& e) {
    tData_.addStatValue("fib.thrift.failure.sync_fib", 1, fbzmq::COUNT);
    LOG(ERROR) << "Failed to sync routeDb with switch FIB agent. Error: "
               << folly::exceptionStr(e);
    agentRoutes_.clear();
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
  createFibClient();
  int64_t aliveSince = client_->sync_aliveSince();
  // Check if FIB has restarted or not
  if (aliveSince != latestAliveSince_) {
    LOG(WARNING) << "FibAgent seems to have restarted. "
                 << "Performing full route DB sync ...";
    agentRoutes_.clear();
    // reset our backoff and wait coldStartDuration_ to try to program routes on
    // the newly started agent.
    expBackoff_.reportSuccess();
    syncRoutesTimer_->scheduleTimeout(coldStartDuration_);
  }
  latestAliveSince_ = aliveSince;
}

void
Fib::createFibClient() {
  // Reset client if channel is not good
  if (socket_ && (!socket_->good() || socket_->hangup())) {
    client_.reset();
    socket_.reset();
  }

  // Do not create new client if one exists already
  if (client_) {
    return;
  }

  // Create socket to thrift server and set some connection parameters
  socket_ = apache::thrift::async::TAsyncSocket::newSocket(
      &evb_,
      Constants::kPlatformHost,
      thriftPort_,
      Constants::kPlatformConnTimeout.count());

  // Create channel and set timeout
  auto channel = apache::thrift::HeaderClientChannel::newChannel(socket_);
  channel->setTimeout(Constants::kPlatformProcTimeout.count());

  // Set BinaryProtocol and Framed client type for talkiing with thrift1 server
  channel->setProtocolId(apache::thrift::protocol::T_BINARY_PROTOCOL);
  channel->setClientType(THRIFT_FRAMED_DEPRECATED);

  // Reset client_
  client_ = std::make_unique<thrift::FibServiceAsyncClient>(std::move(channel));
}

void
Fib::submitCounters() {
  VLOG(3) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  counters["fib.num_routes"] = routeDb_.size();
  counters["fib.require_routedb_sync"] = syncRoutesTimer_->isScheduled();

  // Aliveness report counters
  counters["fib.aliveness"] = 1;

  // Prepare for submitting counters
  fbzmq::CounterMap submittingCounters = prepareSubmitCounters(counters);

  zmqMonitorClient_->setCounters(submittingCounters);
}

void
Fib::logEvent(const std::string& event) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "Fib");
  sample.addString("node_name", myNodeName_);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory,
      {sample.toJson()}));
}

void
Fib::logPerfEvents() {
  if (!maybePerfEvents_ or !maybePerfEvents_->events.size()) {
    LOG(ERROR) << "Received null or empty perf events to log!";
    return;
  }

  // Ignore bad perf event sample if creation time of first event is
  // less than creation time of our recently logged perf events.
  if (recentPerfEventCreateTs_ >= maybePerfEvents_->events[0].unixTs) {
    LOG(WARNING) << "Ignoring perf event with old create timestamp "
                 << maybePerfEvents_->events[0].unixTs << ", expected > "
                 << recentPerfEventCreateTs_;
    return;
  } else {
    recentPerfEventCreateTs_ = maybePerfEvents_->events[0].unixTs;
  }

  // Add latest event information (this function is meant to be called after
  // routeDb has synced)
  addPerfEvent(*maybePerfEvents_, myNodeName_, "OPENR_FIB_ROUTES_PROGRAMMED");

  // Ignore perf events with very off total duration
  auto totalDuration = getTotalPerfEventsDuration(*maybePerfEvents_);
  if (totalDuration.count() < 0 or
      totalDuration > Constants::kConvergenceMaxDuration) {
    LOG(WARNING) << "Ignoring perf event with bad total duration "
                 << totalDuration.count() << "ms.";
    return;
  }

  // Add new entry to perf DB and purge extra entries
  perfDb_.push_back(*maybePerfEvents_);
  while (perfDb_.size() >= Constants::kPerfBufferSize) {
    perfDb_.pop_front();
  }

  // Log event
  auto eventStrs = sprintPerfEvents(*maybePerfEvents_);
  maybePerfEvents_ = folly::none;
  LOG(INFO) << "OpenR convergence performance. "
            << "Duration=" << totalDuration.count();
  for (auto& str : eventStrs) {
    LOG(INFO) << "  " << str;
  }

  // Export convergence duration counter
  tData_.addStatValue(
      "fib.convergence_time_ms", totalDuration.count(), fbzmq::AVG);

  // Log via zmq monitor
  fbzmq::LogSample sample{};
  sample.addString("event", "ROUTE_CONVERGENCE");
  sample.addString("entity", "Fib");
  sample.addString("node_name", myNodeName_);
  sample.addStringVector("perf_events", eventStrs);
  sample.addInt("duration_ms", totalDuration.count());
  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory,
      {sample.toJson()}));
}

std::vector<thrift::Path>
Fib::getBestPaths(std::vector<thrift::Path> const& paths) {
  // Find minimum cost
  int32_t minCost = std::numeric_limits<int32_t>::max();
  for (auto const& path : paths) {
    minCost = std::min(minCost, path.metric);
  }

  // Find paths with the minimum cost
  std::vector<thrift::Path> ret;
  for (auto const& path : paths) {
    if (path.metric == minCost) {
      ret.push_back(path);
    }
  }

  return ret;
}

} // namespace openr
