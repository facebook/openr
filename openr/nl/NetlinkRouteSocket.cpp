/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkRouteSocket.h"
#include "NetlinkException.h"

#include <algorithm>
#include <memory>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/gen/Base.h>
#include <folly/gen/Core.h>

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace {
const int kIpAddrBufSize = 128;
const uint32_t kAqRouteTableId = RT_TABLE_MAIN;

// iproute2 protocol IDs in the kernel are a shared resource
// Various well known and custom protocols use it
// This is a *Weak* attempt to protect against some already
// known protocols
const uint8_t kMinRouteProtocolId = 17;
const uint8_t kMaxRouteProtocolId = 245;

} // anonymous namespace

namespace openr {

// Our context to pass to libnl when iterating routes
// We keep a local copy of routes
// We keep a local copy of multicast routes to prevent adding duplicate
// mcast routes (kernel and user requested)
// we have a link cache to translate ifName to ifIndex
struct RouteFuncCtx {
  RouteFuncCtx(
      UnicastRoutes* unicastRoutes,
      MulticastRoutes* multicastRoutes,
      LinkRoutes* linkRoutes,
      nl_cache* linkCache,
      uint8_t routeProtocolId)
      : unicastRoutes(unicastRoutes),
        multicastRoutes(multicastRoutes),
        linkRoutes(linkRoutes),
        linkCache(linkCache),
        routeProtocolId_(routeProtocolId) {}

  UnicastRoutes* unicastRoutes{nullptr};
  MulticastRoutes* multicastRoutes{nullptr};
  LinkRoutes* linkRoutes{nullptr};
  nl_cache* linkCache{nullptr};
  uint8_t routeProtocolId_{0};
};

// Our context to pass to libnl when iterating nextHops for a specific route
// nextHops are for that route entry which we fill
// we have a link cache to translate ifName to ifIndex
struct NextHopFuncCtx {
  NextHopFuncCtx(NextHops* nextHops, nl_cache* linkCache)
      : nextHops(nextHops), linkCache(linkCache) {}

  NextHops* nextHops{nullptr};
  nl_cache* linkCache{nullptr};
};

// A simple wrapper over libnl route object
class NetlinkRoute final {
 public:
  NetlinkRoute(const folly::CIDRNetwork& destination, uint8_t routeProtocolId)
      : NetlinkRoute(destination, routeProtocolId, RT_SCOPE_UNIVERSE) {}

  NetlinkRoute(
      const folly::CIDRNetwork& destination,
      uint8_t routeProtocolId,
      uint8_t scope)
      : destination_(destination), routeProtocolId_(routeProtocolId) {
    VLOG(4) << "Creating route object";

    route_ = rtnl_route_alloc();
    if (route_ == nullptr) {
      throw NetlinkException("Cannot allocate route object");
    }
    SCOPE_FAIL {
      rtnl_route_put(route_);
    };

    rtnl_route_set_scope(route_, scope);
    rtnl_route_set_type(route_, RTN_UNICAST);
    rtnl_route_set_family(route_, destination.first.family());
    rtnl_route_set_table(route_, kAqRouteTableId);
    rtnl_route_set_protocol(route_, routeProtocolId_);

    // We need to set destination
    struct nl_addr* nlAddr = nl_addr_build(
        destination_.first.family(),
        (void*)(destination_.first.bytes()),
        destination_.first.byteCount());
    if (nlAddr == nullptr) {
      throw NetlinkException("Failed to create nl addr");
    }

    // route object takes a ref if dst is successfully set
    // so we should always drop our ref, success or failure
    SCOPE_EXIT {
      nl_addr_put(nlAddr);
    };

    nl_addr_set_prefixlen(nlAddr, destination_.second);
    int err = rtnl_route_set_dst(route_, nlAddr);
    if (err != 0) {
      throw NetlinkException(folly::sformat(
          "Failed to set dst for route {} : {}",
          folly::IPAddress::networkToString(destination_),
          nl_geterror(err)));
    }
  }

  ~NetlinkRoute() {
    VLOG(4) << "Destroying route object";
    DCHECK(route_);
    rtnl_route_put(route_);
  }

  struct rtnl_route*
  getRoutePtr() {
    return route_;
  }

  void
  addNextHop(const int ifIdx) {
    // We create a nextHop oject here but by adding it to route
    // the route object owns it
    // Once we destroy the route object, it will internally free this nextHop
    struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
    if (nextHop == nullptr) {
      throw NetlinkException("Failed to create nextHop");
    }
    rtnl_route_nh_set_ifindex(nextHop, ifIdx);
    rtnl_route_add_nexthop(route_, nextHop);
  }

  void
  addNextHop(const int ifIdx, const folly::IPAddress& gateway) {
    CHECK_EQ(destination_.first.family(), gateway.family());

    struct nl_addr* nlGateway = nl_addr_build(
        gateway.family(), (void*)(gateway.bytes()), gateway.byteCount());

    if (nlGateway == nullptr) {
      throw NetlinkException("Failed to create nl addr for gateway");
    }

    // nextHop object takes a ref if gateway is successfully set
    // Either way, success or failure, we drop our ref
    SCOPE_EXIT {
      nl_addr_put(nlGateway);
    };

    // We create a nextHop oject here but by adding it to route
    // the route object owns it
    // Once we destroy the route object, it will internally free this nextHop
    struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
    if (nextHop == nullptr) {
      throw NetlinkException("Failed to create nextHop");
    }

    rtnl_route_nh_set_ifindex(nextHop, ifIdx);
    rtnl_route_nh_set_gateway(nextHop, nlGateway);
    rtnl_route_add_nexthop(route_, nextHop);
  }

  // addNexthop with nexthop = global ip addresses
  void
  addNextHop(const folly::IPAddress& gateway) {
    CHECK_EQ(destination_.first.family(), gateway.family());

    if (gateway.isLinkLocal()) {
      throw NetlinkException(folly::sformat(
          "Failed to resolve interface name for link local address {}",
          gateway.str()));
    }

    struct nl_addr* nlGateway = nl_addr_build(
        gateway.family(), (void*)(gateway.bytes()), gateway.byteCount());

    if (nlGateway == nullptr) {
      throw NetlinkException("Failed to create nl addr for gateway");
    }

    // nextHop object takes a ref if gateway is successfully set
    // Either way, success or failure, we drop our ref
    SCOPE_EXIT {
      nl_addr_put(nlGateway);
    };

    // We create a nextHop oject here but by adding it to route
    // the route object owns it
    // Once we destroy the route object, it will internally free this nextHop
    struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
    if (nextHop == nullptr) {
      throw NetlinkException("Failed to create nextHop");
    }

    rtnl_route_nh_set_gateway(nextHop, nlGateway);
    rtnl_route_add_nexthop(route_, nextHop);
  }

 private:
  NetlinkRoute(const NetlinkRoute&) = delete;
  NetlinkRoute& operator=(const NetlinkRoute&) = delete;

  folly::CIDRNetwork destination_;
  uint8_t routeProtocolId_{0};
  struct rtnl_route* route_{nullptr};
};

NetlinkRouteSocket::NetlinkRouteSocket(
    fbzmq::ZmqEventLoop* zmqEventLoop, uint8_t routeProtocolId)
    : evl_(zmqEventLoop), routeProtocolId_(routeProtocolId) {
  CHECK(evl_) << "Invalid ZMQ event loop handle";
  if ((routeProtocolId_ < kMinRouteProtocolId) ||
      (routeProtocolId_ > kMaxRouteProtocolId)) {
    throw NetlinkException(
        folly::sformat("Invalid route protocol ID: {}", routeProtocolId));
  }

  // We setup the socket explicitly to create our cache explicitly
  int err = 0;
  socket_ = nl_socket_alloc();
  if (socket_ == nullptr) {
    throw NetlinkException("Failed to create socket");
  }

  SCOPE_FAIL {
    nl_socket_free(socket_);
  };

  err = nl_connect(socket_, NETLINK_ROUTE);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to connect socket. Error: {}", nl_geterror(err)));
  }

  // We let flags param be 0 to capture all routes
  // ROUTE_CACHE_CONTENT can be used to get cache routes
  err = rtnl_route_alloc_cache(socket_, AF_INET, 0, &cacheV4_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to allocate v4 route cache . Error: {}", nl_geterror(err)));
  }
  SCOPE_FAIL {
    nl_cache_free(cacheV4_);
  };

  err = rtnl_route_alloc_cache(socket_, AF_INET6, 0, &cacheV6_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to allocate v6 route cache . Error: {}", nl_geterror(err)));
  }
  SCOPE_FAIL {
    nl_cache_free(cacheV6_);
  };

  err = rtnl_link_alloc_cache(socket_, AF_UNSPEC, &linkCache_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to allocate link cache . Error: {}", nl_geterror(err)));
  }
  SCOPE_FAIL {
    nl_cache_free(linkCache_);
  };

  evl_->runImmediatelyOrInEventLoop([this]() mutable {
    doUpdateRouteCache();
    unicastRouteDb_ = doGetUnicastRoutes();
  });
}

NetlinkRouteSocket::~NetlinkRouteSocket() {
  VLOG(3) << "Destroying cache we created";
  nl_cache_free(linkCache_);
  nl_cache_free(cacheV4_);
  nl_cache_free(cacheV6_);
  nl_socket_free(socket_);
}

std::unique_ptr<NetlinkRoute>
NetlinkRouteSocket::buildMulticastOrLinkRouteHelper(
    const folly::CIDRNetwork& prefix,
    const std::string& ifName,
    uint8_t scope) {
  int err = 0;
  err = nl_cache_refill(socket_, linkCache_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to refill link cache . Error: {}", nl_geterror(err)));
  }

  auto route = std::make_unique<NetlinkRoute>(prefix, routeProtocolId_, scope);
  int ifIdx = rtnl_link_name2i(linkCache_, ifName.c_str());
  if (ifIdx == 0) {
    throw NetlinkException(
        folly::sformat("Failed to get ifidx for interface: {}", ifName));
  }
  route->addNextHop(ifIdx);
  VLOG(4) << "Added nextHop for prefix "
          << folly::IPAddress::networkToString(prefix) << " via " << ifName;
  return route;
}

std::unique_ptr<NetlinkRoute>
NetlinkRouteSocket::buildMulticastRoute(
    const folly::CIDRNetwork& prefix, const std::string& ifName) {
  return buildMulticastOrLinkRouteHelper(prefix, ifName, RT_SCOPE_UNIVERSE);
}

std::unique_ptr<NetlinkRoute>
NetlinkRouteSocket::buildLinkRoute(
    const folly::CIDRNetwork& prefix, const std::string& ifName) {
  return buildMulticastOrLinkRouteHelper(prefix, ifName, RT_SCOPE_LINK);
}

std::unique_ptr<NetlinkRoute>
NetlinkRouteSocket::buildUnicastRoute(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  int err = 0;
  err = nl_cache_refill(socket_, linkCache_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to refill link cache . Error: {}", nl_geterror(err)));
  }

  auto route = std::make_unique<NetlinkRoute>(prefix, routeProtocolId_);
  int ifIdx;
  for (const auto& nextHop : nextHops) {
    if (std::get<0>(nextHop).empty()) {
      route->addNextHop(std::get<1>(nextHop));
      VLOG(4) << "Added nextHop for prefix "
              << folly::IPAddress::networkToString(prefix) << " nexthop via "
              << std::get<1>(nextHop).str();
    } else {
      ifIdx = rtnl_link_name2i(linkCache_, std::get<0>(nextHop).c_str());
      if (ifIdx == 0) {
        throw NetlinkException(folly::sformat(
            "Failed to get ifidx for interface: {}", std::get<0>(nextHop)));
      }
      route->addNextHop(ifIdx, std::get<1>(nextHop));
      VLOG(4) << "Added nextHop for prefix "
              << folly::IPAddress::networkToString(prefix) << " nexthop dev "
              << std::get<0>(nextHop) << " via " << std::get<1>(nextHop).str();
    }
  }
  return route;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::addUnicastRoute(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  VLOG(3) << "Adding unicast route";
  CHECK(not nextHops.empty());
  CHECK(not prefix.first.isMulticast() && not prefix.first.isLinkLocal());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, promise = std::move(promise), prefix, nextHops]() mutable {
        try {
          auto search = unicastRouteDb_.find(prefix);
          if (search == unicastRouteDb_.end()) {
            doAddUnicastRoute(prefix, nextHops);
          } else {
            doUpdateRoute(prefix, nextHops, search->second);
          }
          unicastRouteDb_[prefix] = nextHops;
          promise.setValue();
        } catch (NetlinkException const& ex) {
          LOG(ERROR) << "Error adding unicast routes to "
                     << folly::IPAddress::networkToString(prefix);
          promise.setException(ex);
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error adding unicast routes to "
                     << folly::IPAddress::networkToString(prefix);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::addMulticastRoute(
    const folly::CIDRNetwork& prefix, const std::string& ifName) {
  VLOG(3) << "Adding multicast route";
  CHECK(prefix.first.isMulticast());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, promise = std::move(promise), prefix, ifName]() mutable {
        try {
          doAddMulticastRoute(prefix, ifName);
          promise.setValue();
        } catch (NetlinkException const& ex) {
          LOG(ERROR) << "Error adding multicast routes to "
                     << folly::IPAddress::networkToString(prefix);
          promise.setException(ex);
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error adding multicast routes to "
                     << folly::IPAddress::networkToString(prefix);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::deleteUnicastRoute(const folly::CIDRNetwork& prefix) {
  VLOG(3) << "Deleting unicast route";
  CHECK(not prefix.first.isMulticast() && not prefix.first.isLinkLocal());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, promise = std::move(promise), prefix]() mutable {
        try {
          if (unicastRouteDb_.count(prefix) == 0) {
            LOG(ERROR) << "Trying to delete non-existing prefix "
                       << folly::IPAddress::networkToString(prefix);
          } else {
            const auto& oldNextHops = unicastRouteDb_.at(prefix);
            doDeleteUnicastRoute(prefix, oldNextHops);
            unicastRouteDb_.erase(prefix);
          }
          promise.setValue();
        } catch (NetlinkException const& ex) {
          LOG(ERROR) << "Error deleting unicast routes to "
                     << folly::IPAddress::networkToString(prefix)
                     << " Error: " << folly::exceptionStr(ex);
          promise.setException(ex);
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error deleting unicast routes to "
                     << folly::IPAddress::networkToString(prefix)
                     << " Error: " << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::deleteMulticastRoute(
    const folly::CIDRNetwork& prefix, const std::string& ifName) {
  VLOG(3) << "Deleting multicast route";
  CHECK(prefix.first.isMulticast());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, promise = std::move(promise), prefix, ifName]() mutable {
        try {
          doDeleteMulticastRoute(prefix, ifName);
          promise.setValue();
        } catch (NetlinkException const& ex) {
          LOG(ERROR) << "Error deleting multicast routes to "
                     << folly::IPAddress::networkToString(prefix)
                     << " Error: " << folly::exceptionStr(ex);
          promise.setException(ex);
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error deleting multicast routes to "
                     << folly::IPAddress::networkToString(prefix)
                     << " Error: " << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<UnicastRoutes>
NetlinkRouteSocket::getUnicastRoutes() const {
  VLOG(3) << "Getting all routes";

  folly::Promise<UnicastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this, promise = std::move(promise)]() mutable {
    try {
      promise.setValue(unicastRouteDb_);
    } catch (NetlinkException const& ex) {
      LOG(ERROR) << "Error updating route cache: " << folly::exceptionStr(ex);
      promise.setException(ex);
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error updating route cache: " << folly::exceptionStr(ex);
      promise.setException(ex);
    }
  });
  return future;
}

folly::Future<UnicastRoutes>
NetlinkRouteSocket::getKernelUnicastRoutes() {
  VLOG(3) << "Getting all routes from Kernel";

  folly::Promise<UnicastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this, promise = std::move(promise)]() mutable {
    try {
      doUpdateRouteCache();
      unicastRouteDb_ = doGetUnicastRoutes();
      promise.setValue(unicastRouteDb_);
    } catch (NetlinkException const& ex) {
      LOG(ERROR) << "Error updating route cache: " << folly::exceptionStr(ex);
      promise.setException(ex);
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error updating route cache: " << folly::exceptionStr(ex);
      promise.setException(ex);
    }
  });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::syncUnicastRoutes(UnicastRoutes newRouteDb) {
  VLOG(3) << "Syncing Unicast Routes....";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, promise = std::move(promise), newRouteDb]() mutable {
        try {
          doSyncUnicastRoutes(newRouteDb);
          promise.setValue();
        } catch (NetlinkException const& ex) {
          LOG(ERROR) << "Error syncing unicast routeDb with Fib: "
                     << folly::exceptionStr(ex);
          promise.setException(ex);
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error syncing unicast routeDb with Fib: "
                     << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::syncLinkRoutes(const LinkRoutes& newRouteDb) {
  VLOG(3) << "Syncing Link Routes....";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, promise = std::move(promise), newRouteDb]() mutable {
        try {
          doSyncLinkRoutes(newRouteDb);
          promise.setValue();
        } catch (NetlinkException const& ex) {
          LOG(ERROR) << "Error syncing link routeDb with Fib: "
                     << folly::exceptionStr(ex);
          promise.setException(ex);
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error syncing link routeDb with Fib: "
                     << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

void
NetlinkRouteSocket::doAddUnicastRoute(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  if (prefix.first.isV4()) {
    doAddUnicastRouteV4(prefix, nextHops);
  } else {
    doAddUnicastRouteV6(prefix, nextHops);
  }

  // Cache new nexthops in our local-cache if everything is good
  unicastRoutes_[prefix].insert(nextHops.begin(), nextHops.end());
}

void
NetlinkRouteSocket::doAddUnicastRouteV4(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  CHECK(prefix.first.isV4());

  // Create new set of nexthops to be programmed. Existing + New ones
  auto newNextHops = folly::get_default(unicastRoutes_, prefix, NextHops{});
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.second.isV4());
    newNextHops.insert(nextHop);
  }

  auto route = buildUnicastRoute(prefix, newNextHops);
  auto err = rtnl_route_add(socket_, route->getRoutePtr(), NLM_F_REPLACE);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Could not add Route to: {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }
}

void
NetlinkRouteSocket::doAddUnicastRouteV6(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  CHECK(prefix.first.isV6());
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.second.isV6());
  }

  auto route = buildUnicastRoute(prefix, nextHops);
  auto err = rtnl_route_add(socket_, route->getRoutePtr(), 0 /* flags */);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Could not add Route to: {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }
}

void
NetlinkRouteSocket::doDeleteUnicastRoute(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  if (prefix.first.isV4()) {
    deleteUnicastRouteV4(prefix, nextHops);
  } else {
    deleteUnicastRouteV6(prefix, nextHops);
  }

  // Update local cache with reduced nexthops
  auto& existingNexthops = unicastRoutes_[prefix];
  for (auto const& nextHop : nextHops) {
    existingNexthops.erase(nextHop);
  }

  // Remove entry if it doesn't exists
  if (existingNexthops.empty()) {
    unicastRoutes_.erase(prefix);
  }
}

void
NetlinkRouteSocket::deleteUnicastRouteV4(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  CHECK(prefix.first.isV4());
  auto newNextHops = folly::get_default(unicastRoutes_, prefix, NextHops{});
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.second.isV4());
    newNextHops.erase(nextHop);
  }

  auto route = buildUnicastRoute(prefix, newNextHops);
  int err = 0;
  if (newNextHops.empty()) {
    err = rtnl_route_delete(socket_, route->getRoutePtr(), 0 /* flags */);
  } else {
    err = rtnl_route_add(socket_, route->getRoutePtr(), NLM_F_REPLACE);
  }

  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to delete route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }
}

void
NetlinkRouteSocket::deleteUnicastRouteV6(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  CHECK(prefix.first.isV6());
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.second.isV6());
  }

  auto route = buildUnicastRoute(prefix, nextHops);
  int err = rtnl_route_delete(socket_, route->getRoutePtr(), 0 /* flags */);

  // Mask off NLE_OBJ_NOTFOUND error because Netlink automatically withdraw
  // some routes when interface goes down
  if (err != 0 && nl_geterror(err) != nl_geterror(NLE_OBJ_NOTFOUND)) {
    throw NetlinkException(folly::sformat(
        "Failed to delete route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }
}

void
NetlinkRouteSocket::doAddMulticastRoute(
    const folly::CIDRNetwork& prefix, const std::string& ifName) {
  // Since the time we build our cache at init, virtual interfaces
  // could have been created which may have multicast routes
  // installed by the kernel
  // Hence Triggering an update here
  doUpdateRouteCache();

  if (mcastRoutes_.count(std::make_pair(prefix, ifName))) {
    // This could be kernel proto or our proto. we dont care
    LOG(WARNING)
        << "Multicast route: " << folly::IPAddress::networkToString(prefix)
        << " exists for interface: " << ifName;
    return;
  }

  VLOG(3)
      << "Adding multicast route: " << folly::IPAddress::networkToString(prefix)
      << " for interface: " << ifName;

  // We add it with our proto-ID
  std::unique_ptr<NetlinkRoute> route = buildMulticastRoute(prefix, ifName);
  int err = rtnl_route_add(socket_, route->getRoutePtr(), 0);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to add multicast route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }

  mcastRoutes_.emplace(prefix, ifName);
}

void
NetlinkRouteSocket::doDeleteMulticastRoute(
    const folly::CIDRNetwork& prefix, const std::string& ifName) {
  // Triggering an update here
  doUpdateRouteCache();

  if (mcastRoutes_.count(std::make_pair(prefix, ifName)) == 0) {
    // This could be kernel proto or our proto. we dont care
    LOG(WARNING)
        << "Multicast route: " << folly::IPAddress::networkToString(prefix)
        << " doesn't exists for interface: " << ifName;
    return;
  }

  VLOG(3) << "Deleting multicast route: "
          << folly::IPAddress::networkToString(prefix)
          << " for interface: " << ifName;

  // We add it with our proto-ID
  std::unique_ptr<NetlinkRoute> route = buildMulticastRoute(prefix, ifName);
  int err = rtnl_route_delete(socket_, route->getRoutePtr(), 0);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to delete multicast route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }

  mcastRoutes_.erase(std::make_pair(prefix, ifName));
}

UnicastRoutes
NetlinkRouteSocket::doGetUnicastRoutes() const {
  return unicastRoutes_;
}

void
NetlinkRouteSocket::doUpdateRouteCache() {
  // Refill from kernel
  int err = 0;
  err = nl_cache_refill(socket_, cacheV4_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to refill v4-route cache . Error: {}", nl_geterror(err)));
  }
  err = nl_cache_refill(socket_, cacheV6_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to refill v6-route cache . Error: {}", nl_geterror(err)));
  }
  err = nl_cache_refill(socket_, linkCache_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to refill link cache . Error: {}", nl_geterror(err)));
  }

  // clear our own state, we will re-fill here
  unicastRoutes_.clear();
  mcastRoutes_.clear();
  linkRoutes_.clear();

  // Our function for each route called by libnl on iteration
  // These should not throw exceptions as they are libnl callbacks
  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    char ipAddrBuf[kIpAddrBufSize];
    char ifNameBuf[IFNAMSIZ];
    folly::CIDRNetwork prefix;
    RouteFuncCtx* routeFuncCtx = static_cast<RouteFuncCtx*>(arg);
    struct rtnl_route* route = reinterpret_cast<struct rtnl_route*>(obj);

    uint32_t scope = rtnl_route_get_scope(route);
    uint32_t table = rtnl_route_get_table(route);
    uint32_t flags = rtnl_route_get_flags(route);
    uint32_t proto = rtnl_route_get_protocol(route);
    struct nl_addr* dst = rtnl_route_get_dst(route);

    // Skip cached route entries and any routes not in the main table
    if ((table != kAqRouteTableId) || (flags & RTM_F_CLONED)) {
      return;
    }

    // Special handling for default routes
    // All others can be constructed from binary address form
    if (nl_addr_get_prefixlen(dst) == 0) {
      if (nl_addr_get_family(dst) == AF_INET6) {
        VLOG(3) << "Creating a V6 default route";
        prefix = folly::IPAddress::createNetwork("::/0");
      } else if (nl_addr_get_family(dst) == AF_INET) {
        VLOG(3) << "Creating a V4 default route";
        prefix = folly::IPAddress::createNetwork("0.0.0.0/0");
      } else {
        LOG(ERROR) << "Unknown address family for default route";
        return;
      }
    } else {
      // route object dst is the prefix. parse it
      try {
        const auto ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
            static_cast<const unsigned char*>(nl_addr_get_binary_addr(dst)),
            nl_addr_get_len(dst)));
        prefix = {ipAddress, nl_addr_get_prefixlen(dst)};
      } catch (std::exception const& e) {
        LOG(ERROR) << "Error creating prefix for addr: "
                   << nl_addr2str(dst, ipAddrBuf, sizeof(ipAddrBuf));
        return;
      }
    }

    // Multicast routes do not belong to our proto
    // Save it in our local copy and move on
    if (prefix.first.isMulticast()) {
      if (rtnl_route_get_nnexthops(route) != 1) {
        LOG(ERROR) << "Unexpected nextHops for multicast address: "
                   << folly::IPAddress::networkToString(prefix);
        return;
      }
      struct rtnl_nexthop* nextHop = rtnl_route_nexthop_n(route, 0);
      std::string ifName(rtnl_link_i2name(
          routeFuncCtx->linkCache,
          rtnl_route_nh_get_ifindex(nextHop),
          ifNameBuf,
          sizeof(ifNameBuf)));
      routeFuncCtx->multicastRoutes->emplace(
          std::make_pair(std::move(prefix), ifName));
      return;
    }

    // Skip non OpenR routes
    // We deliberately do this after multicast route check
    if (proto != routeFuncCtx->routeProtocolId_) {
      return;
    }

    // Handle link scope routes
    if (scope == RT_SCOPE_LINK) {
      if (rtnl_route_get_nnexthops(route) != 1) {
        LOG(ERROR) << "Unexpected nextHops for link scope route: "
                   << folly::IPAddress::networkToString(prefix);
        return;
      }
      struct rtnl_nexthop* nextHop = rtnl_route_nexthop_n(route, 0);
      std::string ifName(rtnl_link_i2name(
          routeFuncCtx->linkCache,
          rtnl_route_nh_get_ifindex(nextHop),
          ifNameBuf,
          sizeof(ifNameBuf)));
      routeFuncCtx->linkRoutes->emplace(
          std::make_pair(std::move(prefix), ifName));
      return;
    }

    // Ideally link-local routes should never be programmed
    if (prefix.first.isLinkLocal()) {
      return;
    }

    // Check for duplicates. Only applicable for v4 case
    // For v6. Duplicate route is treated as nexthop in kernel
    auto& unicastRoutes = *(routeFuncCtx->unicastRoutes);
    if (prefix.first.isV4() && unicastRoutes.count(prefix)) {
      LOG(FATAL) << "Got redundant v4 route for prefix "
                 << folly::IPAddress::networkToString(prefix)
                 << ". We shouldn't be programming duplicate routes at all.";
    }

    // our nextHop parse function called by libnl for each nextHop
    // of this route
    // These should not throw exceptions as they are libnl callbacks
    auto nextHopFunc = [](struct rtnl_nexthop * obj, void* ctx) noexcept->void {
      char ipAddrBuf[kIpAddrBufSize];
      char ifNameBuf[IFNAMSIZ];
      NextHopFuncCtx* nextHopFuncCtx = (NextHopFuncCtx*)ctx;

      struct rtnl_nexthop* nextHop =
          reinterpret_cast<struct rtnl_nexthop*>(obj);

      // Get the interface name from nextHop
      std::string ifName(rtnl_link_i2name(
          nextHopFuncCtx->linkCache,
          rtnl_route_nh_get_ifindex(nextHop),
          ifNameBuf,
          sizeof(ifNameBuf)));

      // Get the gateway IP from nextHop
      struct nl_addr* gw = rtnl_route_nh_get_gateway(nextHop);
      if (!gw) {
        return;
      }
      try {
        auto gwAddr = folly::IPAddress::fromBinary(folly::ByteRange(
            (const unsigned char*)nl_addr_get_binary_addr(gw),
            nl_addr_get_len(gw)));
        nextHopFuncCtx->nextHops->emplace(std::move(ifName), std::move(gwAddr));
      } catch (std::exception const& e) {
        LOG(ERROR) << "Error parsing GW addr: "
                   << nl_addr2str(gw, ipAddrBuf, sizeof(ipAddrBuf));
        return;
      }
    };

    // For this route, get all nexthops and fill it in our cache
    auto& nextHops = unicastRoutes[prefix];
    NextHopFuncCtx nextHopFuncCtx{&nextHops, routeFuncCtx->linkCache};
    rtnl_route_foreach_nexthop(route, nextHopFunc, &nextHopFuncCtx);
  };

  // Create context and let libnl call our handler routeFunc for
  // each route
  RouteFuncCtx routeFuncCtx{
    &unicastRoutes_,
    &mcastRoutes_,
    &linkRoutes_,
    linkCache_,
    routeProtocolId_
  };
  nl_cache_foreach_filter(cacheV4_, nullptr, routeFunc, &routeFuncCtx);
  nl_cache_foreach_filter(cacheV6_, nullptr, routeFunc, &routeFuncCtx);
}

// helper: given a set of old vs new next-hops add/del the prefix routing info
void
NetlinkRouteSocket::doUpdateRoute(
    const folly::CIDRNetwork& prefix,
    const std::unordered_set<std::pair<std::string, folly::IPAddress>>&
        newNextHops,
    const std::unordered_set<std::pair<std::string, folly::IPAddress>>&
        oldNextHops) {
  DCHECK(!prefix.first.isMulticast());
  DCHECK(!prefix.first.isLinkLocal());

  // add new route
  auto toAdd = buildSetDifference(newNextHops, oldNextHops);
  if (!toAdd.empty()) {
    try {
      doAddUnicastRoute(prefix, toAdd);
    } catch (NetlinkException const& err) {
      throw NetlinkException(folly::sformat(
          "Could not add Route to: {} via nextHops {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          folly::join(
              ", ",
              from(toAdd) |
                  mapped(
                      [](const std::pair<std::string, folly::IPAddress>& val) {
                        return (val.second.str() + "@" + val.first);
                      }) |
                  as<std::set<std::string>>()),
          folly::exceptionStr(err)));
    } catch (std::exception const& err) {
      throw NetlinkException(folly::sformat(
          "Could not add Route to: {} via nextHops {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          folly::join(
              ", ",
              from(toAdd) |
                  mapped(
                      [](const std::pair<std::string, folly::IPAddress>& val) {
                        return (val.second.str() + "@" + val.first);
                      }) |
                  as<std::set<std::string>>()),
          folly::exceptionStr(err)));
    }
  }

  // remove stale route
  auto toDel = buildSetDifference(oldNextHops, newNextHops);
  if (!toDel.empty()) {
    try {
      doDeleteUnicastRoute(prefix, toDel);
    } catch (NetlinkException const& err) {
      throw NetlinkException(folly::sformat(
          "Could not del Route to: {} via nextHops {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          folly::join(
              ", ",
              from(toDel) |
                  mapped(
                      [](const std::pair<std::string, folly::IPAddress>& val) {
                        return (val.second.str() + "@" + val.first);
                      }) |
                  as<std::set<std::string>>()),
          folly::exceptionStr(err)));
    } catch (std::exception const& err) {
      throw NetlinkException(folly::sformat(
          "Could not del Route to: {} via nextHops {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          folly::join(
              ", ",
              from(toDel) |
                  mapped(
                      [](const std::pair<std::string, folly::IPAddress>& val) {
                        return (val.second.str() + "@" + val.first);
                      }) |
                  as<std::set<std::string>>()),
          folly::exceptionStr(err)));
    }
  }
}

void
NetlinkRouteSocket::doSyncUnicastRoutes(UnicastRoutes newRouteDb) {
  // Get latest routing table from kernel and use it as our snapshot
  doUpdateRouteCache();
  unicastRouteDb_ = doGetUnicastRoutes();

  // Go over routes that are not in new routeDb, delete
  for (auto it = unicastRouteDb_.begin(); it != unicastRouteDb_.end();) {
    auto const& prefix = it->first;
    if (newRouteDb.find(prefix) == newRouteDb.end()) {
      try {
        doDeleteUnicastRoute(prefix, it->second);
      } catch (NetlinkException const& err) {
        throw NetlinkException(folly::sformat(
            "Could not del Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err)));
      } catch (std::exception const& err) {
        throw std::runtime_error(folly::sformat(
            "Could not del Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err)));
      }
      it = unicastRouteDb_.erase(it);
    } else {
      ++it;
    }
  }

  // Go over routes that did not exist in old FIB, add
  for (auto it = newRouteDb.begin(); it != newRouteDb.end();) {
    auto const& prefix = it->first;
    if (unicastRouteDb_.find(prefix) == unicastRouteDb_.end()) {
      try {
        doAddUnicastRoute(prefix, it->second);
      } catch (NetlinkException const& err) {
        throw NetlinkException(folly::sformat(
            "Could not add Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err)));
      } catch (std::exception const& err) {
        throw std::runtime_error(folly::sformat(
            "Could not add Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err)));
      }
      unicastRouteDb_.emplace(prefix, std::move(it->second));
      it = newRouteDb.erase(it);
    } else {
      ++it;
    }
  }

  // STEP4: Go over routes that exist both in old and new routeDb, update
  for (auto const& kv : unicastRouteDb_) {
    auto const& prefix = kv.first;
    if (newRouteDb.find(prefix) != newRouteDb.end()) {
      try {
        doUpdateRoute(
            prefix, newRouteDb.at(prefix), unicastRouteDb_.at(prefix));
        unicastRouteDb_[prefix] = newRouteDb.at(prefix);
      } catch (std::exception const& err) {
        throw std::runtime_error(folly::sformat(
            "Could not add Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err)));
      }
    }
  }
}

void
NetlinkRouteSocket::doSyncLinkRoutes(const LinkRoutes& newRouteDb) {
  // Update linkRoutes_ with latest routes from the kernel
  doUpdateRouteCache();

  const auto toDel = buildSetDifference(linkRoutes_, newRouteDb);
  for (const auto& routeToDel : toDel) {
    const auto& prefix = routeToDel.first;
    const auto& ifName = routeToDel.second;
    try {
      std::unique_ptr<NetlinkRoute> route = buildLinkRoute(prefix, ifName);
      int err = rtnl_route_delete(socket_, route->getRoutePtr(), 0);
      if (err != 0) {
        throw NetlinkException(folly::sformat(
            "Could not del link Route to: {} dev {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            ifName,
            nl_geterror(err)));
      }
    } catch (std::exception const& err) {
      throw std::runtime_error(folly::sformat(
          "Could not del link Route to: {} dev {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          ifName,
          folly::exceptionStr(err)));
    }
  }

  const auto toAdd = buildSetDifference(newRouteDb, linkRoutes_);
  for (const auto& routeToAdd : toAdd) {
    const auto& prefix = routeToAdd.first;
    const auto& ifName = routeToAdd.second;
    try {
      std::unique_ptr<NetlinkRoute> route = buildLinkRoute(prefix, ifName);
      int err = rtnl_route_add(socket_, route->getRoutePtr(), 0);
      if (err != 0) {
        throw NetlinkException(folly::sformat(
            "Could not add link Route to: {} dev {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            ifName,
            nl_geterror(err)));
      }
    } catch (std::exception const& err) {
      throw std::runtime_error(folly::sformat(
          "Could not add link Route to: {} dev {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          ifName,
          folly::exceptionStr(err)));
    }
  }

  linkRoutes_ = newRouteDb;
} // namespace openr

} // namespace openr
