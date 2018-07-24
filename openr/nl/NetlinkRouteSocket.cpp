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
#include <folly/system/ThreadName.h>
#include <folly/gen/Base.h>
#include <folly/gen/Core.h>

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace {
const int kIpAddrBufSize = 128;

} // anonymous namespace

namespace openr {

struct PrefixCmp {
  bool operator()(
    const folly::CIDRNetwork& lhs, const folly::CIDRNetwork& rhs) {
    if (lhs.first != rhs.first) {
      return lhs.first < rhs.first;
    } else {
      return lhs.second < rhs.second;
    }
  }
};

// Our context to pass to libnl when iterating routes
// We keep a local copy of routes
// We keep a local copy of multicast routes to prevent adding duplicate
// mcast routes (kernel and user requested)
// we have a link cache to translate ifName to ifIndex
struct RouteFuncCtx {
  RouteFuncCtx(
      UnicastRoutesDb* unicastRoutesCache,
      MulticastRoutesDb* multicastRoutesCache,
      LinkRoutesDb* linkRoutesCache,
      nl_cache* linkCache)
      : unicastRoutesCache(unicastRoutesCache),
        multicastRoutesCache(multicastRoutesCache),
        linkRoutesCache(linkRoutesCache),
        linkCache(linkCache) {}

  UnicastRoutesDb* unicastRoutesCache{nullptr};
  MulticastRoutesDb* multicastRoutesCache{nullptr};
  LinkRoutesDb* linkRoutesCache{nullptr};
  nl_cache* linkCache{nullptr};
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
  NetlinkRoute(const folly::CIDRNetwork& destination, uint8_t protocolId)
      : NetlinkRoute(destination, protocolId, RT_SCOPE_UNIVERSE) {}

  NetlinkRoute(
      const folly::CIDRNetwork& destination,
      uint8_t protocolId,
      uint8_t scope)
      : destination_(destination), protocolId_(protocolId) {
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
    rtnl_route_set_table(route_, RT_TABLE_MAIN);
    rtnl_route_set_protocol(route_, protocolId_);

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

    if (gateway.isV4()) {
      rtnl_route_nh_set_flags(nextHop, RTNH_F_ONLINK);
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
  uint8_t protocolId_{0};
  struct rtnl_route* route_{nullptr};
};

NetlinkRouteSocket::NetlinkRouteSocket(
    fbzmq::ZmqEventLoop* zmqEventLoop)
    : evl_(zmqEventLoop) {
  CHECK(evl_) << "Invalid ZMQ event loop handle";
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
  });
}

NetlinkRouteSocket::~NetlinkRouteSocket() {
  VLOG(3) << "Destroying cache we created";
  nl_cache_free(linkCache_);
  nl_cache_free(cacheV4_);
  nl_cache_free(cacheV6_);
  nl_socket_free(socket_);
  linkCache_ = nullptr;
  cacheV4_ = nullptr;
  cacheV6_ = nullptr;
  socket_ = nullptr;
}

std::unique_ptr<NetlinkRoute>
NetlinkRouteSocket::buildMulticastOrLinkRouteHelper(
    const folly::CIDRNetwork& prefix,
    const std::string& ifName,
    uint8_t protocolId,
    uint8_t scope) {
  auto route = std::make_unique<NetlinkRoute>(prefix, protocolId, scope);

  updateLinkCacheThrottled();
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
    uint8_t protocolId,
    const folly::CIDRNetwork& prefix,
    const std::string& ifName) {
  return buildMulticastOrLinkRouteHelper(
            prefix, ifName, protocolId, RT_SCOPE_UNIVERSE);
}

std::unique_ptr<NetlinkRoute>
NetlinkRouteSocket::buildLinkRoute(
    uint8_t protocolId,
    const folly::CIDRNetwork& prefix,
    const std::string& ifName) {
  return buildMulticastOrLinkRouteHelper(
            prefix, ifName, protocolId, RT_SCOPE_LINK);
}

std::unique_ptr<NetlinkRoute>
NetlinkRouteSocket::buildUnicastRoute(
    uint8_t protocolId,
    const folly::CIDRNetwork& prefix,
    const NextHops& nextHops) {
  auto route = std::make_unique<NetlinkRoute>(prefix, protocolId);
  for (const auto& nextHop : nextHops) {
    if (std::get<0>(nextHop).empty()) {
      route->addNextHop(std::get<1>(nextHop));
      VLOG(4) << "Added nextHop for prefix "
              << folly::IPAddress::networkToString(prefix) << " nexthop via "
              << std::get<1>(nextHop).str();
    } else {
      updateLinkCacheThrottled();
      int ifIdx = rtnl_link_name2i(linkCache_, std::get<0>(nextHop).c_str());
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
    uint8_t protocolId,
    folly::CIDRNetwork prefix,
    NextHops nextHops) {
  VLOG(3) << "Adding unicast route";
  CHECK(not nextHops.empty());
  CHECK(not prefix.first.isMulticast() && not prefix.first.isLinkLocal());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        promise = std::move(promise),
        prefix = std::move(prefix),
        nextHops = std::move(nextHops),
        protocolId
      ]() mutable {
        try {
          doAddUpdateUnicastRoute(protocolId, prefix, nextHops);
          promise.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error adding unicast routes to "
                     << folly::IPAddress::networkToString(prefix)
                     << ". Exception: " << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::addMulticastRoute(
    uint8_t protocolId,
    folly::CIDRNetwork prefix,
    std::string ifName) {
  VLOG(3) << "Adding multicast route";
  CHECK(prefix.first.isMulticast());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        promise = std::move(promise),
        prefix = std::move(prefix),
        ifName = std::move(ifName),
        protocolId
      ]() mutable {
        try {
          doAddMulticastRoute(protocolId, prefix, ifName);
          promise.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error adding multicast routes to "
                     << folly::IPAddress::networkToString(prefix)
                     << ". Exception: " << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::deleteUnicastRoute(
  uint8_t protocolId,
  folly::CIDRNetwork prefix) {
  VLOG(3) << "Deleting unicast route";
  CHECK(not prefix.first.isMulticast() && not prefix.first.isLinkLocal());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        promise = std::move(promise),
        prefix = std::move(prefix),
        protocolId
      ]() mutable {
        try {
          const auto& unicastRoutes = unicastRoutesCache_[protocolId];
          if (unicastRoutes.count(prefix) == 0) {
            LOG(ERROR) << "Trying to delete non-existing prefix "
                       << folly::IPAddress::networkToString(prefix);
          } else {
            doDeleteUnicastRoute(protocolId, prefix);
          }
          promise.setValue();
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
    uint8_t protocolId,
    folly::CIDRNetwork prefix,
    std::string ifName) {
  VLOG(3) << "Deleting multicast route";
  CHECK(prefix.first.isMulticast());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        promise = std::move(promise),
        prefix = std::move(prefix),
        ifName = std::move(ifName),
        protocolId
      ]() mutable {
        try {
          doDeleteMulticastRoute(protocolId, prefix, ifName);
          promise.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error deleting multicast routes to "
                     << folly::IPAddress::networkToString(prefix)
                     << " Error: " << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<int64_t>
NetlinkRouteSocket::getRouteCount() const {
  VLOG(3) << "Getting routes number";

  folly::Promise<int64_t> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this,
     p = std::move(promise)]() mutable {
     int64_t count = 0;
     for (const auto& routes: unicastRoutesCache_) {
       count += routes.second.size();
     }
     p.setValue(count);
  });
  return future;
}

folly::Future<UnicastRoutes>
NetlinkRouteSocket::getCachedUnicastRoutes(uint8_t protocolId) const {
  VLOG(3) << "Getting unicast routes by protocol " << protocolId;

  folly::Promise<UnicastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this,
     p = std::move(promise),
     protocolId]() mutable {
    try {
      static const UnicastRoutes emptyRoutes;
      const UnicastRoutes& routes =
          folly::get_default(unicastRoutesCache_, protocolId, emptyRoutes);
      p.setValue(routes);
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error getting unicast route cache: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<MulticastRoutes>
NetlinkRouteSocket::getCachedMulticastRoutes(uint8_t protocolId) const {
  VLOG(3) << "Getting multicast routes by protocol " << protocolId;

  folly::Promise<MulticastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this,
     p = std::move(promise),
     protocolId]() mutable {
    try {
      static const MulticastRoutes emptyRoutes;
      const MulticastRoutes& routes =
          folly::get_default(mcastRoutesCache_, protocolId, emptyRoutes);
      p.setValue(routes);
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error getting route cache: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

// get cached link route by protocol ID
folly::Future<LinkRoutes>
NetlinkRouteSocket::getCachedLinkRoutes(uint8_t protocolId) const {
  VLOG(3) << "Getting link routes by protocol " << protocolId;

  folly::Promise<LinkRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this,
     p = std::move(promise),
     protocolId]() mutable {
    try {
      static const LinkRoutes emptyRoutes;
      const LinkRoutes& routes =
          folly::get_default(linkRoutesCache_, protocolId, emptyRoutes);
      p.setValue(routes);
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error getting route cache: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::syncUnicastRoutes(
  uint8_t protocolId, UnicastRoutes newRouteDb) {
  VLOG(3) << "Syncing Unicast Routes....";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        promise = std::move(promise),
        newRouteDb = std::move(newRouteDb),
        protocolId
      ]() mutable {
        try {
          doSyncUnicastRoutes(protocolId, newRouteDb);
          promise.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error syncing unicast routeDb with Fib: "
                     << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::syncLinkRoutes(
  uint8_t protocolId, LinkRoutes newRouteDb) {
  VLOG(3) << "Syncing Link Routes....";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        promise = std::move(promise),
        newRouteDb = std::move(newRouteDb),
        protocolId
      ]() mutable {
        try {
          doSyncLinkRoutes(protocolId, newRouteDb);
          promise.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error syncing link routeDb with Fib: "
                     << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::addIfAddress(fbnl::IfAddress ifAddress) {
  VLOG(3) << "Add IfAddress...";

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), addr = std::move(ifAddress)]() mutable {
        try {
          doAddIfAddress(addr.fromIfAddress());
          p.setValue();
        } catch (const std::exception& ex) {
          p.setException(ex);
        }
      });
  return future;
}

void NetlinkRouteSocket::doAddIfAddress(
  struct rtnl_addr* addr) {
  if (nullptr == addr) {
    throw NetlinkException("Can't get rtnl_addr");
  }
  int err = rtnl_addr_add(socket_, addr, 0);
  // NLE_EXIST means duplicated address
  // we treat it as success for backward compatibility
  if (NLE_SUCCESS != err && -NLE_EXIST != err) {
    throw NetlinkException(folly::sformat(
      "Failed to add address Error: {}",
      nl_geterror(err)));
  }
}

folly::Future<folly::Unit>
NetlinkRouteSocket::delIfAddress(fbnl::IfAddress ifAddress) {
  VLOG(3) << "Delete IfAddress...";

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  if (!ifAddress.getPrefix().hasValue()) {
    NetlinkException ex("Prefix must be set");
    promise.setException(std::move(ex));
    return future;
  }
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifAddr = std::move(ifAddress)]() mutable {
        struct rtnl_addr* addr = ifAddr.fromIfAddress();
        try {
          doDeleteAddr(addr);
          p.setValue();
        } catch (const std::exception& ex) {
          p.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkRouteSocket::syncIfAddress(
  int ifIndex,
  std::vector<fbnl::IfAddress> addresses,
  int family, int scope) {
  VLOG(3) << "Sync IfAddress...";

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
    [this, p = std::move(promise),
     addrs = std::move(addresses),
     ifIndex, family, scope] () mutable {
      try {
        doSyncIfAddress(ifIndex, std::move(addrs), family, scope);
        p.setValue();
      } catch (const std::exception& ex) {
        p.setException(ex);
      }
    });
  return future;
}

folly::Future<std::vector<fbnl::IfAddress>>
NetlinkRouteSocket::getIfAddrs(int ifIndex, int family, int scope) {
  VLOG(3) << "Get IfaceAddrs...";

  folly::Promise<std::vector<fbnl::IfAddress>> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
    [this, p = std::move(promise),
     ifIndex, family, scope] () mutable {
      try {
        std::vector<fbnl::IfAddress> addrs;
        doGetIfAddrs(ifIndex, family, scope, addrs);
        p.setValue(std::move(addrs));
      } catch (const std::exception& ex) {
        p.setException(ex);
      }
    });
  return future;
}

void NetlinkRouteSocket::doSyncIfAddress(
    int ifIndex, std::vector<fbnl::IfAddress> addrs, int family, int scope) {
  // Check ifindex and prefix
  std::vector<folly::CIDRNetwork> newPrefixes;
  for (const auto& addr : addrs) {
    if (addr.getIfIndex() != ifIndex) {
      throw NetlinkException("Inconsistent ifIndex in addrs");
    }
    if (!addr.getPrefix().hasValue()) {
      throw NetlinkException("Prefix must be set when sync addresses");
    }
    newPrefixes.emplace_back(addr.getPrefix().value());
  }

  std::vector<folly::CIDRNetwork> oldPrefixes;
  auto oldAddrs = getIfAddrs(ifIndex, family, scope).get();
  for (const auto& addr : oldAddrs) {
    oldPrefixes.emplace_back(addr.getPrefix().value());
  }

  PrefixCmp cmp;
  sort(newPrefixes.begin(), newPrefixes.end(), cmp);
  sort(oldPrefixes.begin(), oldPrefixes.end(), cmp);

  // get a list of prefixes need to be deleted
  std::vector<folly::CIDRNetwork> toDeletePrefixes;
  std::set_difference(oldPrefixes.begin(), oldPrefixes.end(),
                      newPrefixes.begin(), newPrefixes.end(),
                      std::inserter(toDeletePrefixes,
                      toDeletePrefixes.begin()));

  // Do add first, because in Linux deleting the only IP will cause if down.
  // Add new address, existed addresses will be ignored
  for (const auto& addr : addrs) {
    doAddIfAddress(addr.fromIfAddress());
  }

  // Delete deprecated addresses
  fbnl::IfAddressBuilder builder;
  for (const auto& toDel : toDeletePrefixes) {
    auto delAddr = builder.setIfIndex(ifIndex)
                          .setPrefix(toDel)
                          .setScope(scope)
                          .build();
    doDeleteAddr(delAddr.fromIfAddress());
  }
}

void NetlinkRouteSocket::doDeleteAddr(struct rtnl_addr* addr) {
  if (nullptr == addr) {
    throw NetlinkException("Can't get rtnl_addr");
  }
  int err = rtnl_addr_delete(socket_, addr, 0);
  // NLE_NOADDR means delete invalid address
  // we treat it as success for backward compatibility
  if (NLE_SUCCESS != err && -NLE_NOADDR != err) {
    throw NetlinkException(folly::sformat(
      "Failed to delete address Error: {}",
      nl_geterror(err)));
  }
}

void NetlinkRouteSocket::doGetIfAddrs(
  int ifIndex, int family, int scope,
  std::vector<fbnl::IfAddress>& addrs) {
  GetAddrsFuncCtx funcCtx(ifIndex, family, scope);
  auto getFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    GetAddrsFuncCtx* ctx = static_cast<GetAddrsFuncCtx*> (arg);
    struct rtnl_addr* toAdd = reinterpret_cast<struct rtnl_addr*>(obj);
    if (ctx->family != AF_UNSPEC
     && ctx->family != rtnl_addr_get_family(toAdd)) {
      return;
    }
    if (ctx->scope != RT_SCOPE_NOWHERE
     && ctx->scope != rtnl_addr_get_scope(toAdd)) {
      return;
    }
    if (ctx->ifIndex != rtnl_addr_get_ifindex(toAdd)) {
      return;
    }
    struct nl_addr* ipaddr = rtnl_addr_get_local(toAdd);
    if (!ipaddr) {
      return;
    }
    folly::IPAddress ipAddress =
      folly::IPAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(ipaddr)),
        nl_addr_get_len(ipaddr)));
    folly::CIDRNetwork prefix =
      std::make_pair(ipAddress, rtnl_addr_get_prefixlen(toAdd));
    fbnl::IfAddressBuilder ifBuilder;
    auto tmpAddr = ifBuilder.setPrefix(prefix)
                            .setIfIndex(ctx->ifIndex)
                            .setScope(ctx->scope)
                            .build();
    ctx->addrs.emplace_back(std::move(tmpAddr));
  };

  struct nl_cache* addrCache = nullptr;
  int err = rtnl_addr_alloc_cache(socket_, &addrCache);
  if (err != 0) {
    if (addrCache) {
      nl_cache_free(addrCache);
    }
    throw NetlinkException(folly::sformat(
        "Failed to allocate addr cache . Error: {}", nl_geterror(err)));
  }
  CHECK_NOTNULL(addrCache);
  nl_cache_foreach(addrCache, getFunc, &funcCtx);
  nl_cache_free(addrCache);
  funcCtx.addrs.swap(addrs);
}

folly::Future<int>
NetlinkRouteSocket::getIfIndex(const std::string& ifName) {
  folly::Promise<int> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifStr = ifName.c_str()]() mutable {
        updateLinkCacheThrottled();
        int ifIndex = rtnl_link_name2i(linkCache_, ifStr);
        p.setValue(ifIndex);
      });
  return future;
}

folly::Future<std::string>
NetlinkRouteSocket::getIfName(int ifIndex) {
  folly::Promise<std::string> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifIndex]() mutable {
        updateLinkCacheThrottled();
        std::array<char, IFNAMSIZ> ifNameBuf;
        std::string ifName(
          rtnl_link_i2name(
            linkCache_, ifIndex, ifNameBuf.data(), ifNameBuf.size()));
        p.setValue(ifName);
      });
  return future;
}

void
NetlinkRouteSocket::doAddUpdateUnicastRoute(
  uint8_t protocolId,
  const folly::CIDRNetwork& prefix,
  const NextHops& nextHops) {

  const bool isV4 = prefix.first.isV4();
  for (auto const& nextHop : nextHops) {
    CHECK_EQ(nextHop.second.isV4(), isV4);
  }

  // Create new set of nexthops to be programmed. Existing + New ones
  auto& unicastRoutes = unicastRoutesCache_[protocolId];
  static const NextHops emptyNextHops;
  const NextHops& oldNextHops =
      folly::get_default(unicastRoutes, prefix, emptyNextHops);
  // Only update if there's any difference in new nextHops
  if (oldNextHops == nextHops) {
    return;
  }

  for (auto const& nexthop : oldNextHops) {
    VLOG(2) << "existing nextHop for prefix "
            << folly::IPAddress::networkToString(prefix) << " nexthop dev "
            << std::get<0>(nexthop) << " via " << std::get<1>(nexthop).str();
  }

  for (auto const& nexthop : nextHops) {
    VLOG(2) << "new nextHop for prefix "
            << folly::IPAddress::networkToString(prefix) << " nexthop dev "
            << std::get<0>(nexthop) << " via " << std::get<1>(nexthop).str();
  }

  if (isV4) {
    doAddUpdateUnicastRouteV4(protocolId, prefix, nextHops);
  } else {
    doAddUpdateUnicastRouteV6(protocolId, prefix, nextHops, oldNextHops);
  }

  // Cache new nexthops in our local-cache if everything is good
  unicastRoutes[prefix] = nextHops;
}

void
NetlinkRouteSocket::doAddUpdateUnicastRouteV4(
  uint8_t protocolId,
  const folly::CIDRNetwork& prefix,
  const NextHops& newNextHops) {

  auto route = buildUnicastRoute(protocolId, prefix, newNextHops);
  auto err = rtnl_route_add(socket_, route->getRoutePtr(), NLM_F_REPLACE);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Could not add Route to: {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }
}

void
NetlinkRouteSocket::doAddUpdateUnicastRouteV6(
  uint8_t protocolId,
  const folly::CIDRNetwork& prefix,
  const NextHops& newNextHops,
  const NextHops& oldNextHops) {

  // We need to explicitly add new V6 routes & remove old routes
  // With IPv6, if new route being requested has different properties
  // (like gateway or metric or..) the existing one will not be replaced,
  // instead a new route will be created, which may cause underlying kernel
  // crash when releasing netdevices

  // add new nexthops
  auto toAdd = buildSetDifference(newNextHops, oldNextHops);
  if (!toAdd.empty()) {
    auto route = buildUnicastRoute(protocolId, prefix, toAdd);
    auto err = rtnl_route_add(socket_, route->getRoutePtr(), 0 /* flags */);
    if (err != 0) {
      throw NetlinkException(folly::sformat(
          "Could not add Route to: {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          nl_geterror(err)));
    }
  }

  // remove stale nexthops
  auto toDel = buildSetDifference(oldNextHops, newNextHops);
  if (!toDel.empty()) {
    auto route = buildUnicastRoute(protocolId, prefix, toDel);
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
}

void
NetlinkRouteSocket::doDeleteUnicastRoute(
  uint8_t protocolId,
  const folly::CIDRNetwork& prefix) {

  auto route = std::make_unique<NetlinkRoute>(prefix, protocolId);
  int err = rtnl_route_delete(socket_, route->getRoutePtr(), 0 /* flags */);

  // Mask off NLE_OBJ_NOTFOUND error because Netlink automatically withdraw
  // some routes when interface goes down
  if (err != 0 && nl_geterror(err) != nl_geterror(NLE_OBJ_NOTFOUND)) {
    throw NetlinkException(folly::sformat(
        "Failed to delete route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }

  // Update local cache with removed prefix
  auto& unicastRoutes = unicastRoutesCache_[protocolId];
  unicastRoutes.erase(prefix);
}

void
NetlinkRouteSocket::doAddMulticastRoute(
    uint8_t protocolId,
    const folly::CIDRNetwork& prefix,
    const std::string& ifName) {
  // Since the time we build our cache at init, virtual interfaces
  // could have been created which may have multicast routes
  // installed by the kernel
  // Hence Triggering an update here
  doUpdateRouteCache();

  auto& mcastRoutes = mcastRoutesCache_[protocolId];
  if (mcastRoutes.count(std::make_pair(prefix, ifName))) {
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
  std::unique_ptr<NetlinkRoute> route =
      buildMulticastRoute(protocolId, prefix, ifName);
  int err = rtnl_route_add(socket_, route->getRoutePtr(), 0);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to add multicast route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }

  mcastRoutes.emplace(prefix, ifName);
}

void
NetlinkRouteSocket::doDeleteMulticastRoute(
    uint8_t protocolId,
    const folly::CIDRNetwork& prefix,
    const std::string& ifName) {
  // Triggering an update here
  doUpdateRouteCache();

  auto& mcastRoutes = mcastRoutesCache_[protocolId];
  if (mcastRoutes.count(std::make_pair(prefix, ifName)) == 0) {
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
  std::unique_ptr<NetlinkRoute> route =
      buildMulticastRoute(protocolId, prefix, ifName);
  int err = rtnl_route_delete(socket_, route->getRoutePtr(), 0);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to delete multicast route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }

  mcastRoutes.erase(std::make_pair(prefix, ifName));
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
  unicastRoutesCache_.clear();
  mcastRoutesCache_.clear();
  linkRoutesCache_.clear();

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
    if ((table != RT_TABLE_MAIN) || (flags & RTM_F_CLONED)) {
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
      auto& mcastRoutes = (*(routeFuncCtx->multicastRoutesCache))[proto];
      mcastRoutes.emplace(std::make_pair(std::move(prefix), ifName));
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
      auto& linkRoutes = (*(routeFuncCtx->linkRoutesCache))[proto];
      linkRoutes.emplace(std::make_pair(std::move(prefix), ifName));
      return;
    }

    // Ideally link-local routes should never be programmed
    if (prefix.first.isLinkLocal()) {
      return;
    }

    // Check for duplicates. Only applicable for v4 case
    // For v6. Duplicate route is treated as nexthop in kernel
    auto& unicastRoutes = (*(routeFuncCtx->unicastRoutesCache))[proto];
    if (prefix.first.isV4() && unicastRoutes.count(prefix)) {
      LOG(FATAL) << "Got redundant v4 route for prefix "
                 << folly::IPAddress::networkToString(prefix)
                 << ". We shouldn't be programming duplicate routes at all.";
    }

    // our nextHop parse function called by libnl for each nextHop
    // of this route
    // These should not throw exceptions as they are libnl callbacks
    auto nextHopFunc = [](struct rtnl_nexthop * obj, void* ctx) noexcept->void {
      char ipAddrBuf2[kIpAddrBufSize];
      char ifNameBuf2[IFNAMSIZ];
      NextHopFuncCtx* nextHopFuncCtx = (NextHopFuncCtx*)ctx;

      struct rtnl_nexthop* nextHop =
          reinterpret_cast<struct rtnl_nexthop*>(obj);

      // Get the interface name from nextHop
      std::string ifName(rtnl_link_i2name(
          nextHopFuncCtx->linkCache,
          rtnl_route_nh_get_ifindex(nextHop),
          ifNameBuf2,
          sizeof(ifNameBuf2)));

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
                   << nl_addr2str(gw, ipAddrBuf2, sizeof(ipAddrBuf2));
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
    &unicastRoutesCache_,
    &mcastRoutesCache_,
    &linkRoutesCache_,
    linkCache_
  };
  nl_cache_foreach_filter(cacheV4_, nullptr, routeFunc, &routeFuncCtx);
  nl_cache_foreach_filter(cacheV6_, nullptr, routeFunc, &routeFuncCtx);
}

void
NetlinkRouteSocket::doSyncUnicastRoutes(
    uint8_t protocolId,
    const UnicastRoutes& newRouteDb) {
  // Get latest routing table from kernel and use it as our snapshot
  doUpdateRouteCache();
  auto& unicastRoutes = unicastRoutesCache_[protocolId];

  // Go over routes that are not in new routeDb, delete
  std::unordered_set<folly::CIDRNetwork> toDelete;
  for (auto const& kv : unicastRoutes) {
    if (newRouteDb.find(kv.first) == newRouteDb.end()) {
      toDelete.insert(kv.first);
    }
  }
  // Delete routes from kernel
  for (auto it = toDelete.begin(); it != toDelete.end(); ++it) {
    auto const& prefix = *it;
    try {
      doDeleteUnicastRoute(protocolId, prefix);
    } catch (std::exception const& err) {
      throw std::runtime_error(folly::sformat(
        "Could not del Route to: {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        folly::exceptionStr(err)));
    }
  }

  // Go over routes in new routeDb, update/add
  for (auto const& kv : newRouteDb) {
    auto const& prefix = kv.first;
    auto const& nextHops = kv.second;
    try {
      doAddUpdateUnicastRoute(protocolId, prefix, nextHops);
    } catch (std::exception const& err) {
      throw std::runtime_error(folly::sformat(
          "Could not update Route to: {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          folly::exceptionStr(err)));
    }
  }
}

void
NetlinkRouteSocket::doSyncLinkRoutes(
    uint8_t protocolId,
    const LinkRoutes& newRouteDb) {
  // Update linkRoutes_ with latest routes from the kernel
  doUpdateRouteCache();
  auto& linkRoutes = linkRoutesCache_[protocolId];
  const auto toDel = buildSetDifference(linkRoutes, newRouteDb);
  for (const auto& routeToDel : toDel) {
    const auto& prefix = routeToDel.first;
    const auto& ifName = routeToDel.second;
    std::unique_ptr<NetlinkRoute> route =
        buildLinkRoute(protocolId, prefix, ifName);
    int err = rtnl_route_delete(socket_, route->getRoutePtr(), 0);
    if (err != 0) {
      throw NetlinkException(folly::sformat(
          "Could not del link Route to: {} dev {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          ifName,
          nl_geterror(err)));
    }
  }

  const auto toAdd = buildSetDifference(newRouteDb, linkRoutes);
  for (const auto& routeToAdd : toAdd) {
    const auto& prefix = routeToAdd.first;
    const auto& ifName = routeToAdd.second;
    std::unique_ptr<NetlinkRoute> route =
        buildLinkRoute(protocolId, prefix, ifName);
    int err = rtnl_route_add(socket_, route->getRoutePtr(), 0);
    if (err != 0) {
      throw NetlinkException(folly::sformat(
          "Could not add link Route to: {} dev {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          ifName,
          nl_geterror(err)));
    }
  }

  linkRoutes = newRouteDb;
}

void
NetlinkRouteSocket::updateLinkCacheThrottled() {
  if (linkCache_ == nullptr or socket_ == nullptr) {
    return;
  }

  // Apply throttling
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      now - linkCacheUpdateTs_);
  if (elapsed < Constants::kNetlinkSyncThrottleInterval) {
    return;
  }
  linkCacheUpdateTs_ = now;

  // Update cache
  int ret = nl_cache_refill(socket_, linkCache_);
  if (ret != 0) {
    LOG(ERROR) << "Failed to refill link cache . Error: " << nl_geterror(ret);
  }
}

} // namespace openr
