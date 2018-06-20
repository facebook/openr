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
                            if (not nextHop.first.empty()) {
                              binaryAddr.ifName = nextHop.first;
                            }
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

NextHops
fromThriftNexthops(const std::vector<thrift::BinaryAddress>& thriftNexthops) {
  NextHops nexthops;
  for (auto const& nexthop : thriftNexthops) {
    nexthops.emplace(
      nexthop.ifName.hasValue() ? nexthop.ifName.value() : "",
      toIPAddress(nexthop)
    );
  }
  return nexthops;
}

} // namespace

NetlinkFibHandler::NetlinkFibHandler(fbzmq::ZmqEventLoop* zmqEventLoop)
    : netlinkSocket_(
          std::make_unique<NetlinkRouteSocket>(zmqEventLoop, kAqRouteProtoId)),
      startTime_(std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count()),
      evl_{zmqEventLoop} {
  CHECK_NOTNULL(zmqEventLoop);
  keepAliveCheckTimer_ = fbzmq::ZmqTimeout::make(zmqEventLoop, [&]() noexcept {
    auto now = std::chrono::steady_clock::now();
    if (now - recentKeepAliveTs_ > kRoutesHoldTimeout) {
      LOG(WARNING) << "Open/R health check: FAIL. Expiring routes!";
      auto emptyRoutes = std::make_unique<std::vector<thrift::UnicastRoute>>();
      auto ret = future_syncFib(0 /* clientId */, std::move(emptyRoutes));
      ret.then([]() {
        LOG(WARNING) << "Expired routes on health check failure!";
      });
    } else {
      VLOG(2) << "Open/R health check: PASS";
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

  VLOG(1) << "Adding/Updating route for " << toString(route->dest);

  auto prefix = toIPNetwork(route->dest);
  auto nexthops = fromThriftNexthops(route->nexthops);
  return netlinkSocket_->addUnicastRoute(
      std::move(prefix),
      std::move(nexthops)
  );
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteUnicastRoute(
    int16_t, std::unique_ptr<thrift::IpPrefix> prefix) {
  VLOG(1) << "Deleting route for " << toString(*prefix);

  auto myPrefix = toIPNetwork(*prefix);
  return netlinkSocket_->deleteUnicastRoute(myPrefix);
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addUnicastRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Adding/Updates routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  // Run all route updates in a single eventloop
  evl_->runImmediatelyOrInEventLoop(
    [ this, clientId,
      promise = std::move(promise),
      routes = std::move(routes)
    ]() mutable {
      for (auto& route : *routes) {
        auto ptr = folly::make_unique<thrift::UnicastRoute>(std::move(route));
        try {
          // This is going to be synchronous call as we are invoking from
          // within event loop
          future_addUnicastRoute(clientId, std::move(ptr)).get();
        } catch (std::exception const& e) {
          promise.setException(e);
          return;
        }
      }
      promise.setValue();
    });

  return future;
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteUnicastRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::IpPrefix>> prefixes) {
  LOG(INFO) << "Deleting routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [ this, clientId,
      promise = std::move(promise),
      prefixes = std::move(prefixes)
    ]() mutable {
      for (auto& prefix : *prefixes) {
        auto ptr = folly::make_unique<thrift::IpPrefix>(std::move(prefix));
        try {
          future_deleteUnicastRoute(clientId, std::move(ptr)).get();
        } catch (std::exception const& e) {
          promise.setException(e);
          return;
        }
      }
      promise.setValue();
    });

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
    CHECK(route.nexthops.size());
    auto prefix = toIPNetwork(route.dest);
    auto nexthops = fromThriftNexthops(route.nexthops);
    newRouteDb.emplace(std::move(prefix), std::move(nexthops));
  }

  return netlinkSocket_->syncUnicastRoutes(std::move(newRouteDb));
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

openr::thrift::ServiceStatus
NetlinkFibHandler::getStatus() {
  VLOG(3) << "Received getStatus";
  return openr::thrift::ServiceStatus::ALIVE;
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

void
NetlinkFibHandler::getCounters(std::map<std::string, int64_t>& counters) {
  auto routes = netlinkSocket_->getUnicastRoutes().get();
  counters["fibagent.num_of_routes"] = routes.size();
}

} // namespace openr
