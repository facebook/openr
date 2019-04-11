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

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Platform_constants.h>
#include <openr/platform/NetlinkFibHandler.h>

DEFINE_bool(
    enable_recursive_lookup,
    false,
    "If set, recursive lookup (in static routes) will be enabled");

using apache::thrift::FRAGILE;

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace openr {

namespace {

const std::chrono::seconds kRoutesHoldTimeout{30};
const std::chrono::seconds kSyncStaticRouteTimeout{30};

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
      std::move(ret).thenValue([](folly::Unit) {
        LOG(WARNING) << "Expired routes on health check failure!";
      });
    } else {
      VLOG(2) << "Open/R health check: PASS";
    }
  });
  if (FLAGS_enable_recursive_lookup) {
    syncStaticRouteTimer_ =
        fbzmq::ZmqTimeout::make(zmqEventLoop, [&]() noexcept {
          netlinkSocket_->getCachedUnicastRoutes(RTPROT_STATIC)
              .thenValue([this](fbnl::NlUnicastRoutes res) mutable {
                staticRouteCache_ = std::move(res);
                LOG(INFO) << "Static routes synced.";
              })
              .thenError<std::runtime_error>([](std::exception const& ex) {
                LOG(ERROR) << "Failed to get static routes: " << ex.what();
              })
              .onTimeout(std::chrono::seconds(1), []() {
                LOG(ERROR) << "Timed out on getting static routes.";
              });
        });
  }
  zmqEventLoop->runInEventLoop([&]() {
    const bool isPeriodic = true;
    keepAliveCheckTimer_->scheduleTimeout(kRoutesHoldTimeout, isPeriodic);
    if (FLAGS_enable_recursive_lookup) {
      syncStaticRouteTimer_->scheduleTimeout(
          kSyncStaticRouteTimeout, isPeriodic);
    }
  });
}

NetlinkFibHandler::~NetlinkFibHandler() {}

template <class A>
folly::Expected<int16_t, bool>
NetlinkFibHandler::getProtocol(folly::Promise<A>& promise, int16_t clientId) {
  auto ret = thrift::Platform_constants::clientIdtoProtocolId().find(clientId);
  if (ret == thrift::Platform_constants::clientIdtoProtocolId().end()) {
    auto ex =
        fbnl::NlException(folly::sformat("Invalid ClientId : {}", clientId));
    promise.setException(ex);
    return folly::makeUnexpected(false);
  }
  if (ret->second < kMinRouteProtocolId || ret->second > kMaxRouteProtocolId) {
    auto ex = fbnl::NlException(
        folly::sformat("Invalid Protocol Id : {}", ret->second));
    promise.setException(ex);
    return folly::makeUnexpected(false);
  }
  return ret->second;
}

std::vector<thrift::UnicastRoute>
NetlinkFibHandler::toThriftUnicastRoutes(const fbnl::NlUnicastRoutes& routeDb) {
  std::vector<thrift::UnicastRoute> routes;

  for (auto const& kv : routeDb) {
    auto const& prefix = kv.first;
    auto const& nextHops = kv.second.getNextHops();

    std::vector<thrift::NextHopThrift> thriftNextHops;

    for (auto const& nh : nextHops) {
      CHECK(nh.getGateway().hasValue());
      const auto& ifName = nh.getIfIndex().hasValue()
          ? netlinkSocket_->getIfName(nh.getIfIndex().value()).get()
          : "";
      thrift::NextHopThrift nextHop;
      nextHop.address = toBinaryAddress(nh.getGateway().value());
      nextHop.address.ifName = ifName;
      thriftNextHops.emplace_back(std::move(nextHop));
    }

    thrift::UnicastRoute route;
    route.dest = toIpPrefix(prefix);
    route.nextHops.insert(
        route.nextHops.end(), thriftNextHops.begin(), thriftNextHops.end());
    // DEPRECATED - Only for backward compatibility
    route.deprecatedNexthops = createDeprecatedNexthops(route.nextHops);
    routes.emplace_back(std::move(route));
  }
  return routes;
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addUnicastRoute(
    int16_t clientId, std::unique_ptr<thrift::UnicastRoute> route) {
  VLOG(1) << "Adding/Updating route for " << toString(route->dest);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  return netlinkSocket_->addRoute(buildRoute(*route, protocol.value()));
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
  return netlinkSocket_->delRoute(rtBuilder.build());
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addUnicastRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Adding/Updates routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  // Run all route updates in a single eventloop
  evl_->runImmediatelyOrInEventLoop([this,
                                     clientId,
                                     promise = std::move(promise),
                                     routes = std::move(routes)]() mutable {
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

  evl_->runImmediatelyOrInEventLoop([this,
                                     clientId,
                                     promise = std::move(promise),
                                     prefixes = std::move(prefixes)]() mutable {
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
NetlinkFibHandler::future_addMplsRoute(
    int16_t clientId, std::unique_ptr<thrift::MplsRoute> route) {
  VLOG(1) << "Adding/Updating MPLS route for " << route->topLabel;

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }
  return netlinkSocket_->addMplsRoute(buildMplsRoute(*route, protocol.value()));
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteMplsRoute(int16_t clientId, int32_t topLabel) {
  VLOG(1) << "Deleting mpls route " << topLabel;

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }
  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setMplsLabel(topLabel).setProtocolId(protocol.value());
  return netlinkSocket_->delMplsRoute(rtBuilder.build());
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addMplsRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::MplsRoute>> routes) {
  LOG(INFO) << "Adding/Updates routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  // Run all route updates in a single eventloop
  evl_->runImmediatelyOrInEventLoop([this,
                                     clientId,
                                     promise = std::move(promise),
                                     routes = std::move(routes)]() mutable {
    for (auto& route : *routes) {
      auto ptr = folly::make_unique<thrift::MplsRoute>(std::move(route));
      try {
        // This is going to be synchronous call as we are invoking from
        // within event loop
        future_addMplsRoute(clientId, std::move(ptr)).get();
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
NetlinkFibHandler::future_deleteMplsRoutes(
    int16_t clientId, std::unique_ptr<std::vector<int32_t>> topLabels) {
  LOG(INFO) << "Deleting mpls routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this,
       clientId,
       promise = std::move(promise),
       topLabels = std::move(topLabels)]() mutable {
        for (auto& label : *topLabels) {
          try {
            future_deleteMplsRoute(clientId, label).get();
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
  for (auto const& route : *routes) {
    newRoutes.emplace(
        toIPNetwork(route.dest), buildRoute(route, protocol.value()));
  }

  return netlinkSocket_->syncUnicastRoutes(
      protocol.value(), std::move(newRoutes));
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_syncMplsFib(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::MplsRoute>> mplsRoutes) {
  LOG(INFO) << "Syncing MPLS FIB with provided routes. Client: "
            << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  fbnl::NlMplsRoutes newMplsRoutes;
  for (auto const& mplsRoute : *mplsRoutes) {
    newMplsRoutes.emplace(
        mplsRoute.topLabel, buildMplsRoute(mplsRoute, protocol.value()));
  }

  return netlinkSocket_->syncMplsRoutes(
      protocol.value(), std::move(newMplsRoutes));
}

int64_t
NetlinkFibHandler::aliveSince() {
  VLOG(3) << "Received KeepAlive from OpenR";
  recentKeepAliveTs_ = std::chrono::steady_clock::now();
  return startTime_;
}

facebook::fb303::cpp2::fb_status
NetlinkFibHandler::getStatus() {
  VLOG(3) << "Received getStatus";
  return facebook::fb303::cpp2::fb_status::ALIVE;
}

folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
NetlinkFibHandler::future_getRouteTableByClient(int16_t clientId) {
  LOG(INFO) << "Get routes from FIB for clientId " << clientId;

  folly::Promise<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
      promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  return netlinkSocket_->getCachedUnicastRoutes(protocol.value())
      .thenValue([this](fbnl::NlUnicastRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            toThriftUnicastRoutes(res));
      })
      .thenError<std::runtime_error>([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get routing table by client: " << ex.what()
                   << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>();
      });
}

void
NetlinkFibHandler::buildMplsAction(
    fbnl::NextHopBuilder& nhBuilder, const thrift::NextHopThrift& nhop) const {
  if (!nhop.mplsAction.hasValue()) {
    return;
  }
  auto mplsAction = nhop.mplsAction.value();
  nhBuilder.setLabelAction(mplsAction.action);
  if (mplsAction.action == thrift::MplsActionCode::SWAP) {
    if (!mplsAction.swapLabel.hasValue()) {
      throw fbnl::NlException("Swap label not provided");
    }
    nhBuilder.setSwapLabel(mplsAction.swapLabel.value());
  } else if (mplsAction.action == thrift::MplsActionCode::PUSH) {
    if (!mplsAction.pushLabels.hasValue()) {
      throw fbnl::NlException("Push label(s) not provided");
    }
    nhBuilder.setPushLabels(mplsAction.pushLabels.value());
  } else if (mplsAction.action == thrift::MplsActionCode::POP_AND_LOOKUP) {
    auto lpbkIfIndex = netlinkSocket_->getLoopbackIfindex().get();
    if (lpbkIfIndex.hasValue()) {
      nhBuilder.setIfIndex(lpbkIfIndex.value());
    } else {
      throw fbnl::NlException("POP action, loopback interface not available");
    }
  }
  return;
}

void
NetlinkFibHandler::buildNextHop(
    fbnl::RouteBuilder& rtBuilder,
    const std::vector<thrift::NextHopThrift>& nhop) const {
  // add nexthops
  fbnl::NextHopBuilder nhBuilder;
  for (const auto& nh : nhop) {
    // if recursive lookup is enabled, try resolve nexthop first
    if (FLAGS_enable_recursive_lookup) {
      const auto& resolvedNhSet = lookupNexthop(nh.address);
      for (const auto& resolvedNh : resolvedNhSet) {
        if (resolvedNh.getIfIndex().hasValue()) {
          nhBuilder.setIfIndex(resolvedNh.getIfIndex().value());
        }
        if (resolvedNh.getGateway().hasValue()) {
          nhBuilder.setGateway(resolvedNh.getGateway().value());
        }
        rtBuilder.addNextHop(nhBuilder.setWeight(0).build());
        nhBuilder.reset();
      }
      // This nexthop has been resolved, continue to next
      if (resolvedNhSet.size()) {
        continue;
      }
    }
    // recursive lookup is not enabled, or nexthop is not resolved
    if (nh.address.ifName.hasValue()) {
      nhBuilder.setIfIndex(
          netlinkSocket_->getIfIndex(nh.address.ifName.value()).get());
    }
    nhBuilder.setGateway(toIPAddress(nh.address));
    buildMplsAction(nhBuilder, nh);
    rtBuilder.addNextHop(nhBuilder.setWeight(0).build());
    nhBuilder.reset();
  }
}

fbnl::Route
NetlinkFibHandler::buildRoute(
    const thrift::UnicastRoute& route, int protocol) const noexcept {
  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setDestination(toIPNetwork(route.dest)).setProtocolId(protocol);

  // treat empty nexthop as null route
  if (route.nextHops.empty()) {
    rtBuilder.setType(RTN_BLACKHOLE);
    return rtBuilder.build();
  }
  buildNextHop(rtBuilder, route.nextHops);
  return rtBuilder.setFlags(0).setValid(true).build();
}

fbnl::Route
NetlinkFibHandler::buildMplsRoute(
    const thrift::MplsRoute& mplsRoute, int protocol) const noexcept {
  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setMplsLabel(static_cast<uint32_t>(mplsRoute.topLabel));
  rtBuilder.setProtocolId(protocol);

  // treat empty nexthop as null route
  if (mplsRoute.nextHops.empty()) {
    rtBuilder.setType(RTN_BLACKHOLE);
  }
  buildNextHop(rtBuilder, mplsRoute.nextHops);
  return rtBuilder.setFlags(0).setValid(true).build();
}

fbnl::NextHopSet
NetlinkFibHandler::lookupNexthop(const thrift::BinaryAddress& nh) const
    noexcept {
  VLOG(3) << "Nexthop Lookup for " << toIPAddress(nh).str();
  const auto& staticRoute = staticRouteCache_.find(
      folly::IPAddress::createNetwork(toIPAddress(nh).str()));
  if (staticRoute == staticRouteCache_.cend()) {
    return fbnl::NextHopSet{};
  }
  return staticRoute->second.getNextHops();
}

void
NetlinkFibHandler::getCounters(std::map<std::string, int64_t>& counters) {
  counters["fibagent.num_of_routes"] = netlinkSocket_->getRouteCount().get();
}

} // namespace openr
