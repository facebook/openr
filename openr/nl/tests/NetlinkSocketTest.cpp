/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <map>
#include <memory>
#include <string>
#include <thread>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/MacAddress.h>
#include <folly/gen/Base.h>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

extern "C" {
#include <netlink/cache.h>
#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/link/veth.h>
#include <netlink/route/route.h>
#include <netlink/socket.h>
#include <linux/if.h>
#include <sys/ioctl.h>
}

#include <openr/nl/NetlinkSocket.h>

using namespace openr;
using namespace openr::fbnl;

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");

// too bad 0xFB (251) is taken by gated/ospfase
// We will use this as the proto for our routes
const uint8_t kAqRouteProtoId = 99;
const uint8_t kAqRouteProtoId1 = 159;
} // namespace

// This fixture creates virtual interface (veths)
// which the UT can use to add routes (via interface)
class NetlinkSocketFixture : public testing::Test {
 public:

  struct RouteCallbackContext {
    struct nl_cache* routeCache{nullptr};
    std::vector<Route> results;
  };

  struct AddressCallbackContext {
    struct nl_cache* linkeCache{nullptr};
    std::vector<IfAddress> results;
  };

  NetlinkSocketFixture() = default;
  ~NetlinkSocketFixture() override = default;

  void
  SetUp() override {
    // Not handling errors here ...
    link_ = rtnl_link_veth_alloc();
    ASSERT(link_);

    socket_ = nl_socket_alloc();
    ASSERT(socket_);
    nl_connect(socket_, NETLINK_ROUTE);

    rtnl_link_alloc_cache(socket_, AF_UNSPEC, &linkCache_);
    ASSERT(linkCache_);

    rtnl_addr_alloc_cache(socket_, &addrCache_);
    ASSERT(addrCache_);

    rtnl_route_alloc_cache(socket_, AF_UNSPEC, 0, &routeCache_);
    ASSERT(routeCache_);

    rtnl_link_set_name(link_, kVethNameX.c_str());
    rtnl_link_set_name(rtnl_link_veth_get_peer(link_), kVethNameY.c_str());
    int err = rtnl_link_add(socket_, link_, NLM_F_CREATE);
    ASSERT_EQ(0, err);

    nl_cache_refill(socket_, linkCache_);
    addAddress(kVethNameX, "169.254.0.101");
    addAddress(kVethNameY, "169.254.0.102");

    // set interface status to up
    bringUpIntf(kVethNameX);
    bringUpIntf(kVethNameY);

    // create netlink route socket
    auto handler = std::make_unique<NetlinkSocket::EventsHandler>();
    netlinkSocket = std::make_unique<NetlinkSocket>(&evl, std::move(handler));

    // Run the zmq event loop in its own thread
    // We will either timeout if expected events are not received
    // or stop after we receive expected events
    eventThread = std::thread([&]() {
      evl.run();
      evl.waitUntilStopped();
    });

    evl.waitUntilRunning();
  }

  void
  TearDown() override {
    if (evl.isRunning()) {
      evl.stop();
      eventThread.join();
    }

    netlinkSocket.reset();

    rtnl_link_delete(socket_, link_);
    nl_cache_free(linkCache_);
    nl_cache_free(addrCache_);
    nl_cache_free(routeCache_);
    nl_socket_free(socket_);
    rtnl_link_veth_release(link_);
  }

 protected:

  void rtnlCacheCB(
     void(*cb)(struct nl_object*, void*),
     void* arg, struct nl_cache* cache) {
    nl_cache_refill(socket_, cache);
    nl_cache_foreach_filter(cache, nullptr, cb, arg);
  }

  Route buildRoute(
    int ifIndex,
    int protocolId,
    const std::vector<folly::IPAddress>& nexthops,
    const folly::CIDRNetwork& dest) {

    fbnl::RouteBuilder rtBuilder;
    auto route = rtBuilder.setDestination(dest)
                          .setProtocolId(protocolId);
    fbnl::NextHopBuilder nhBuilder;
    for (const auto& nh : nexthops) {
      nhBuilder.setIfIndex(ifIndex)
               .setGateway(nh);
      rtBuilder.addNextHop(nhBuilder.build());
      nhBuilder.reset();
    }
    return rtBuilder.buildRoute();
  }

  Route buildMCastRoute(
    int ifIndex,
    int protocolId,
    const std::string& ifName,
    const folly::CIDRNetwork& dest) {
    fbnl::RouteBuilder builder;
    builder.setRouteIfIndex(ifIndex)
           .setProtocolId(protocolId)
           .setDestination(dest)
           .setRouteIfName(ifName);
    return builder.buildMulticastRoute();
  }

  Route buildLinkRoute(
    int ifIndex,
    int protocolId,
    const std::string& ifName,
    const folly::CIDRNetwork& dest) {
    fbnl::RouteBuilder builder;
    builder.setRouteIfIndex(ifIndex)
           .setProtocolId(protocolId)
           .setDestination(dest)
           .setRouteIfName(ifName);
    return builder.buildLinkRoute();
  }

  std::unique_ptr<NetlinkSocket> netlinkSocket;
  fbzmq::ZmqEventLoop evl;
  std::thread eventThread;

  struct rtnl_link* link_{nullptr};
  struct nl_sock* socket_{nullptr};
  struct nl_cache* linkCache_{nullptr};
  struct nl_cache* addrCache_{nullptr};
  struct nl_cache* routeCache_{nullptr};

 private:
  void
  addAddress(const std::string& ifName, const std::string& address) {
    int ifIndex = rtnl_link_name2i(linkCache_, ifName.c_str());
    ASSERT_NE(0, ifIndex);

    auto addrMask = std::make_pair(folly::IPAddress(address), 16);
    struct nl_addr* nlAddr = nl_addr_build(
        addrMask.first.family(),
        (void*)addrMask.first.bytes(),
        addrMask.first.byteCount());
    ASSERT(nlAddr);
    nl_addr_set_prefixlen(nlAddr, addrMask.second);

    struct rtnl_addr* addr = rtnl_addr_alloc();
    ASSERT(addr);

    rtnl_addr_set_local(addr, nlAddr);
    rtnl_addr_set_ifindex(addr, ifIndex);

    int err = rtnl_addr_add(socket_, addr, 0);
    ASSERT_EQ(0, err);
  }

  static void
  bringUpIntf(const std::string& ifName) {
    // Prepare socket
    auto sockFd = socket(PF_INET, SOCK_DGRAM, 0);
    CHECK_LT(0, sockFd);

    // Prepare request
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    folly::strlcpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ);

    // Get existing flags
    int error = ioctl(sockFd, SIOCGIFFLAGS, static_cast<void*>(&ifr));
    CHECK_EQ(0, error);

    // Mutate flags and set them back
    ifr.ifr_flags |= IFF_UP;
    error = ioctl(sockFd, SIOCSIFFLAGS, static_cast<void*>(&ifr));
    CHECK_EQ(0, error);
  }

 protected:
  bool CompareNextHops(
      std::vector<folly::IPAddress>& nexthops, const Route& route) {
    std::vector<folly::IPAddress> actual;
    for (const auto& nh : route.getNextHops()) {
      if (!nh.getGateway().hasValue()) {
        return false;
      }
      actual.push_back(nh.getGateway().value());
    }
    sort(nexthops.begin(), nexthops.end());
    sort(actual.begin(), actual.end());
    return nexthops == actual;
  }

  void doUpdateRouteTest(bool isV4) {
    // - Add a route
    // - Verify it is added
    // - Add another path (nexthop) to the same route
    // - Verify the route is updated with 2 paths
    // - Delete it and then verify it is deleted
    folly::CIDRNetwork prefix1 = isV4
                ? std::make_pair(folly::IPAddress("192.168.0.11"), 32)
                : std::make_pair(folly::IPAddress("fc00:cafe:3::3"), 128);
    std::vector<folly::IPAddress> nexthops;
    if (isV4) {
      nexthops.emplace_back(folly::IPAddress("169.254.0.1"));
    } else {
      nexthops.emplace_back(folly::IPAddress("fe80::1"));
    }
    int ifIndex = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

    auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
      RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
      struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
      RouteBuilder builder;
      if (rtnl_route_get_protocol(routeObj) == kAqRouteProtoId) {
        ctx->results.emplace_back(builder.buildFromObject(routeObj));
      }
    };

    // Add a route with single nextHop
    netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix1)).get();

    auto routes =
      netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix1));
    const Route& rt = routes.at(prefix1);
    EXPECT_EQ(1, rt.getNextHops().size());
    EXPECT_EQ(nexthops[0], rt.getNextHops().begin()->getGateway());

    // Check kernel
    RouteCallbackContext ctx;
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    int count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId
       && r.getNextHops().size() == 1
       && r.getNextHops().begin()->getGateway() == nexthops[0]) {
         count++;
      }
    }
    EXPECT_EQ(1, count);

    // Change nexthop to nh2
    nexthops.clear();
    if (isV4) {
      nexthops.emplace_back(folly::IPAddress("169.254.0.2"));
    } else {
      nexthops.emplace_back(folly::IPAddress("fe80::2"));
    }

    // Update the same route with new nextHop nh2
    netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix1)).get();

    // The route should now have only nh2
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix1));
    const Route& rt1 = routes.at(prefix1);
    EXPECT_EQ(1, rt1.getNextHops().size());
    EXPECT_EQ(nexthops[0], rt1.getNextHops().begin()->getGateway());

    // Add back nexthop nh1
    if (isV4) {
      nexthops.emplace_back(folly::IPAddress("169.254.0.1"));
    } else {
      nexthops.emplace_back(folly::IPAddress("fe80::1"));
    }
    // Update the same route with new nextHop nh1
    netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix1)).get();

    // The route should now have both nh1 and nh2
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix1));
    const Route& rt2 = routes.at(prefix1);
    EXPECT_EQ(2, rt2.getNextHops().size());

    // Check kernel
    ctx.results.clear();
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix1
      && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 2) {
        count++;
      }
    }
    EXPECT_EQ(1, count);

    // Delete the route via both nextHops
    netlinkSocket->delRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix1)).get();
      routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
      EXPECT_EQ(0, routes.size());

    // - Add a route with 2 paths (nextHops)
    // - Verify it is added
    // - Remove one of the paths (nextHops)
    // - Verify the route is updated with 1 path
    // - Delete it and then verify it is deleted
    folly::CIDRNetwork prefix2 = isV4
                ? std::make_pair(folly::IPAddress("192.168.0.12"), 32)
                : std::make_pair(folly::IPAddress("fc00:cafe:3::4"), 128);

    // Add a route with 2 nextHops
    netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix2)).get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix2));
    const Route& rt3 = routes.at(prefix2);
    EXPECT_EQ(2, rt3.getNextHops().size());

    // Remove 1 of nextHops from the route
    nexthops.pop_back();
    netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix2)).get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix2));
    const Route& rt4 = routes.at(prefix2);
    EXPECT_EQ(1, rt4.getNextHops().size());
    EXPECT_EQ(nexthops[0], rt4.getNextHops().begin()->getGateway());

    // Check kernel
    ctx.results.clear();
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId
       && r.getNextHops().size() == 1
       && r.getNextHops().begin()->getGateway() == nexthops[0]) {
         count++;
      }
    }
    EXPECT_EQ(1, count);
    // Delete the same route
    netlinkSocket->delRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix2)).get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(0, routes.size());

    ctx.results.clear();
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId
       && r.getNextHops().size() == 1
       && r.getNextHops().begin()->getGateway() == nexthops[0]) {
         count++;
      }
    }
    EXPECT_EQ(0, count);
  }

  void doUpdateMultiRouteTest(bool isV4) {
    folly::CIDRNetwork prefix = isV4
                ? std::make_pair(folly::IPAddress("192.168.0.11"), 32)
                : std::make_pair(folly::IPAddress("fc00:cafe:3::3"), 128);
    std::vector<folly::IPAddress> nexthops;
    if (isV4) {
      nexthops.emplace_back(folly::IPAddress("169.254.0.1"));
      nexthops.emplace_back(folly::IPAddress("169.254.0.2"));
      nexthops.emplace_back(folly::IPAddress("169.254.0.3"));
    } else {
      nexthops.emplace_back(folly::IPAddress("fe80::1"));
      nexthops.emplace_back(folly::IPAddress("fe80::2"));
      nexthops.emplace_back(folly::IPAddress("fe80::3"));
    }
    int ifIndex = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

    auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
      RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
      struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
      RouteBuilder builder;
      if (rtnl_route_get_protocol(routeObj) == kAqRouteProtoId) {
        ctx->results.emplace_back(builder.buildFromObject(routeObj));
      }
    };
    // Add a route with 3 nextHops
    netlinkSocket->addRoute(
    buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix)).get();
    auto routes =
      netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    EXPECT_EQ(1, routes.size());
    EXPECT_EQ(1, routes.count(prefix));
    const Route& rt = routes.at(prefix);
    EXPECT_EQ(3, rt.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops, rt));

    RouteCallbackContext ctx;
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    int count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 3) {
        count++;
      }
    }
    EXPECT_EQ(1, count);

    // Delete the path via nextHop 3
    nexthops.clear();
    if (isV4) {
      nexthops.emplace_back(folly::IPAddress("169.254.0.1"));
      nexthops.emplace_back(folly::IPAddress("169.254.0.2"));
    } else {
      nexthops.emplace_back(folly::IPAddress("fe80::1"));
      nexthops.emplace_back(folly::IPAddress("fe80::2"));
    }
    netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix)).get();

    // The route now has nextHop 1 and nextHop 2
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix));
    const Route& rt1 = routes.at(prefix);
    EXPECT_EQ(2, rt1.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops, rt1));

    ctx.results.clear();
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 2) {
        count++;
      }
    }
    EXPECT_EQ(1, count);
    // Now add a new nextHop 4
    if (isV4) {
      nexthops.emplace_back(folly::IPAddress("169.254.0.4"));
    } else {
      nexthops.emplace_back(folly::IPAddress("fe80::4"));
    }
    netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix)).get();

     // The route now has nextHop 1, 2, and 4
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix));
    const Route& rt2 = routes.at(prefix);
    EXPECT_EQ(3, rt2.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops, rt2));

    ctx.results.clear();
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 3) {
        count++;
      }
    }
    EXPECT_EQ(1, count);

    // Delete the route
    netlinkSocket->delRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix)).get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(0, routes.size());

    ctx.results.clear();
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 3) {
        count++;
      }
    }
    EXPECT_EQ(0, count);
  }

  void doSyncRouteTest(bool isV4) {
    NlUnicastRoutes routeDb;
    folly::CIDRNetwork prefix1 = isV4
                ? std::make_pair(folly::IPAddress("192.168.0.11"), 32)
                : std::make_pair(folly::IPAddress("fc00:cafe:3::"), 64);
    folly::CIDRNetwork prefix2 = isV4
                ? std::make_pair(folly::IPAddress("192.168.0.12"), 32)
                : std::make_pair(folly::IPAddress("fc00:cafe:4::"), 64);
    std::vector<folly::IPAddress> nexthops1;
    std::vector<folly::IPAddress> nexthops2;
    if (isV4) {
      nexthops1.emplace_back(folly::IPAddress("169.254.0.1"));
      nexthops1.emplace_back(folly::IPAddress("169.254.0.2"));
      nexthops2.emplace_back(folly::IPAddress("169.254.0.3"));
    } else {
      nexthops1.emplace_back(folly::IPAddress("fe80::1"));
      nexthops1.emplace_back(folly::IPAddress("fe80::2"));
      nexthops2.emplace_back(folly::IPAddress("fe80::3"));
    }
    int ifIndex = rtnl_link_name2i(linkCache_, kVethNameY.c_str());
    auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
      RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
      struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
      RouteBuilder builder;
      if (rtnl_route_get_protocol(routeObj) == kAqRouteProtoId) {
        ctx->results.emplace_back(builder.buildFromObject(routeObj));
      }
    };

    routeDb.emplace(
      prefix1, buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1));
    routeDb.emplace(
      prefix2, buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2));

    // Sync routeDb
    netlinkSocket->syncUnicastRoutes(kAqRouteProtoId, std::move(routeDb)).get();
    auto routes =
      netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    // Check in kernel
    RouteCallbackContext ctx;
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    int count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 2) {
        count++;
      }
      if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 1) {
        count++;
      }
    }
    EXPECT_EQ(2, count);

    EXPECT_EQ(1, routes.count(prefix1));
    EXPECT_EQ(1, routes.count(prefix2));
    const Route& rt1 = routes.at(prefix1);
    const Route& rt2 = routes.at(prefix2);
    EXPECT_EQ(2, rt1.getNextHops().size());
    EXPECT_EQ(1, rt2.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops1, rt1));
    EXPECT_TRUE(CompareNextHops(nexthops2, rt2));

    // Change nexthops
    nexthops1.pop_back();
    if (isV4) {
      nexthops2.emplace_back(folly::IPAddress("169.254.0.4"));
    } else {
      nexthops2.emplace_back(folly::IPAddress("fe80::4"));
    }
    routeDb.clear();
    routeDb.emplace(
      prefix1, buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1));
    routeDb.emplace(
      prefix2, buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2));

    // Sync routeDb
    netlinkSocket->syncUnicastRoutes(kAqRouteProtoId, std::move(routeDb)).get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    ctx.results.clear();
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 1) {
        count++;
      }
      if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 2) {
        count++;
      }
    }
    EXPECT_EQ(2, count);

    EXPECT_EQ(1, routes.count(prefix1));
    EXPECT_EQ(1, routes.count(prefix2));
    const Route& rt3 = routes.at(prefix1);
    const Route& rt4 = routes.at(prefix2);
    EXPECT_EQ(1, rt3.getNextHops().size());
    EXPECT_EQ(2, rt4.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops1, rt3));
    EXPECT_TRUE(CompareNextHops(nexthops2, rt4));

    // Remove prefixes
    routeDb.clear();
    routeDb.emplace(
      prefix2, buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2));
    // Sync routeDb
    netlinkSocket->syncUnicastRoutes(kAqRouteProtoId, std::move(routeDb)).get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    ctx.results.clear();
    rtnlCacheCB(routeFunc, &ctx, routeCache_);
    count = 0;
    for (const auto& r : ctx.results) {
      if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 1) {
        count++;
      }
      if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId
      && r.getNextHops().size() == 2) {
        count++;
      }
    }
    EXPECT_EQ(1, count);

    EXPECT_EQ(1, routes.count(prefix2));
    const Route& rt5 = routes.at(prefix2);
    EXPECT_EQ(2, rt5.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops2, rt5));

    // Delete the remaining route
    netlinkSocket->delRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2)).get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(0, routes.size());
  }
};

TEST_F(NetlinkSocketFixture, EmptyRouteTest) {
  auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId);
  SCOPE_EXIT {
    EXPECT_EQ(0, std::move(routes).get().size());
  };
}

// - Add a route
// - verify it is added,
// - Delete it and then verify it is deleted
TEST_F(NetlinkSocketFixture, SingleRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  std::vector<folly::IPAddress> nexthops{folly::IPAddress("fe80::1")};
  int ifIndex = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    if (rtnl_route_get_protocol(routeObj) == kAqRouteProtoId) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int before = ctx.results.size();

  // Add a route
  netlinkSocket->addRoute(
    buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix)).get();

  // Check in Kernel
  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  EXPECT_EQ(before + 1, ctx.results.size());
  int count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 1
     && r.getNextHops().begin()->getGateway() == nexthops[0]) {
       count++;
    }
  }
  EXPECT_EQ(1, count);
  auto routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  const Route& rt = routes.at(prefix);
  EXPECT_EQ(prefix, rt.getDestination());
  EXPECT_EQ(kAqRouteProtoId, rt.getProtocolId());
  EXPECT_EQ(1, rt.getNextHops().size());
  EXPECT_EQ(nexthops[0], rt.getNextHops().begin()->getGateway());

  // Delete the same route
  netlinkSocket->delRoute(
    buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkSocketFixture, SingleRouteTestV4) {
  const folly::CIDRNetwork prefix{folly::IPAddress("192.168.0.11"), 32};
  std::vector<folly::IPAddress> nexthops{folly::IPAddress("169.254.0.1")};
  int ifIndex = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    if (rtnl_route_get_protocol(routeObj) == kAqRouteProtoId) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int before = ctx.results.size();

  // Add a route
  netlinkSocket->addRoute(
    buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix)).get();

  // Check in Kernel
  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  EXPECT_EQ(before + 1, ctx.results.size());
  int count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 1
     && r.getNextHops().begin()->getGateway() == nexthops[0]) {
       count++;
    }
  }
  EXPECT_EQ(1, count);
  auto routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  const Route& rt = routes.at(prefix);
  EXPECT_EQ(prefix, rt.getDestination());
  EXPECT_EQ(kAqRouteProtoId, rt.getProtocolId());
  EXPECT_EQ(1, rt.getNextHops().size());
  EXPECT_EQ(nexthops[0], rt.getNextHops().begin()->getGateway());

  // Delete the same route
  netlinkSocket->delRoute(
    buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkSocketFixture, UpdateRouteTest) {
  doUpdateRouteTest(false);
}

TEST_F(NetlinkSocketFixture, UpdateRouteTestV4) {
  doUpdateRouteTest(true);
}

// - Add a route with 3 paths (nextHops)
// - verify it is added
// - delete a path so it now has only 2 paths
// - verify the route is updated
// - add another path to the same route
// - verify that the route again has 3 paths
// - Delete the paths one by one to finally delete the route
// - verify it is deleted
TEST_F(NetlinkSocketFixture, UpdateMultiRouteTest) {
  doUpdateMultiRouteTest(false);
}

TEST_F(NetlinkSocketFixture, UpdateMultiRouteTestV4) {
  doUpdateMultiRouteTest(true);
}

// Create unicast routes database
// Modify route database in various ways: change nexthops, remove prefixes.. etc
// Verify netlinkSocket sync up with route db correctly
TEST_F(NetlinkSocketFixture, SyncRouteTest) {
  doSyncRouteTest(false);
}

TEST_F(NetlinkSocketFixture, SyncRouteTestV4) {
  doSyncRouteTest(true);
}

// - Add a mulitcast route
// - verify it is added
// - try adding it again
// - verify that duplicate route is not added
// - try deleting it
TEST_F(NetlinkSocketFixture, ModifyMulticastRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("ff00::"), 8};
  int ifIndex = rtnl_link_name2i(linkCache_, kVethNameY.c_str());
  // Add a route which is added to system
  netlinkSocket->addRoute(
    buildMCastRoute(ifIndex, kAqRouteProtoId, kVethNameY, prefix)).get();
  auto routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  auto mcastRoutes =
    netlinkSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, mcastRoutes.size());

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(0, routes.size());

  // Check kernel
  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    if (rtnl_route_get_protocol(routeObj) == kAqRouteProtoId) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix) {
      count++;
    }
  }
  EXPECT_EQ(1, count);

  // Try adding it back now, (it will not be added to system)
  netlinkSocket->addRoute(
    buildMCastRoute(ifIndex, kAqRouteProtoId, kVethNameY, prefix)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(0, routes.size());

  // Try deleting it
  netlinkSocket->delRoute(
    buildMCastRoute(ifIndex, kAqRouteProtoId, kVethNameY, prefix)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  mcastRoutes =
    netlinkSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, mcastRoutes.size());
}

// - Add a unicast route with 2 paths (nextHops)
// - verify it is added
// - Add another unicast route with 2 paths (nextHops)
// - verify it is added
// - delete both routes and verify they were deleted
TEST_F(NetlinkSocketFixture, MultiPathTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::"), 64};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:1::3"), 128};
  std::vector<folly::IPAddress> nexthops1;
  std::vector<folly::IPAddress> nexthops2;
  nexthops1.emplace_back(folly::IPAddress("fe80::1"));
  nexthops1.emplace_back(folly::IPAddress("fe80::2"));
  nexthops2.emplace_back(folly::IPAddress("fe80::3"));
  nexthops2.emplace_back(folly::IPAddress("fe80::4"));
  int ifIndex = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

  // Add a route1
  netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1)).get();
  auto routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  const Route& rt = routes.at(prefix1);
  EXPECT_EQ(2, rt.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nexthops1, rt));

  // Add a route2
  netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(1, routes.count(prefix2));
  const Route& rt2 = routes.at(prefix1);
  const Route& rt3 = routes.at(prefix2);
  EXPECT_EQ(2, rt2.getNextHops().size());
  EXPECT_EQ(2, rt3.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nexthops1, rt2));
  EXPECT_TRUE(CompareNextHops(nexthops2, rt3));

  // Check kernel
  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    if (rtnl_route_get_protocol(routeObj) == kAqRouteProtoId) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 2) {
      count++;
    }
    if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 2) {
      count++;
    }
  }
  EXPECT_EQ(2, count);

  // Delete the routes
  netlinkSocket->delRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.count(prefix2));
  const Route& rt4 = routes.at(prefix2);
  EXPECT_EQ(2, rt4.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nexthops2, rt4));

  netlinkSocket->delRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 2) {
      count++;
    }
    if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 2) {
      count++;
    }
  }
  EXPECT_EQ(0, count);
}

// - Add a simple unicast route with single path
// - Verify it is added
// - Try deleting route but with an invalid path
// - Verify this return error
// - Now delete correct route and verify it is deleted
TEST_F(NetlinkSocketFixture, DeleteNonExistingRouteTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
  std::vector<folly::IPAddress> nexthops1{folly::IPAddress("fe80::1")};
  int ifIndex = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

  // Add a route via a single nextHop nh1
  netlinkSocket->addRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1)).get();
  auto routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    if (rtnl_route_get_protocol(routeObj) == kAqRouteProtoId) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  const Route& rt = routes.at(prefix1);
  EXPECT_EQ(1, rt.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nexthops1, rt));

  // Try deleting the route with a non existing prefix2
  netlinkSocket->delRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix2)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());

  // Check kernel
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 1) {
      count++;
    }
  }
  EXPECT_EQ(1, count);

  // Delete the route
  netlinkSocket->delRoute(
      buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 1) {
      count++;
    }
  }
  EXPECT_EQ(0, count);
}

// - Add different routes for different protocols
// - Verify it is added,
// - Update nh and then verify it is updated
// - Delete it and then verify it is deleted
TEST_F(NetlinkSocketFixture, MultiProtocolUnicastTest) {
  // V6 routes for protocol 99
  folly::CIDRNetwork prefix1V6{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2V6{folly::IPAddress("fc00:cafe:3::4"), 128};
  auto nh1V6 = folly::IPAddress("fe80::1");
  auto nh2V6 = folly::IPAddress("fe80::2");
  int ifIndexX = rtnl_link_name2i(linkCache_, kVethNameX.c_str());
  int ifIndexY = rtnl_link_name2i(linkCache_, kVethNameY.c_str());
  // V4 routes for protocol 159
  folly::CIDRNetwork prefix1V4{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2V4{folly::IPAddress("192.168.0.12"), 32};
  auto nh1V4 = folly::IPAddress("169.254.0.1");
  auto nh2V4 = folly::IPAddress("169.254.0.2");

  std::vector<folly::IPAddress> nextHopsV6{nh1V6};

  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    int protocol = rtnl_route_get_protocol(routeObj);
    if (protocol == kAqRouteProtoId || protocol == kAqRouteProtoId1) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };

  // Add routes with single nextHop for protocol 99
  netlinkSocket->addRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6)).get();
  netlinkSocket->addRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6)).get();
  auto routes =
      netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix1V6));
  ASSERT_EQ(1, routes.count(prefix2V6));
  const Route& rt1 = routes.at(prefix1V6);
  const Route& rt2 = routes.at(prefix2V6);
  EXPECT_EQ(1, rt1.getNextHops().size());
  EXPECT_EQ(1, rt2.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHopsV6, rt1));
  EXPECT_TRUE(CompareNextHops(nextHopsV6, rt2));

  // Adde routes for protocol 159
  std::vector<folly::IPAddress> nextHops1V4{nh1V4};
  netlinkSocket->addRoute(
    buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4)).get();
  netlinkSocket->addRoute(
    buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix2V4)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix1V4));
  ASSERT_EQ(1, routes.count(prefix2V4));
  const Route& rt3 = routes.at(prefix1V4);
  const Route& rt4 = routes.at(prefix2V4);
  EXPECT_EQ(1, rt3.getNextHops().size());
  EXPECT_EQ(1, rt4.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V4, rt3));
  EXPECT_TRUE(CompareNextHops(nextHops1V4, rt4));

  // Check kernel
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix1V6 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix2V6 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix1V4 && r.getProtocolId() == kAqRouteProtoId1
     && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix2V4 && r.getProtocolId() == kAqRouteProtoId1
     && r.getNextHops().size() == 1) {
      count++;
    }
  }
  EXPECT_EQ(4, count);

  // Change nexthop to nh2
  nextHopsV6.clear();
  nextHopsV6.push_back(nh2V6);

  // Update the same route with new nextHop nh2 for protocol 99
  netlinkSocket->addRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6)).get();

  // The route should now have only nh2
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V6));
  const Route& rt5 = routes.at(prefix1V6);
  EXPECT_EQ(1, rt5.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHopsV6, rt5));

  // Update the same route with new nextHop nh2 for protocol 159
  nextHops1V4.clear();
  nextHops1V4.push_back(nh2V4);
  netlinkSocket->addRoute(
    buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix2V4)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix2V4));
  const Route& rt6 = routes.at(prefix2V4);
  EXPECT_EQ(1, rt6.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V4, rt6));

  // Add back nexthop nh1
  nextHopsV6.push_back(nh1V6);
  // Update the same route with new nextHop nh1
  netlinkSocket->addRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6)).get();

  // The route should now have both nh1 and nh2
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix2V6));
  const Route& rt7 = routes.at(prefix2V6);
  EXPECT_EQ(2, rt7.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHopsV6, rt7));

  // Add back nexthop nh3
  nextHops1V4.push_back(nh1V4);
  // Update the same route with new nextHop nh3
  netlinkSocket->addRoute(
    buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4)).get();

  // The route should now have both nh3 and nh4
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));
  const Route& rt8 = routes.at(prefix1V4);
  EXPECT_EQ(2, rt8.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V4, rt8));

  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix1V6 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix2V6 && r.getProtocolId() == kAqRouteProtoId
     && r.getNextHops().size() == 2) {
      count++;
    }
    if (r.getDestination() == prefix1V4 && r.getProtocolId() == kAqRouteProtoId1
     && r.getNextHops().size() == 2) {
      count++;
    }
    if (r.getDestination() == prefix2V4 && r.getProtocolId() == kAqRouteProtoId1
     && r.getNextHops().size() == 1) {
      count++;
    }
  }
  EXPECT_EQ(4, count);

  // Delete the route
  netlinkSocket->delRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  netlinkSocket->delRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  nextHops1V4.clear();
  nextHops1V4.push_back(nh2V4);
  nextHops1V4.push_back(nh1V4);
  netlinkSocket->delRoute(
    buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  nextHops1V4.clear();
  nextHops1V4.push_back(nh2V4);
  netlinkSocket->delRoute(
    buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix2V4)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());

  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix1V6 || r.getDestination() == prefix2V6
     || r.getDestination() == prefix1V4 || r.getDestination() == prefix2V4) {
      count++;
    }
  }

  EXPECT_EQ(0, count);
}

// - Add different routes for different protocols
// - Verify it is added,
// - Update nh and then verify it is updated
// - Delete it and then verify it is deleted
TEST_F(NetlinkSocketFixture, MutiProtocolMulticastRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("ff00::"), 8};
  int ifIndexX = rtnl_link_name2i(linkCache_, kVethNameX.c_str());
  int ifIndexY = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    int protocol = rtnl_route_get_protocol(routeObj);
    if (protocol == kAqRouteProtoId || protocol == kAqRouteProtoId1) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };

  // Add routes for different protocols
  netlinkSocket->addRoute(
    buildMCastRoute(ifIndexY, kAqRouteProtoId, kVethNameY, prefix)).get();
  netlinkSocket->addRoute(
    buildMCastRoute(ifIndexX, kAqRouteProtoId1, kVethNameX, prefix)).get();

  auto routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
  routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());

  auto mcastRoutes =
    netlinkSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, mcastRoutes.size());
  mcastRoutes =
    netlinkSocket->getCachedMulticastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, mcastRoutes.size());

  // Try adding it back now, (it will not be added to system)
  netlinkSocket->addRoute(
    buildMCastRoute(ifIndexY, kAqRouteProtoId, kVethNameY, prefix)).get();
  mcastRoutes =
    netlinkSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, mcastRoutes.size());

  netlinkSocket->addRoute(
    buildMCastRoute(ifIndexX, kAqRouteProtoId1, kVethNameX, prefix)).get();
  mcastRoutes =
    netlinkSocket->getCachedMulticastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, mcastRoutes.size());

  // Check kernel
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId1) {
      count++;
    }
  }
  EXPECT_EQ(2, count);

  // Try deleting it
  netlinkSocket->delRoute(
    buildMCastRoute(ifIndexY, kAqRouteProtoId, kVethNameY, prefix)).get();
  mcastRoutes =
    netlinkSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, mcastRoutes.size());

  netlinkSocket->delRoute(
    buildMCastRoute(ifIndexY, kAqRouteProtoId1, kVethNameX, prefix)).get();
  mcastRoutes =
    netlinkSocket->getCachedMulticastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, mcastRoutes.size());

  // Check kernel
  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if (r.getDestination() == prefix && r.getProtocolId() == kAqRouteProtoId1) {
      count++;
    }
  }
  EXPECT_EQ(0, count);
}

TEST_F(NetlinkSocketFixture, MultiProtocolSyncUnicastRouteTest) {
  int ifIndexX = rtnl_link_name2i(linkCache_, kVethNameX.c_str());
  int ifIndexY = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    int protocol = rtnl_route_get_protocol(routeObj);
    if (protocol == kAqRouteProtoId || protocol == kAqRouteProtoId1) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };

  // V6
  NlUnicastRoutes routeDbV6;
  folly::CIDRNetwork prefix1V6{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2V6{folly::IPAddress("fc00:cafe:4::3"), 128};
  auto nh1V6 = folly::IPAddress("fe80::1");
  auto nh2V6 = folly::IPAddress("fe80::2");
  auto nh3V6 = folly::IPAddress("fe80::3");
  auto nh4V6 = folly::IPAddress("fe80::4");

  std::vector<folly::IPAddress> nextHops1V6{nh1V6, nh2V6};
  std::vector<folly::IPAddress> nextHops2V6{nh3V6};
  routeDbV6.emplace(
    prefix1V6, buildRoute(ifIndexX, kAqRouteProtoId, nextHops1V6, prefix1V6));
  routeDbV6.emplace(
    prefix2V6, buildRoute(ifIndexY, kAqRouteProtoId, nextHops2V6, prefix2V6));

  // Pre-add unicast routes
  netlinkSocket->addRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHops2V6, prefix1V6)).get();
  auto routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V6));

  // V4
  NlUnicastRoutes routeDbV4;
  folly::CIDRNetwork prefix1V4{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2V4{folly::IPAddress("192.168.0.12"), 32};
  auto nh1V4 = folly::IPAddress("169.254.0.1");
  auto nh2V4 = folly::IPAddress("169.254.0.2");
  auto nh3V4 = folly::IPAddress("169.254.0.3");
  auto nh4V4 = folly::IPAddress("169.254.0.4");
  std::vector<folly::IPAddress> nextHops1V4{nh1V4, nh2V4};
  std::vector<folly::IPAddress> nextHops2V4{nh3V4};
  routeDbV4.emplace(
    prefix1V4, buildRoute(ifIndexX, kAqRouteProtoId1, nextHops1V4, prefix1V4));
  routeDbV4.emplace(
    prefix2V4, buildRoute(ifIndexY, kAqRouteProtoId1, nextHops2V4, prefix2V4));

  // Pre-add unicast routes
  netlinkSocket->addRoute(
    buildRoute(ifIndexX, kAqRouteProtoId1, nextHops2V4, prefix1V4)).get();
  routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));

  // Check kernel
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix1V6
     && r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if (r.getDestination() == prefix1V4
     && r.getProtocolId() == kAqRouteProtoId1) {
      count++;
    }
  }
  EXPECT_EQ(2, count);

  // Sync routeDb
  netlinkSocket->syncUnicastRoutes(kAqRouteProtoId, std::move(routeDbV6)).get();
  routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V6));
  EXPECT_EQ(1, routes.count(prefix2V6));
  const Route& rt1 = routes.at(prefix1V6);
  const Route& rt2 = routes.at(prefix2V6);
  EXPECT_EQ(2, rt1.getNextHops().size());
  EXPECT_EQ(1, rt2.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V6, rt1));
  EXPECT_TRUE(CompareNextHops(nextHops2V6, rt2));

  netlinkSocket->syncUnicastRoutes(
    kAqRouteProtoId1, std::move(routeDbV4)).get();
  routes =
    netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));
  EXPECT_EQ(1, routes.count(prefix2V4));
  const Route& rt3 = routes.at(prefix1V4);
  const Route& rt4 = routes.at(prefix2V4);
  EXPECT_EQ(2, rt3.getNextHops().size());
  EXPECT_EQ(1, rt4.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V4, rt3));
  EXPECT_TRUE(CompareNextHops(nextHops2V4, rt4));

  // Check kernel
  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if ((r.getDestination() == prefix1V6 || r.getDestination() == prefix2V6)
     && r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if ((r.getDestination() == prefix1V4 || r.getDestination() == prefix2V4)
     && r.getProtocolId() == kAqRouteProtoId1) {
      count++;
    }
  }
  EXPECT_EQ(4, count);
  // Change nexthops V6
  nextHops1V6.pop_back();
  nextHops2V6.push_back(nh4V6);
  routeDbV6.clear();
  routeDbV6.emplace(
    prefix1V6, buildRoute(ifIndexX, kAqRouteProtoId, nextHops1V6, prefix1V6));
  routeDbV6.emplace(
    prefix2V6, buildRoute(ifIndexY, kAqRouteProtoId, nextHops2V6, prefix2V6));

  // Sync routeDb V6
  netlinkSocket->
    syncUnicastRoutes(kAqRouteProtoId, std::move(routeDbV6)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V6));
  EXPECT_EQ(1, routes.count(prefix2V6));
  const Route& rt5 = routes.at(prefix1V6);
  const Route& rt6 = routes.at(prefix2V6);
  EXPECT_EQ(1, rt5.getNextHops().size());
  EXPECT_EQ(2, rt6.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V6, rt5));
  EXPECT_TRUE(CompareNextHops(nextHops2V6, rt6));

  // Change nexthops V4
  nextHops1V4.pop_back();
  nextHops2V4.push_back(nh4V4);
  routeDbV4.clear();
  routeDbV4.emplace(
    prefix1V4, buildRoute(ifIndexX, kAqRouteProtoId1, nextHops1V4, prefix1V4));
  routeDbV4.emplace(
    prefix2V4, buildRoute(ifIndexY, kAqRouteProtoId1, nextHops2V4, prefix2V4));

  // Sync routeDb V4
  netlinkSocket->
    syncUnicastRoutes(kAqRouteProtoId1, std::move(routeDbV4)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();

  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));
  EXPECT_EQ(1, routes.count(prefix2V4));
  const Route& rt7 = routes.at(prefix1V4);
  const Route& rt8 = routes.at(prefix2V4);
  EXPECT_EQ(1, rt7.getNextHops().size());
  EXPECT_EQ(2, rt8.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V4, rt7));
  EXPECT_TRUE(CompareNextHops(nextHops2V4, rt8));

  // Remove V6 prefixes
  routeDbV6.clear();
  routeDbV6.emplace(
    prefix2V6, buildRoute(ifIndexY, kAqRouteProtoId, nextHops2V6, prefix2V6));
  // Sync routeDb
  netlinkSocket->syncUnicastRoutes(kAqRouteProtoId, std::move(routeDbV6)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix2V6));
  const Route& rt9 = routes.at(prefix2V6);
  EXPECT_EQ(2, rt9.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops2V6, rt9));

  // Check kernel
  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if (r.getDestination() == prefix2V6
     && r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
  }
  EXPECT_EQ(1, count);

  // Delete V4 route
  netlinkSocket->delRoute(
    buildRoute(ifIndexX, kAqRouteProtoId1, nextHops1V4, prefix1V4)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  nextHops2V4.clear();
  netlinkSocket->delRoute(
    buildRoute(ifIndexY, kAqRouteProtoId1, nextHops2V4, prefix2V4)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());

  // Delete V6 route
  netlinkSocket->delRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHops1V6, prefix1V6)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  nextHops2V6.clear();
  netlinkSocket->delRoute(
    buildRoute(ifIndexX, kAqRouteProtoId, nextHops2V6, prefix2V6)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  // Check kernel
  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if ((r.getDestination() == prefix1V6 || r.getDestination() == prefix2V6)
     && r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if ((r.getDestination() == prefix1V4 || r.getDestination() == prefix2V4)
     && r.getProtocolId() == kAqRouteProtoId1) {
      count++;
    }
  }
  EXPECT_EQ(0, count);
}

TEST_F(NetlinkSocketFixture, MultiProtocolSyncLinkRouteTest) {
  auto routeFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    RouteCallbackContext* ctx = static_cast<RouteCallbackContext*> (arg);
    struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
    RouteBuilder builder;
    int protocol = rtnl_route_get_protocol(routeObj);
    if (protocol == kAqRouteProtoId || protocol == kAqRouteProtoId1) {
      ctx->results.emplace_back(builder.buildFromObject(routeObj));
    }
  };

  // V6
  NlLinkRoutes routeDbV6;
  folly::CIDRNetwork prefix1V6{folly::IPAddress("fc00:cafe:3::"), 64};
  folly::CIDRNetwork prefix2V6{folly::IPAddress("fc00:cafe:4::"), 64};
  int ifIndexX = rtnl_link_name2i(linkCache_, kVethNameX.c_str());
  int ifIndexY = rtnl_link_name2i(linkCache_, kVethNameY.c_str());
  routeDbV6.emplace(std::make_pair(prefix1V6, kVethNameX),
    buildLinkRoute(ifIndexX, kAqRouteProtoId, kVethNameX, prefix1V6));
  routeDbV6.emplace(std::make_pair(prefix2V6, kVethNameY),
    buildLinkRoute(ifIndexY, kAqRouteProtoId, kVethNameY, prefix2V6));

  // V4
  NlLinkRoutes routeDbV4;
  folly::CIDRNetwork prefix1V4{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2V4{folly::IPAddress("192.168.0.12"), 32};
  routeDbV4.emplace(std::make_pair(prefix1V4, kVethNameX),
    buildLinkRoute(ifIndexX, kAqRouteProtoId1, kVethNameX, prefix1V4));
  routeDbV4.emplace(std::make_pair(prefix2V4, kVethNameY),
    buildLinkRoute(ifIndexY, kAqRouteProtoId1, kVethNameY, prefix2V4));

  // Sync routeDb
  netlinkSocket->syncLinkRoutes(kAqRouteProtoId, std::move(routeDbV6)).get();
  auto routes =
    netlinkSocket->getCachedLinkRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(std::make_pair(prefix1V6, kVethNameX)));
  EXPECT_EQ(1, routes.count(std::make_pair(prefix2V6, kVethNameY)));

  netlinkSocket->syncLinkRoutes(kAqRouteProtoId1, std::move(routeDbV4)).get();
  routes =
    netlinkSocket->getCachedLinkRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(std::make_pair(prefix1V4, kVethNameX)));
  EXPECT_EQ(1, routes.count(std::make_pair(prefix2V4, kVethNameY)));

  // Check kernel
  RouteCallbackContext ctx;
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  int count = 0;
  for (const auto& r : ctx.results) {
    if ((r.getDestination() == prefix1V6 || r.getDestination() == prefix2V6)
     && r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if ((r.getDestination() == prefix1V4 || r.getDestination() == prefix2V4)
     && r.getProtocolId() == kAqRouteProtoId1) {
      count++;
    }
  }
  EXPECT_EQ(4, count);

  // Cleanup
  routeDbV6.clear();
  netlinkSocket->syncLinkRoutes(kAqRouteProtoId, std::move(routeDbV6)).get();
  routes = netlinkSocket->getCachedLinkRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  routeDbV4.clear();
  netlinkSocket->syncLinkRoutes(kAqRouteProtoId1, std::move(routeDbV4)).get();
  routes = netlinkSocket->getCachedLinkRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());

  ctx.results.clear();
  rtnlCacheCB(routeFunc, &ctx, routeCache_);
  count = 0;
  for (const auto& r : ctx.results) {
    if ((r.getDestination() == prefix1V6 || r.getDestination() == prefix2V6)
     && r.getProtocolId() == kAqRouteProtoId
     && r.getScope() == RT_SCOPE_LINK) {
      count++;
    }
    if ((r.getDestination() == prefix1V4 || r.getDestination() == prefix2V4)
     && r.getProtocolId() == kAqRouteProtoId1
     && r.getScope() == RT_SCOPE_LINK) {
      count++;
    }
  }
  EXPECT_EQ(0, count);
}

TEST_F(NetlinkSocketFixture, IfNametoIfIndexTest) {
  int ifIndex = netlinkSocket->getIfIndex(kVethNameX).get();
  std::array<char, IFNAMSIZ> ifNameBuf;
  std::string ifName(
    rtnl_link_i2name(linkCache_, ifIndex, ifNameBuf.data(), ifNameBuf.size()));
  EXPECT_EQ(rtnl_link_name2i(linkCache_, kVethNameX.c_str()), ifIndex);
  EXPECT_EQ(ifName, netlinkSocket->getIfName(ifIndex).get());

  ifIndex = netlinkSocket->getIfIndex(kVethNameY).get();
  std::string ifName1(
    rtnl_link_i2name(linkCache_, ifIndex, ifNameBuf.data(), ifNameBuf.size()));
  EXPECT_EQ(rtnl_link_name2i(linkCache_, kVethNameY.c_str()), ifIndex);
  EXPECT_EQ(ifName1, netlinkSocket->getIfName(ifIndex).get());
}

TEST_F(NetlinkSocketFixture, AddDelIfAddressBaseTest) {
  folly::CIDRNetwork prefixV6{folly::IPAddress("fc00:cafe:3::3"), 128};
  IfAddressBuilder builder;

  int ifIndex = netlinkSocket->getIfIndex(kVethNameX).get();
  ASSERT_NE(0, ifIndex);
  auto ifAddr = builder.setPrefix(prefixV6)
                       .setIfIndex(ifIndex)
                       .build();

  builder.reset();
  int ifIndex1 = netlinkSocket->getIfIndex(kVethNameY).get();
  const folly::CIDRNetwork prefixV4{folly::IPAddress("192.168.0.11"), 32};
  auto ifAddr1 = builder.setPrefix(prefixV4)
                        .setIfIndex(ifIndex1)
                        .build();

  auto addrFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    AddressCallbackContext* ctx = static_cast<AddressCallbackContext*> (arg);
    struct rtnl_addr* addr = reinterpret_cast<struct rtnl_addr*>(obj);
    struct nl_addr* ipaddr = rtnl_addr_get_local(addr);
    if (!ipaddr) {
      return;
    }
    folly::IPAddress ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(ipaddr)),
        nl_addr_get_len(ipaddr)));
    folly::CIDRNetwork prefix =
      std::make_pair(ipAddress, rtnl_addr_get_prefixlen(addr));
    IfAddressBuilder ifBuilder;
    auto tmpAddr = ifBuilder.setPrefix(prefix)
                            .setIfIndex(rtnl_addr_get_ifindex(addr))
                            .build();
    ctx->results.emplace_back(std::move(tmpAddr));
  };
  AddressCallbackContext ctx;
  rtnlCacheCB(addrFunc, &ctx, addrCache_);
  size_t before = ctx.results.size();

  // Add address
  netlinkSocket->addIfAddress(std::move(ifAddr)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();
  ctx.results.clear();
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  // two more addresses
  EXPECT_EQ(before + 2, ctx.results.size());

  int cnt = 0;
  for (const auto& ret : ctx.results) {
    if (ret.getPrefix() == prefixV6 && ret.getIfIndex() == ifIndex) {
      cnt++;
    } else if (ret.getPrefix() == prefixV4
            && ret.getIfIndex() == ifIndex1) {
      cnt++;
    }
  }
  EXPECT_EQ(2, cnt);

  // delete addresses
  ifAddr = builder.setPrefix(prefixV6)
                  .setIfIndex(ifIndex)
                  .build();
  builder.reset();
  ifAddr1 = builder.setPrefix(prefixV4)
                   .setIfIndex(ifIndex1)
                   .build();
  netlinkSocket->delIfAddress(std::move(ifAddr)).get();
  netlinkSocket->delIfAddress(std::move(ifAddr1)).get();

  ctx.results.clear();
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  EXPECT_EQ(before, ctx.results.size());

  bool found = false;
  for (const auto& ret : ctx.results) {
    // No more added address
    if ((ret.getPrefix() == prefixV6 && ret.getIfIndex() == ifIndex)
    || (ret.getPrefix() == prefixV4 && ret.getIfIndex() == ifIndex1)) {
       found = true;
     }
   }
   EXPECT_FALSE(found);
}

TEST_F(NetlinkSocketFixture, AddDelDuplicatedIfAddressTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  IfAddressBuilder builder;

  int ifIndex = netlinkSocket->getIfIndex(kVethNameX).get();
  ASSERT_NE(0, ifIndex);
  auto ifAddr = builder.setPrefix(prefix)
                       .setIfIndex(ifIndex)
                       .build();
  builder.reset();
  auto ifAddr1 = builder.setPrefix(prefix)
                        .setIfIndex(ifIndex)
                        .build();

  auto addrFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    AddressCallbackContext* ctx = static_cast<AddressCallbackContext*> (arg);
    struct rtnl_addr* addr = reinterpret_cast<struct rtnl_addr*>(obj);
    struct nl_addr* ipaddr = rtnl_addr_get_local(addr);
    if (!ipaddr) {
      return;
    }
    folly::IPAddress ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(ipaddr)),
        nl_addr_get_len(ipaddr)));
    folly::CIDRNetwork tmpPrefix =
      std::make_pair(ipAddress, rtnl_addr_get_prefixlen(addr));
    IfAddressBuilder ifBuilder;
    auto tmpAddr = ifBuilder.setPrefix(tmpPrefix)
                            .setIfIndex(rtnl_addr_get_ifindex(addr))
                            .build();
    ctx->results.emplace_back(std::move(tmpAddr));
  };
  AddressCallbackContext ctx;
  rtnlCacheCB(addrFunc, &ctx, addrCache_);
  size_t before = ctx.results.size();

  // Add new address
  netlinkSocket->addIfAddress(std::move(ifAddr)).get();
  // Add duplicated address
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();

  ctx.results.clear();
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  // one more addresse
  EXPECT_EQ(before + 1, ctx.results.size());

  int cnt = 0;
  for (const auto& ret : ctx.results) {
    if (ret.getPrefix() == prefix&& ret.getIfIndex() == ifIndex) {
      cnt++;
    }
  }
  EXPECT_EQ(1, cnt);

  // delete addresses
  ifAddr = builder.setPrefix(prefix)
                  .setIfIndex(ifIndex)
                  .build();
  builder.reset();
  ifAddr1 = builder.setPrefix(prefix)
                   .setIfIndex(ifIndex)
                   .build();
  netlinkSocket->delIfAddress(std::move(ifAddr)).get();
  // double delete
  netlinkSocket->delIfAddress(std::move(ifAddr1)).get();

  ctx.results.clear();
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  EXPECT_EQ(before, ctx.results.size());

  bool found = false;
  for (const auto& ret : ctx.results) {
    // No more added address
    if ((ret.getPrefix() == prefix && ret.getIfIndex() == ifIndex)) {
       found = true;
     }
   }
   EXPECT_FALSE(found);
}

TEST_F(NetlinkSocketFixture, AddressSyncTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
  folly::CIDRNetwork prefix3{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix4{folly::IPAddress("192.168.0.12"), 32};
  folly::CIDRNetwork prefix5{folly::IPAddress("fc00:cafe:3::5"), 128};
  IfAddressBuilder builder;

  int ifIndex = netlinkSocket->getIfIndex(kVethNameX).get();
  ASSERT_NE(0, ifIndex);
  auto ifAddr1 = builder.setPrefix(prefix1)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr2 = builder.setPrefix(prefix2)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr3 = builder.setPrefix(prefix3)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr4 = builder.setPrefix(prefix4)
                        .setIfIndex(ifIndex)
                        .build();

  auto addrFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    AddressCallbackContext* ctx = static_cast<AddressCallbackContext*> (arg);
    struct rtnl_addr* addr = reinterpret_cast<struct rtnl_addr*>(obj);
    struct nl_addr* ipaddr = rtnl_addr_get_local(addr);
    if (!ipaddr) {
      return;
    }
    folly::IPAddress ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(ipaddr)),
        nl_addr_get_len(ipaddr)));
    folly::CIDRNetwork tmpPrefix =
      std::make_pair(ipAddress, rtnl_addr_get_prefixlen(addr));
    IfAddressBuilder ifBuilder;
    auto tmpAddr = ifBuilder.setPrefix(tmpPrefix)
                            .setIfIndex(rtnl_addr_get_ifindex(addr))
                            .setScope(rtnl_addr_get_scope(addr))
                            .build();
    ctx->results.emplace_back(std::move(tmpAddr));
  };

  // Add new address
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr4)).get();

  AddressCallbackContext ctx;
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  bool p1 = false, p2 = false, p3 = false, p4 = false, p5 = false;
  // Check contains all prefix
  for (const auto& ret : ctx.results) {
    if (ret.getPrefix() == prefix1) {
      p1 = true;
    } else if (ret.getPrefix() == prefix2) {
      p2 = true;
    } else if (ret.getPrefix() == prefix3) {
      p3 = true;
    } else if (ret.getPrefix() == prefix4) {
      p4 = true;
    }
  }
  EXPECT_TRUE(p1);
  EXPECT_TRUE(p2);
  EXPECT_TRUE(p3);
  EXPECT_TRUE(p4);

  std::vector<IfAddress> addrs;

  // Different ifindex will throw exception
  builder.reset();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex + 1).setPrefix(prefix1).build());
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix2).build());
  EXPECT_THROW(netlinkSocket->
    syncIfAddress(ifIndex, std::move(addrs), AF_UNSPEC, RT_SCOPE_NOWHERE).get(),
    std::exception);

  // Prefix not set will throw exception
  builder.reset();
  addrs.clear();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  builder.reset();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).build());
  EXPECT_THROW(netlinkSocket->
    syncIfAddress(ifIndex, std::move(addrs), AF_UNSPEC, RT_SCOPE_NOWHERE).get(),
    std::exception);

  // Sync v6 address
  builder.reset();
  addrs.clear();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  netlinkSocket->syncIfAddress(
    ifIndex, std::move(addrs), AF_INET6, RT_SCOPE_NOWHERE).get();

  // Sync v4 address
  builder.reset();
  addrs.clear();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix4).build());
  netlinkSocket->syncIfAddress(
    ifIndex, std::move(addrs), AF_INET, RT_SCOPE_NOWHERE).get();

  ctx.results.clear();
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  // Check no prefix2, prefix3
  p1 = false, p2 = false, p3 = false, p4 = false;
  for (const auto& ret : ctx.results) {
    if (ret.getPrefix() == prefix1) {
      p1 = true;
    } else if (ret.getPrefix() == prefix2) {
      p2 = true;
    } else if (ret.getPrefix() == prefix3) {
      p3 = true;
    } else if (ret.getPrefix() == prefix4) {
      p4 = true;
    }
  }
  EXPECT_TRUE(p1);
  EXPECT_FALSE(p2);
  EXPECT_FALSE(p3);
  EXPECT_TRUE(p4);

  builder.reset();
  ifAddr2 = builder.setPrefix(prefix2)
                   .setIfIndex(ifIndex)
                   .build();
  builder.reset();
  auto ifAddr5 = builder.setPrefix(prefix5)
                        .setIfIndex(ifIndex)
                        .build();
  netlinkSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr5)).get();

  builder.reset();
  addrs.clear();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix5).build());
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  netlinkSocket->syncIfAddress(
    ifIndex, std::move(addrs), AF_INET6, RT_SCOPE_NOWHERE).get();

  ctx.results.clear();
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  // Check no prefix2, prefix3
  p1 = false, p2 = false, p3 = false, p4 = false;
  for (const auto& ret : ctx.results) {
    if (ret.getPrefix() == prefix1) {
      p1 = true;
    } else if (ret.getPrefix() == prefix2) {
      p2 = true;
    } else if (ret.getPrefix() == prefix3) {
      p3 = true;
    } else if (ret.getPrefix() == prefix4) {
      p4 = true;
    } else if (ret.getPrefix() == prefix5) {
      p5 = true;
    }
  }
  EXPECT_TRUE(p1);
  EXPECT_FALSE(p2);
  EXPECT_FALSE(p3);
  EXPECT_TRUE(p4);
  EXPECT_TRUE(p5);
}

TEST_F(NetlinkSocketFixture, AddressFlushTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
  folly::CIDRNetwork prefix3{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix4{folly::IPAddress("192.168.0.12"), 32};
  IfAddressBuilder builder;

  int ifIndex = netlinkSocket->getIfIndex(kVethNameX).get();
  ASSERT_NE(0, ifIndex);
  auto ifAddr1 = builder.setPrefix(prefix1)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr2 = builder.setPrefix(prefix2)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr3 = builder.setPrefix(prefix3)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr4 = builder.setPrefix(prefix4)
                        .setIfIndex(ifIndex)
                        .build();

  auto addrFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    AddressCallbackContext* ctx = static_cast<AddressCallbackContext*> (arg);
    struct rtnl_addr* addr = reinterpret_cast<struct rtnl_addr*>(obj);
    struct nl_addr* ipaddr = rtnl_addr_get_local(addr);
    if (!ipaddr) {
      return;
    }
    folly::IPAddress ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(ipaddr)),
        nl_addr_get_len(ipaddr)));
    folly::CIDRNetwork tmpPrefix =
      std::make_pair(ipAddress, rtnl_addr_get_prefixlen(addr));
    IfAddressBuilder ifBuilder;
    auto tmpAddr = ifBuilder.setPrefix(tmpPrefix)
                            .setIfIndex(rtnl_addr_get_ifindex(addr))
                            .setScope(rtnl_addr_get_scope(addr))
                            .build();
    ctx->results.emplace_back(std::move(tmpAddr));
  };

  // Add new address
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr4)).get();

  AddressCallbackContext ctx;
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  int ipV4cnt = 0, ipV6cnt = 0;
  for (const auto& ret : ctx.results) {
    if (ret.getFamily() == AF_INET && ret.getIfIndex() == ifIndex) {
      ipV4cnt++;
    }
    if (ret.getFamily() == AF_INET6 && ret.getIfIndex() == ifIndex) {
      ipV6cnt++;
    }
  }

  // delete IPV6 address family
  std::vector<IfAddress> addrs;
  netlinkSocket->
    syncIfAddress(ifIndex, std::move(addrs), AF_INET6, RT_SCOPE_NOWHERE).get();
  ctx.results.clear();
  rtnlCacheCB(addrFunc, &ctx, addrCache_);

  // No V6 address found
  int actualV4cnt = 0, actualV6cnt = 0;
  for (const auto& ret : ctx.results) {
    // No more added address
    if (ret.getFamily() == AF_INET && ret.getIfIndex() == ifIndex) {
      actualV4cnt++;
     }
    if (ret.getFamily() == AF_INET6 && ret.getIfIndex() == ifIndex
      && ret.getScope().value() == RT_SCOPE_UNIVERSE) {
      actualV6cnt++;
     }
  }
  EXPECT_EQ(ipV4cnt, actualV4cnt);
  EXPECT_EQ(0, actualV6cnt);

  addrs.clear();
  netlinkSocket->syncIfAddress(
    ifIndex, std::move(addrs), AF_INET, RT_SCOPE_NOWHERE).get();

  // No v4 address
  ctx.results.clear();
  rtnlCacheCB(addrFunc, &ctx, addrCache_);
  actualV4cnt = 0, actualV6cnt = 0;
  for (const auto& ret : ctx.results) {
    // No more added address
    if (ret.getFamily() == AF_INET && ret.getIfIndex() == ifIndex) {
      actualV4cnt++;
     }
    if (ret.getFamily() == AF_INET6 && ret.getIfIndex() == ifIndex
     && ret.getScope().value() == RT_SCOPE_UNIVERSE) {
      actualV6cnt++;
     }
  }
  EXPECT_EQ(0, actualV4cnt);
  EXPECT_EQ(0, actualV6cnt);
}

TEST_F(NetlinkSocketFixture, GetAddrsTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
  folly::CIDRNetwork prefix3{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix4{folly::IPAddress("192.168.0.12"), 32};
  IfAddressBuilder builder;

  int ifIndex = netlinkSocket->getIfIndex(kVethNameX).get();
  ASSERT_NE(0, ifIndex);
  auto ifAddr1 = builder.setPrefix(prefix1)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr2 = builder.setPrefix(prefix2)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr3 = builder.setPrefix(prefix3)
                        .setIfIndex(ifIndex)
                        .build();
  builder.reset();
  auto ifAddr4 = builder.setPrefix(prefix4)
                        .setIfIndex(ifIndex)
                        .build();

  // Cleaup all address
  std::vector<IfAddress> addrs;
  netlinkSocket->
    syncIfAddress(ifIndex, std::move(addrs), AF_UNSPEC, RT_SCOPE_NOWHERE).get();

  // Add new address
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr4)).get();

  // Check get V6 address
  auto v6Addrs = netlinkSocket->
    getIfAddrs(ifIndex, AF_INET6, RT_SCOPE_NOWHERE).get();
  EXPECT_EQ(2, v6Addrs.size());
  bool p1 = false, p2 = false;
  for (const auto& addr : v6Addrs) {
    if (addr.getPrefix().value() == prefix1) {
      p1 = true;
    }
    if (addr.getPrefix().value() == prefix2) {
      p2 = true;
    }
  }
  EXPECT_TRUE(p1);
  EXPECT_TRUE(p2);

  // Check get V4 address
  auto v4Addrs = netlinkSocket->
    getIfAddrs(ifIndex, AF_INET, RT_SCOPE_NOWHERE).get();
  EXPECT_EQ(2, v4Addrs.size());
  bool p3 = false, p4 = false;
  for (const auto& addr : v4Addrs) {
    if (addr.getPrefix().value() == prefix3) {
      p3 = true;
    }
    if (addr.getPrefix().value() == prefix4) {
      p4 = true;
    }
  }
  EXPECT_TRUE(p3);
  EXPECT_TRUE(p4);

  // CHeck get all address
  auto allAddrs = netlinkSocket->
    getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();
  EXPECT_EQ(4, allAddrs.size());
  p1 = false, p2 = false, p3 = false, p4 = false;
  for (const auto& addr : allAddrs) {
    if (addr.getPrefix().value() == prefix1) {
      p1 = true;
    }
    if (addr.getPrefix().value() == prefix2) {
      p2 = true;
    }
    if (addr.getPrefix().value() == prefix3) {
      p3 = true;
    }
    if (addr.getPrefix().value() == prefix4) {
      p4 = true;
    }
  }
  EXPECT_TRUE(p1);
  EXPECT_TRUE(p2);
  EXPECT_TRUE(p3);
  EXPECT_TRUE(p4);
}

int main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
