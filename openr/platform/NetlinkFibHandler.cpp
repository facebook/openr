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
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Platform_constants.h>

using apache::thrift::FRAGILE;

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace openr {

namespace {

const std::chrono::seconds kRoutesHoldTimeout{30};

// iproute2 protocol IDs in the kernel are a shared resource
// Various well known and custom protocols use it
// This is a *Weak* attempt to protect against some already
// known protocols
const uint8_t kMinRouteProtocolId = 17;
const uint8_t kMaxRouteProtocolId = 253;

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

NetlinkFibHandler::NetlinkFibHandler(
  fbzmq::ZmqEventLoop* zmqEventLoop,
  std::shared_ptr<fbnl::NetlinkSocket> netlinkSocket)
    : netlinkSocket_(netlinkSocket),
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
      std::move(ret).then([](folly::Unit) {
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

template<class A>
folly::Expected<int16_t, bool>
NetlinkFibHandler::getProtocol(folly::Promise<A>& promise, int16_t clientId) {
  auto ret = thrift::Platform_constants::clientIdtoProtocolId().find(clientId);
  if (ret == thrift::Platform_constants::clientIdtoProtocolId().end()) {
    auto ex =
      NetlinkException(folly::sformat("Invalid ClientId : {}", clientId));
    promise.setException(ex);
    return folly::makeUnexpected(false);
  }
  if (ret->second < kMinRouteProtocolId || ret->second > kMaxRouteProtocolId) {
    auto ex =
      NetlinkException(folly::sformat("Invalid Protocol Id : {}", ret->second));
    promise.setException(ex);
    return folly::makeUnexpected(false);
  }
  return ret->second;
}

std::vector<thrift::UnicastRoute>
NetlinkFibHandler::toThriftUnicastRoutes(
  const fbnl::NlUnicastRoutes& routeDb) {
  std::vector<thrift::UnicastRoute> routes;

  for (auto const& kv : routeDb) {
    auto const& prefix = kv.first;
    auto const& nextHops = kv.second.getNextHops();

    std::unordered_set<thrift::BinaryAddress> binaryNextHops;

    for (auto const& nh : nextHops) {
      CHECK(nh.getGateway().hasValue());
      const auto& ifName = nh.getIfIndex().hasValue()
          ? netlinkSocket_->getIfName(nh.getIfIndex().value()).get()
          : "";
      auto binaryAddr = toBinaryAddress(nh.getGateway().value());
      binaryAddr.ifName = ifName;
      binaryNextHops.insert(binaryAddr);
    }

    routes.emplace_back(thrift::UnicastRoute(
        apache::thrift::FRAGILE,
        thrift::IpPrefix(
            apache::thrift::FRAGILE,
            toBinaryAddress(prefix.first),
            static_cast<int16_t>(prefix.second)),
        std::vector<thrift::BinaryAddress>(
          binaryNextHops.begin(), binaryNextHops.end())));
  }
  return routes;
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addUnicastRoute(
    int16_t clientId, std::unique_ptr<thrift::UnicastRoute> route) {
  DCHECK(route->nexthops.size());

  VLOG(1) << "Adding/Updating route for " << toString(route->dest);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setDestination(toIPNetwork(route->dest))
           .setProtocolId(protocol.value());
  fbnl::NextHopBuilder nhBuilder;
  for (const auto& nh : route->nexthops) {
    if (nh.ifName.hasValue()) {
      nhBuilder.setIfIndex(netlinkSocket_->getIfIndex(nh.ifName.value()).get());
    }
    nhBuilder.setGateway(toIPAddress(nh));
    rtBuilder.addNextHop(nhBuilder.build());
    nhBuilder.reset();
  }

  return netlinkSocket_->addRoute(rtBuilder.buildRoute());
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteUnicastRoute(
    int16_t clientId, std::unique_ptr<thrift::IpPrefix> prefix) {
  VLOG(1) << "Deleting route for " << toString(*prefix);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setDestination(toIPNetwork(*prefix))
           .setProtocolId(protocol.value());
  return netlinkSocket_->delRoute(rtBuilder.buildRoute());
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

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  // Build new routeDb
  fbnl::NlUnicastRoutes newRoutes;
  fbnl::RouteBuilder rtBuilder;
  fbnl::NextHopBuilder nhBuilder;
  for (auto const& route : *routes) {
    CHECK(route.nexthops.size());
    auto prefix = toIPNetwork(route.dest);
    rtBuilder.setDestination(prefix)
        .setProtocolId(protocol.value());

    for (const auto& nh : route.nexthops) {
      if (nh.ifName.hasValue()) {
        nhBuilder.setIfIndex(
            netlinkSocket_->getIfIndex(nh.ifName.value()).get());
      }
      nhBuilder.setGateway(toIPAddress(nh));
      rtBuilder.addNextHop(nhBuilder.build());
      nhBuilder.reset();
    }

    newRoutes.emplace(prefix, rtBuilder.buildRoute());
    rtBuilder.reset();
  }

  return netlinkSocket_->
            syncUnicastRoutes(protocol.value(), std::move(newRoutes));
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

  folly::Promise<
      std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  return netlinkSocket_->getCachedUnicastRoutes(protocol.value())
      .then([this](fbnl::NlUnicastRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            toThriftUnicastRoutes(res));
      })
      .onError([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get routing table by client: " << ex.what()
                   << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>();
      });
}

void
NetlinkFibHandler::getCounters(std::map<std::string, int64_t>& counters) {
  counters["fibagent.num_of_routes"] = netlinkSocket_->getRouteCount().get();
}

} // namespace openr
