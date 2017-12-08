/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkFibHandler.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/gen/Base.h>
#include <folly/gen/Core.h>

#include <openr/common/AddressUtil.h>

using apache::thrift::FRAGILE;

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace openr {

namespace {

const uint8_t kAqRouteProtoId = 99;
const std::chrono::seconds kRoutesHoldTimeout{30};

// convert a routeDb into thrift exportable route spec
std::vector<thrift::UnicastRoute>
makeRoutes(const std::unordered_map<
           folly::CIDRNetwork,
           std::unordered_set<std::pair<std::string, folly::IPAddress>>>&
               routeDb) {
  std::vector<thrift::UnicastRoute> routes;

  for (auto const& kv : routeDb) {
    auto const& prefix = kv.first;
    auto const& nextHops = kv.second;

    auto binaryNextHops = from(nextHops) |
        mapped([](const std::pair<std::string, folly::IPAddress>& nextHop) {
                            VLOG(2)
                                << "mapping next-hop " << nextHop.second.str()
                                << " dev " << nextHop.first;
                            auto binaryAddr = toBinaryAddress(nextHop.second);
                            binaryAddr.ifName = nextHop.first;
                            return binaryAddr;
                          }) |
        as<std::vector>();

    routes.emplace_back(thrift::UnicastRoute(
        apache::thrift::FRAGILE,
        thrift::IpPrefix(
            apache::thrift::FRAGILE,
            toBinaryAddress(prefix.first),
            static_cast<int16_t>(prefix.second)),
        std::move(binaryNextHops)));
  }
  return routes;
}

std::string
getClientName(const int16_t clientId) {
  auto it = thrift::_FibClient_VALUES_TO_NAMES.find(
      static_cast<thrift::FibClient>(clientId));
  if (it == thrift::_FibClient_VALUES_TO_NAMES.end()) {
    return folly::sformat("UNKNOWN(id={})", clientId);
  }
  return it->second;
}

} // namespace

NetlinkFibHandler::NetlinkFibHandler(fbzmq::ZmqEventLoop* zmqEventLoop)
    : netlinkSocket_(
          std::make_unique<NetlinkRouteSocket>(zmqEventLoop, kAqRouteProtoId)),
      startTime_(std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count()) {
  keepAliveCheckTimer_ = fbzmq::ZmqTimeout::make(zmqEventLoop, [&]() noexcept {
    auto now = std::chrono::steady_clock::now();
    if (now - recentKeepAliveTs_ > kRoutesHoldTimeout) {
      LOG(ERROR) << "Open/R health check: FAIL. Expiring routes!";
      auto emptyRoutes = std::make_unique<std::vector<thrift::UnicastRoute>>();
      auto ret = future_syncFib(0 /* clientId */, std::move(emptyRoutes));
      ret.then([]() {
        LOG(INFO) << "Expired routes on health check failure!";
      });
    } else {
      LOG(INFO) << "Open/R health check: PASS";
    }
  });
  zmqEventLoop->runInEventLoop([&]() {
    const bool isPeriodic = true;
    keepAliveCheckTimer_->scheduleTimeout(kRoutesHoldTimeout, isPeriodic);
  });
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addUnicastRoute(
    int16_t, std::unique_ptr<thrift::UnicastRoute> route) {
  DCHECK(route->nexthops.size());

  auto prefix = std::make_pair(
      toIPAddress(route->dest.prefixAddress), route->dest.prefixLength);

  auto newNextHops =
      from(route->nexthops) | mapped([](const thrift::BinaryAddress& addr) {
        return std::make_pair(addr.ifName.value(), toIPAddress(addr));
      }) |
      as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();

  LOG(INFO)
      << "Adding route for " << folly::IPAddress::networkToString(prefix)
      << " via "
      << folly::join(
             ", ",
             from(route->nexthops) |
                 mapped([](const thrift::BinaryAddress& addr) {
                   return (toIPAddress(addr).str() + "@" + addr.ifName.value());
                 }) |
                 as<std::set<std::string>>());

  return netlinkSocket_->addUnicastRoute(prefix, newNextHops);
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteUnicastRoute(
    int16_t, std::unique_ptr<thrift::IpPrefix> prefix) {
  auto myPrefix =
      std::make_pair(toIPAddress(prefix->prefixAddress), prefix->prefixLength);

  LOG(INFO) << "Deleting route for "
            << folly::IPAddress::networkToString(myPrefix);

  return netlinkSocket_->deleteUnicastRoute(myPrefix);
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addUnicastRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Adding routes to FIB. Client: " << getClientName(clientId);

  std::vector<folly::Future<folly::Unit>> futures;
  for (auto& route : *routes) {
    auto routePtr = folly::make_unique<thrift::UnicastRoute>(std::move(route));
    auto future = future_addUnicastRoute(clientId, std::move(routePtr));
    futures.emplace_back(std::move(future));
  }
  // Return an aggregate future which is fulfilled when all routes are added
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  folly::collectAll(futures).then(
      [promise = std::move(promise)]() mutable { promise.setValue(); });
  return future;
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteUnicastRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::IpPrefix>> prefixes) {
  LOG(INFO) << "Deleting routes from FIB. Client: " << getClientName(clientId);

  std::vector<folly::Future<folly::Unit>> futures;
  for (auto& prefix : *prefixes) {
    auto prefixPtr = folly::make_unique<thrift::IpPrefix>(std::move(prefix));
    auto future = future_deleteUnicastRoute(clientId, std::move(prefixPtr));
    futures.emplace_back(std::move(future));
  }
  // Return an aggregate future which is fulfilled when all routes are deleted
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  folly::collectAll(futures).then(
      [promise = std::move(promise)]() mutable { promise.setValue(); });
  return future;
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_syncFib(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Syncing FIB with provided routes. Client: "
            << getClientName(clientId);

  // Build new routeDb
  UnicastRoutes newRouteDb;

  for (auto const& route : *routes) {
    if (route.nexthops.size() == 0) {
      LOG(ERROR) << "Got empty nexthops for prefix " << toString(route.dest)
                 << " ... Skipping";
      continue;
    }

    auto prefix = std::make_pair(
        toIPAddress(route.dest.prefixAddress), route.dest.prefixLength);
    auto newNextHops =
        from(route.nexthops) | mapped([](const thrift::BinaryAddress& addr) {
          return std::make_pair(addr.ifName.value(), toIPAddress(addr));
        }) |
        as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();
    newRouteDb[prefix] = newNextHops;
  }

  return netlinkSocket_->syncRoutes(newRouteDb);
}

folly::Future<int64_t>
NetlinkFibHandler::future_periodicKeepAlive(int16_t /* clientId */) {
  VLOG(3) << "Received KeepAlive from OpenR";
  recentKeepAliveTs_ = std::chrono::steady_clock::now();
  return folly::makeFuture(keepAliveId_++);
}

int64_t
NetlinkFibHandler::aliveSince() {
  VLOG(3) << "Received KeepAlive from OpenR";
  recentKeepAliveTs_ = std::chrono::steady_clock::now();
  return startTime_;
}

folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
NetlinkFibHandler::future_getRouteTableByClient(int16_t clientId) {
  LOG(INFO) << "Get routes from FIB for clientId " << clientId;

  return netlinkSocket_->getUnicastRoutes()
      .then([](UnicastRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(res));
      })
      .onError([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get routing table by client: " << ex.what()
                   << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(UnicastRoutes({})));
      });
}

folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
NetlinkFibHandler::future_getKernelRouteTable() {
  LOG(INFO) << "Get routes from kernel";

  return netlinkSocket_->getKernelUnicastRoutes()
      .then([](UnicastRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(res));
      })
      .onError([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get routing table by client: " << ex.what()
                   << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(UnicastRoutes({})));
      });
}

void
NetlinkFibHandler::getCounters(std::map<std::string, int64_t>& counters) {
  LOG(INFO) << "Get counters requested";
  auto routes = netlinkSocket_->getUnicastRoutes().get();
  counters["fibagent.num_of_routes"] = routes.size();
}

} // namespace openr
