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

#include <openr/nl/NetlinkRouteSocket.h>

using namespace openr;
using namespace openr::fbnl;

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");

const std::chrono::milliseconds kEventLoopTimeout(2000);

// too bad 0xFB (251) is taken by gated/ospfase
// We will use this as the proto for our routes
const uint8_t kAqRouteProtoId = 99;
const uint8_t kAqRouteProtoId1 = 159;
} // namespace

// This fixture creates virtual interface (veths)
// which the UT can use to add routes (via interface)
class NetlinkIfFixture : public testing::Test {
 public:
  NetlinkIfFixture() = default;
  ~NetlinkIfFixture() override = default;

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
    netlinkRouteSocket = std::make_unique<NetlinkRouteSocket>(&evl);

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

    netlinkRouteSocket.reset();

    rtnl_link_delete(socket_, link_);
    nl_cache_free(linkCache_);
    nl_cache_free(addrCache_);
    nl_socket_free(socket_);
    rtnl_link_veth_release(link_);
  }

 protected:

   void rtnlAddrCB(void(*cb)(struct nl_object*, void*), void* arg) {
     nl_cache_refill(socket_, addrCache_);
     nl_cache_foreach_filter(addrCache_, nullptr, cb, arg);
   }

  std::unique_ptr<NetlinkRouteSocket> netlinkRouteSocket;
  fbzmq::ZmqEventLoop evl;
  std::thread eventThread;

  struct rtnl_link* link_{nullptr};
  struct nl_sock* socket_{nullptr};
  struct nl_cache* linkCache_{nullptr};
  struct nl_cache* addrCache_{nullptr};

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
};

struct AddressCallbackContext {
  struct nl_cache* linkeCache{nullptr};
  std::vector<IfAddress> results;
};

TEST_F(NetlinkIfFixture, EmptyRouteTest) {
  auto routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId);
  SCOPE_EXIT {
    EXPECT_EQ(0, std::move(routes).get().size());
  };
}

// - Add a route
// - verify it is added,
// - Delete it and then verify it is deleted
TEST_F(NetlinkIfFixture, SingleRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  NextHops nextHops{std::make_pair(kVethNameY, folly::IPAddress("fe80::1"))};

  // Add a route
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the same route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkIfFixture, SingleRouteTestV4) {
  const folly::CIDRNetwork prefix{folly::IPAddress("192.168.0.11"), 32};
  const NextHops nextHops{
      std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"))};

  // Add a route
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the same route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}


TEST_F(NetlinkIfFixture, UpdateRouteTest) {
  // - Add a route
  // - Verify it is added
  // - Add another path (nexthop) to the same route
  // - Verify the route is updated with 2 paths
  // - Delete it and then verify it is deleted
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("fe80::1"));
  NextHops nextHops{nh1};

  // Add a route with single nextHop
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix1, nextHops).get();
  auto routes =
      netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops, routes.at(prefix1));

  // Change nexthop to nh2
  nextHops.clear();
  nextHops.insert(nh2);

  // Update the same route with new nextHop nh2
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix1, nextHops).get();

  // The route should now have only nh2
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops, routes.at(prefix1));

  // Add back nexthop nh1
  nextHops.insert(nh1);
  // Update the same route with new nextHop nh1
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix1, nextHops).get();

  // The route should now have both nh1 and nh2
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops, routes.at(prefix1));

  // Delete the route via both nextHops
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix1).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  // - Add a route with 2 paths (nextHops)
  // - Verify it is added
  // - Remove one of the paths (nextHops)
  // - Verify the route is updated with 1 path
  // - Delete it and then verify it is deleted
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};

  // Add a route with 2 nextHops
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix2, nextHops).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops, routes.at(prefix2));

  // Remove 1 of nextHops from the route
  nextHops.erase(nh1);
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix2, nextHops).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops, routes.at(prefix2));

  // Delete the same route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix2).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkIfFixture, UpdateRouteTestV4) {
  // - Add a route
  // - Verify it is added
  // - Add another path (nexthop) to the same route
  // - Verify the route is updated with 2 paths
  // - Delete it and then verify it is deleted
  const folly::CIDRNetwork prefix1{folly::IPAddress("192.168.0.11"), 32};
  const auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"));
  const auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("169.254.0.2"));
  NextHops nextHops{nh1};

  // Add a route with single nextHop
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix1, nextHops).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops, routes.at(prefix1));

  // Change nexthop to nh2
  nextHops.clear();
  nextHops.insert(nh2);

  // Update the same route with new nextHop nh2
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix1, nextHops).get();

  // The route should now have only nh2
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops, routes.at(prefix1));

  // Add back nexthop nh1
  nextHops.insert(nh1);
  // Update the same route with new nextHop nh1
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix1, nextHops).get();

  // The route should now have both nh1 and nh2
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops, routes.at(prefix1));

  // Delete the route via both nextHops
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix1).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  // - Add a route with 2 paths (nextHops)
  // - Verify it is added
  // - Remove one of the paths (nextHops)
  // - Verify the route is updated with 1 path
  // - Delete it and then verify it is deleted
  const folly::CIDRNetwork prefix2{folly::IPAddress("192.168.0.12"), 32};

  // Add a route with 2 nextHops
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix2, nextHops).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops, routes.at(prefix2));

  // Remove 1 of nextHops from the route
  nextHops.erase(nh1);
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix2, nextHops).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops, routes.at(prefix2));

  // Delete the same route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix2).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

// - Add a route with 2 paths (nextHops) via socket1
// - Create socket2 and verify that those nexthops appears in socket2
TEST_F(NetlinkIfFixture, SocketRestart) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::"), 64};
  auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("fe80::1"));
  const NextHops nextHops{nh1, nh2};

  // Add a route with 2 nextHops
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();

  // Create socket2 and get routes from it
  NetlinkRouteSocket socket2(&evl);
  auto routes = socket2.getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));
}

// - Add a route with 3 paths (nextHops)
// - verify it is added
// - delete a path so it now has only 2 paths
// - verify the route is updated
// - add another path to the same route
// - verify that the route again has 3 paths
// - Delete the paths one by one to finally delete the route
// - verify it is deleted
TEST_F(NetlinkIfFixture, UpdateMultiRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::"), 64};
  auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("fe80::2"));
  auto nh3 = std::make_pair(kVethNameX, folly::IPAddress("fe80::3"));
  auto nh4 = std::make_pair(kVethNameY, folly::IPAddress("fe80::4"));
  NextHops nextHops{nh1, nh2, nh3};

  // Add a route with 3 nextHops
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the path via nextHop 3
  nextHops.clear();
  nextHops.insert(nh1);
  nextHops.insert(nh2);
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();

  // The route now has nextHop 1 and nextHop 2
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Now add a new nextHop 4
  nextHops.insert(nh4);
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();

  // The route now has nextHop 1, 2, and 4
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the route
  nextHops.clear();
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkIfFixture, UpdateMultiRouteTestV4) {
  const folly::CIDRNetwork prefix{folly::IPAddress("192.168.0.11"), 32};
  const auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"));
  const auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("169.254.0.2"));
  const auto nh3 = std::make_pair(kVethNameY, folly::IPAddress("169.254.0.3"));
  const auto nh4 = std::make_pair(kVethNameX, folly::IPAddress("169.254.0.4"));
  NextHops nextHops{nh1, nh2, nh3};

  // Add a route with 3 nextHops
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the path via nextHop 3
  nextHops.clear();
  nextHops.insert(nh1);
  nextHops.insert(nh2);
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();

  // The route now has nextHop 1 and nextHop 2
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Now add a new nextHop 4
  nextHops.insert(nh4);
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix, nextHops).get();

  // The route now has nextHop 1, 2, and 4
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the route
  nextHops.clear();
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

// Create unicast routes database
// Modify route database in various ways: change nexthops, remove prefixes.. etc
// Verify netlinkRouteSocket sync up with route db correctly
TEST_F(NetlinkIfFixture, SyncRouteTest) {
  UnicastRoutes routeDb;
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::"), 64};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:4::"), 64};
  auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("fe80::2"));
  auto nh3 = std::make_pair(kVethNameX, folly::IPAddress("fe80::3"));
  auto nh4 = std::make_pair(kVethNameY, folly::IPAddress("fe80::4"));
  NextHops nextHops1{nh1, nh2};
  NextHops nextHops2{nh3};
  routeDb[prefix1] = nextHops1;
  routeDb[prefix2] = nextHops2;

  // Sync routeDb
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDb).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops1, routes.at(prefix1));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  // Change nexthops
  nextHops1.erase(nh1);
  nextHops2.insert(nh4);
  routeDb[prefix1] = nextHops1;
  routeDb[prefix2] = nextHops2;

  // Sync routeDb
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDb).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops1, routes.at(prefix1));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  // Remove prefixes
  routeDb.erase(prefix1);
  // Sync routeDb
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDb).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  // Delete the remaining route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix2).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkIfFixture, SyncRouteTestV4) {
  UnicastRoutes routeDb;
  folly::CIDRNetwork prefix1{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2{folly::IPAddress("192.168.0.12"), 32};
  const auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"));
  const auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("169.254.0.2"));
  const auto nh3 = std::make_pair(kVethNameY, folly::IPAddress("169.254.0.3"));
  const auto nh4 = std::make_pair(kVethNameX, folly::IPAddress("169.254.0.4"));
  NextHops nextHops1{nh1, nh2};
  NextHops nextHops2{nh3};
  routeDb[prefix1] = nextHops1;
  routeDb[prefix2] = nextHops2;

  // Sync routeDb
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDb).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops1, routes.at(prefix1));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  // Change nexthops
  nextHops1.erase(nh1);
  nextHops2.insert(nh4);
  routeDb[prefix1] = nextHops1;
  routeDb[prefix2] = nextHops2;

  // Sync routeDb
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDb).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops1, routes.at(prefix1));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  // Remove prefixes
  routeDb.erase(prefix1);
  // Sync routeDb
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDb).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  // Delete the remaining route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix2).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

// - Add a mulitcast route
// - verify it is added
// - try adding it again
// - verify that duplicate route is not added
// - try deleting it
TEST_F(NetlinkIfFixture, ModifyMulticastRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("ff00::"), 8};

  // Add a route which is added to system
  netlinkRouteSocket->
      addMulticastRoute(kAqRouteProtoId, prefix, kVethNameY).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  auto mcastRoutes =
    netlinkRouteSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, mcastRoutes.size());

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(0, routes.size());

  // Try adding it back now, (it will not be added to system)
  netlinkRouteSocket->
      addMulticastRoute(kAqRouteProtoId, prefix, kVethNameY).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(0, routes.size());

  // Try deleting it
  netlinkRouteSocket->
      deleteMulticastRoute(kAqRouteProtoId, prefix, kVethNameY).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  mcastRoutes =
    netlinkRouteSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, mcastRoutes.size());
}

// - Add a unicast route with 2 paths (nextHops)
// - verify it is added
// - Add another unicast route with 2 paths (nextHops)
// - verify it is added
// - delete both routes and verify they were deleted
TEST_F(NetlinkIfFixture, MultiPathTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::"), 64};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:1::3"), 128};
  auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2 = std::make_pair(kVethNameY, folly::IPAddress("fe80::2"));
  auto nh3 = std::make_pair(kVethNameX, folly::IPAddress("fe80::3"));
  auto nh4 = std::make_pair(kVethNameX, folly::IPAddress("fe80::4"));
  NextHops nextHops1{nh1, nh2};
  NextHops nextHops2{nh3, nh4};

  // Add a route1
  netlinkRouteSocket->
      addUnicastRoute(kAqRouteProtoId, prefix1, nextHops1).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops1, routes.at(prefix1));

  // Add a route2
  netlinkRouteSocket->
      addUnicastRoute(kAqRouteProtoId, prefix2, nextHops2).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops1, routes.at(prefix1));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  // Delete the routes
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix1).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix2).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
}

// Negative Tests
// - Try adding a route with invalid interface
// - Catch error and verify no route is added
TEST_F(NetlinkIfFixture, InvalidIfRouteAddTest) {
  const folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  const folly::CIDRNetwork prefixV4{folly::IPAddress("192.168.3.3"), 16};
  auto nh1 = std::make_pair("invalid-if", folly::IPAddress("fe80::1"));
  NextHops nextHops1{nh1};

  // Add a route with a invalid interface in nextHop
  EXPECT_THROW(
      netlinkRouteSocket->
        addUnicastRoute(kAqRouteProtoId, prefix, nextHops1).get(),
      std::exception);

  auto nh2 = std::make_pair("", folly::IPAddress("fe80::2"));
  NextHops nextHops2{nh2};

  // Add a route without interface but using v6 link local address in nextHop
  EXPECT_THROW(
      netlinkRouteSocket->
        addUnicastRoute(kAqRouteProtoId, prefix, nextHops2).get(),
      std::exception);

  auto nh3 = std::make_pair("", folly::IPAddress("169.254.0.103"));
  NextHops nextHops3{nh3};

  // Add a route without interface but using v4 link local address in nextHop
  EXPECT_THROW(
      netlinkRouteSocket->
        addUnicastRoute(kAqRouteProtoId, prefixV4, nextHops3).get(),
      std::exception);

  // No routes were added
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };
}

// - Add a simple unicast route with single path
// - Verify it is added
// - Try deleting route but with an invalid path
// - Verify this return error
// - Now delete correct route and verify it is deleted
TEST_F(NetlinkIfFixture, DeleteNonExistingRouteTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
  auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("fe80::1"));
  NextHops nextHops{nh1};

  // Add a route via a single nextHop nh1
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix1, nextHops).get();
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops, routes.at(prefix1));

  // Try deleting the route with a non existing prefix2
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix2).get();

  // Delete the route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix1).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
}

// - Add different routes for different protocols
// - Verify it is added,
// - Update nh and then verify it is updated
// - Delete it and then verify it is deleted
TEST_F(NetlinkIfFixture, MultiProtocolUnicastTest) {
  // V6 routes for protocol 99
  folly::CIDRNetwork prefix1V6{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2V6{folly::IPAddress("fc00:cafe:3::4"), 128};
  auto nh1V6 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2V6 = std::make_pair(kVethNameX, folly::IPAddress("fe80::1"));
  // V4 routes for protocol 159
  folly::CIDRNetwork prefix1V4{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2V4{folly::IPAddress("192.168.0.12"), 32};
  auto nh1V4 = std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"));
  auto nh2V4 = std::make_pair(kVethNameX, folly::IPAddress("169.254.0.1"));

  NextHops nextHopsV6{nh1V6};

  // Add routes with single nextHop for protocol 99
  netlinkRouteSocket->
    addUnicastRoute(kAqRouteProtoId, prefix1V6, nextHopsV6).get();
  netlinkRouteSocket->
    addUnicastRoute(kAqRouteProtoId, prefix2V6, nextHopsV6).get();
  auto routes =
      netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix1V6));
  ASSERT_EQ(1, routes.count(prefix2V6));
  EXPECT_EQ(nextHopsV6, routes.at(prefix1V6));
  EXPECT_EQ(nextHopsV6, routes.at(prefix2V6));

  // Adde routes for protocol 159
  NextHops nextHops1V4{nh1V4};
  netlinkRouteSocket->
    addUnicastRoute(kAqRouteProtoId1, prefix1V4, nextHops1V4).get();
  netlinkRouteSocket->
    addUnicastRoute(kAqRouteProtoId1, prefix2V4, nextHops1V4).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix1V4));
  ASSERT_EQ(1, routes.count(prefix2V4));
  EXPECT_EQ(nextHops1V4, routes.at(prefix1V4));
  EXPECT_EQ(nextHops1V4, routes.at(prefix2V4));

  // Change nexthop to nh2
  nextHopsV6.clear();
  nextHopsV6.insert(nh2V6);

  // Update the same route with new nextHop nh2 for protocol 99
  netlinkRouteSocket->
    addUnicastRoute(kAqRouteProtoId, prefix1V6, nextHopsV6).get();

  // The route should now have only nh2
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix1V6));
  EXPECT_EQ(nextHopsV6, routes.at(prefix1V6));

  // Update the same route with new nextHop nh2 for protocol 159
  nextHops1V4.clear();
  nextHops1V4.insert(nh2V4);
  netlinkRouteSocket->
    addUnicastRoute(kAqRouteProtoId1, prefix2V4, nextHops1V4).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix2V4));
  EXPECT_EQ(nextHops1V4, routes.at(prefix2V4));

  // Add back nexthop nh1
  nextHopsV6.insert(nh1V6);
  // Update the same route with new nextHop nh1
  netlinkRouteSocket->
    addUnicastRoute(kAqRouteProtoId, prefix2V6, nextHopsV6).get();

  // The route should now have both nh1 and nh2
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix2V6));
  EXPECT_EQ(nextHopsV6, routes.at(prefix2V6));

  // Add back nexthop nh3
  nextHops1V4.insert(nh1V4);
  // Update the same route with new nextHop nh3
  netlinkRouteSocket->
    addUnicastRoute(kAqRouteProtoId1, prefix1V4, nextHops1V4).get();

  // The route should now have both nh3 and nh4
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix1V4));
  EXPECT_EQ(nextHops1V4, routes.at(prefix1V4));

  // Delete the route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix1V6).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix2V6).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId1, prefix1V4).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId1, prefix2V4).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());
}

// - Add different routes for different protocols
// - Verify it is added,
// - Update nh and then verify it is updated
// - Delete it and then verify it is deleted
TEST_F(NetlinkIfFixture, ModifyProtocolMulticastRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("ff00::"), 8};

  // Add routes for different protocols
  netlinkRouteSocket->
      addMulticastRoute(kAqRouteProtoId, prefix, kVethNameY).get();
  netlinkRouteSocket->
      addMulticastRoute(kAqRouteProtoId1, prefix, kVethNameX).get();

  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
  routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());

  auto mcastRoutes =
    netlinkRouteSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, mcastRoutes.size());
  mcastRoutes =
    netlinkRouteSocket->getCachedMulticastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, mcastRoutes.size());

  // Try adding it back now, (it will not be added to system)
  netlinkRouteSocket->
      addMulticastRoute(kAqRouteProtoId, prefix, kVethNameY).get();
  mcastRoutes =
    netlinkRouteSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, mcastRoutes.size());

  netlinkRouteSocket->
      addMulticastRoute(kAqRouteProtoId1, prefix, kVethNameX).get();
  mcastRoutes =
    netlinkRouteSocket->getCachedMulticastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, mcastRoutes.size());

  // Try deleting it
  netlinkRouteSocket->
      deleteMulticastRoute(kAqRouteProtoId, prefix, kVethNameY).get();
  mcastRoutes =
    netlinkRouteSocket->getCachedMulticastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, mcastRoutes.size());

  netlinkRouteSocket->
      deleteMulticastRoute(kAqRouteProtoId1, prefix, kVethNameX).get();
  mcastRoutes =
    netlinkRouteSocket->getCachedMulticastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, mcastRoutes.size());
}

TEST_F(NetlinkIfFixture, MultiProtocolSyncUnicastRouteTest) {
  // V6
  UnicastRoutes routeDbV6;
  folly::CIDRNetwork prefix1V6{folly::IPAddress("fc00:cafe:3::"), 64};
  folly::CIDRNetwork prefix2V6{folly::IPAddress("fc00:cafe:4::"), 64};
  auto nh1V6 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2V6 = std::make_pair(kVethNameX, folly::IPAddress("fe80::2"));
  auto nh3V6 = std::make_pair(kVethNameX, folly::IPAddress("fe80::3"));
  auto nh4V6 = std::make_pair(kVethNameY, folly::IPAddress("fe80::4"));
  NextHops nextHops1V6{nh1V6, nh2V6};
  NextHops nextHops2V6{nh3V6};
  routeDbV6[prefix1V6] = nextHops1V6;
  routeDbV6[prefix2V6] = nextHops2V6;
  // Pre-add unicast routes
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId, prefix1V6, nextHops2V6);
  auto routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V6));
  EXPECT_EQ(nextHops2V6, routes.at(prefix1V6));

  // V4
  UnicastRoutes routeDbV4;
  folly::CIDRNetwork prefix1V4{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2V4{folly::IPAddress("192.168.0.12"), 32};
  const auto nh1V4 =
    std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"));
  const auto nh2V4 =
    std::make_pair(kVethNameX, folly::IPAddress("169.254.0.2"));
  const auto nh3V4 =
    std::make_pair(kVethNameY, folly::IPAddress("169.254.0.3"));
  const auto nh4V4 =
    std::make_pair(kVethNameX, folly::IPAddress("169.254.0.4"));
  NextHops nextHops1V4{nh1V4, nh2V4};
  NextHops nextHops2V4{nh3V4};
  routeDbV4[prefix1V4] = nextHops1V4;
  routeDbV4[prefix2V4] = nextHops2V4;
  // Pre-add unicast routes
  netlinkRouteSocket->addUnicastRoute(kAqRouteProtoId1, prefix1V4, nextHops2V4);
  routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));
  EXPECT_EQ(nextHops2V4, routes.at(prefix1V4));

  // Sync routeDb
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDbV6).get();
  routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V6));
  EXPECT_EQ(1, routes.count(prefix2V6));
  EXPECT_EQ(nextHops1V6, routes.at(prefix1V6));
  EXPECT_EQ(nextHops2V6, routes.at(prefix2V6));

  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId1, routeDbV4).get();
  routes =
    netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));
  EXPECT_EQ(1, routes.count(prefix2V4));
  EXPECT_EQ(nextHops1V4, routes.at(prefix1V4));
  EXPECT_EQ(nextHops2V4, routes.at(prefix2V4));

  // Change nexthops V6
  nextHops1V6.erase(nh1V6);
  nextHops2V6.insert(nh4V6);
  routeDbV6[prefix1V6] = nextHops1V6;
  routeDbV6[prefix2V6] = nextHops2V6;

  // Sync routeDb V6
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDbV6).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V6));
  EXPECT_EQ(1, routes.count(prefix2V6));
  EXPECT_EQ(nextHops1V6, routes.at(prefix1V6));
  EXPECT_EQ(nextHops2V6, routes.at(prefix2V6));

  // Change nexthops V4
  nextHops1V4.erase(nh1V4);
  nextHops2V4.insert(nh4V4);
  routeDbV4[prefix1V4] = nextHops1V4;
  routeDbV4[prefix2V4] = nextHops2V4;

  // Sync routeDb V4
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId1, routeDbV4).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();

  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));
  EXPECT_EQ(1, routes.count(prefix2V4));
  EXPECT_EQ(nextHops1V4, routes.at(prefix1V4));
  EXPECT_EQ(nextHops2V4, routes.at(prefix2V4));

  // Remove V6 prefixes
  routeDbV6.erase(prefix1V6);
  // Sync routeDb
  netlinkRouteSocket->syncUnicastRoutes(kAqRouteProtoId, routeDbV6).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix2V6));
  EXPECT_EQ(nextHops2V6, routes.at(prefix2V6));

  // Delete V4 route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId1, prefix1V4).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId1, prefix2V4).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());

  // Delete V6 route
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix1V6).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  netlinkRouteSocket->deleteUnicastRoute(kAqRouteProtoId, prefix2V6).get();
  routes = netlinkRouteSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkIfFixture, MultiProtocolSyncLinkRouteTest) {
  // V6
  LinkRoutes routeDbV6;
  folly::CIDRNetwork prefix1V6{folly::IPAddress("fc00:cafe:3::"), 64};
  folly::CIDRNetwork prefix2V6{folly::IPAddress("fc00:cafe:4::"), 64};
  routeDbV6.emplace(prefix1V6, kVethNameX);
  routeDbV6.emplace(prefix2V6, kVethNameY);

  // V4
  LinkRoutes routeDbV4;
  folly::CIDRNetwork prefix1V4{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2V4{folly::IPAddress("192.168.0.12"), 32};
  routeDbV4.emplace(prefix1V4, kVethNameX);
  routeDbV4.emplace(prefix2V4, kVethNameY);

  // Sync routeDb
  netlinkRouteSocket->syncLinkRoutes(kAqRouteProtoId, routeDbV6).get();
  auto routes =
    netlinkRouteSocket->getCachedLinkRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(routeDbV6, routes);

  netlinkRouteSocket->syncLinkRoutes(kAqRouteProtoId1, routeDbV4).get();
  routes =
    netlinkRouteSocket->getCachedLinkRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(routeDbV4, routes);

  routeDbV6.clear();
  routeDbV6.emplace(prefix1V6, kVethNameY);
  routeDbV6.emplace(prefix2V6, kVethNameX);

  // Sync routeDb V6
  netlinkRouteSocket->syncLinkRoutes(kAqRouteProtoId, routeDbV6).get();
  routes = netlinkRouteSocket->getCachedLinkRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(routeDbV6, routes);

  // Change nexthops V4
  routeDbV4.clear();
  routeDbV4.emplace(prefix1V4, kVethNameY);
  routeDbV4.emplace(prefix2V4, kVethNameX);

  netlinkRouteSocket->syncLinkRoutes(kAqRouteProtoId1, routeDbV4).get();
  routes = netlinkRouteSocket->getCachedLinkRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(routeDbV4, routes);

  // Cleanup
  routeDbV6.clear();
  netlinkRouteSocket->syncLinkRoutes(kAqRouteProtoId, routeDbV6).get();
  routes = netlinkRouteSocket->getCachedLinkRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  routeDbV4.clear();
  netlinkRouteSocket->syncLinkRoutes(kAqRouteProtoId1, routeDbV4).get();
  routes = netlinkRouteSocket->getCachedLinkRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkIfFixture, IfNametoIfIndexTest) {
  int ifIndex = netlinkRouteSocket->getIfIndex(kVethNameX).get();
  std::array<char, IFNAMSIZ> ifNameBuf;
  std::string ifName(
    rtnl_link_i2name(linkCache_, ifIndex, ifNameBuf.data(), ifNameBuf.size()));
  EXPECT_EQ(rtnl_link_name2i(linkCache_, kVethNameX.c_str()), ifIndex);
  EXPECT_EQ(ifName, netlinkRouteSocket->getIfName(ifIndex).get());

  ifIndex = netlinkRouteSocket->getIfIndex(kVethNameY).get();
  std::string ifName1(
    rtnl_link_i2name(linkCache_, ifIndex, ifNameBuf.data(), ifNameBuf.size()));
  EXPECT_EQ(rtnl_link_name2i(linkCache_, kVethNameY.c_str()), ifIndex);
  EXPECT_EQ(ifName1, netlinkRouteSocket->getIfName(ifIndex).get());
}

TEST_F(NetlinkIfFixture, AddDelIfAddressBaseTest) {
  folly::CIDRNetwork prefixV6{folly::IPAddress("fc00:cafe:3::3"), 128};
  IfAddressBuilder builder;

  int ifIndex = netlinkRouteSocket->getIfIndex(kVethNameX).get();
  ASSERT_NE(0, ifIndex);
  auto ifAddr = builder.setPrefix(prefixV6)
                       .setIfIndex(ifIndex)
                       .build();

  builder.reset();
  int ifIndex1 = netlinkRouteSocket->getIfIndex(kVethNameY).get();
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
  rtnlAddrCB(addrFunc, &ctx);
  size_t before = ctx.results.size();

  // Add address
  netlinkRouteSocket->addIfAddress(std::move(ifAddr)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr1)).get();
  ctx.results.clear();
  rtnlAddrCB(addrFunc, &ctx);

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
  netlinkRouteSocket->delIfAddress(std::move(ifAddr)).get();
  netlinkRouteSocket->delIfAddress(std::move(ifAddr1)).get();

  ctx.results.clear();
  rtnlAddrCB(addrFunc, &ctx);

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

TEST_F(NetlinkIfFixture, AddDelDuplicatedIfAddressTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  IfAddressBuilder builder;

  int ifIndex = netlinkRouteSocket->getIfIndex(kVethNameX).get();
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
  rtnlAddrCB(addrFunc, &ctx);
  size_t before = ctx.results.size();

  // Add new address
  netlinkRouteSocket->addIfAddress(std::move(ifAddr)).get();
  // Add duplicated address
  netlinkRouteSocket->addIfAddress(std::move(ifAddr1)).get();

  ctx.results.clear();
  rtnlAddrCB(addrFunc, &ctx);

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
  netlinkRouteSocket->delIfAddress(std::move(ifAddr)).get();
  // double delete
  netlinkRouteSocket->delIfAddress(std::move(ifAddr1)).get();

  ctx.results.clear();
  rtnlAddrCB(addrFunc, &ctx);

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

TEST_F(NetlinkIfFixture, AddressSyncTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
  folly::CIDRNetwork prefix3{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix4{folly::IPAddress("192.168.0.12"), 32};
  folly::CIDRNetwork prefix5{folly::IPAddress("fc00:cafe:3::5"), 128};
  IfAddressBuilder builder;

  int ifIndex = netlinkRouteSocket->getIfIndex(kVethNameX).get();
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
  netlinkRouteSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr4)).get();

  AddressCallbackContext ctx;
  rtnlAddrCB(addrFunc, &ctx);

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
  EXPECT_THROW(netlinkRouteSocket->
    syncIfAddress(ifIndex, std::move(addrs)).get(), std::exception);


  // Prefix not set will throw exception
  builder.reset();
  addrs.clear();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  builder.reset();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).build());
  EXPECT_THROW(netlinkRouteSocket->
    syncIfAddress(ifIndex, std::move(addrs)).get(), std::exception);

  // Sync v6 address
  builder.reset();
  addrs.clear();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  netlinkRouteSocket->syncIfAddress(ifIndex, std::move(addrs), AF_INET6).get();

  // Sync v4 address
  builder.reset();
  addrs.clear();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix4).build());
  netlinkRouteSocket->syncIfAddress(ifIndex, std::move(addrs), AF_INET).get();

  ctx.results.clear();
  rtnlAddrCB(addrFunc, &ctx);

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
  netlinkRouteSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr5)).get();

  builder.reset();
  addrs.clear();
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix5).build());
  addrs.emplace_back(
    builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  netlinkRouteSocket->syncIfAddress(ifIndex, std::move(addrs), AF_INET6).get();

  ctx.results.clear();
  rtnlAddrCB(addrFunc, &ctx);

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

TEST_F(NetlinkIfFixture, AddressFlushTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
  folly::CIDRNetwork prefix3{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix4{folly::IPAddress("192.168.0.12"), 32};
  IfAddressBuilder builder;

  int ifIndex = netlinkRouteSocket->getIfIndex(kVethNameX).get();
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
  netlinkRouteSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr4)).get();

  AddressCallbackContext ctx;
  rtnlAddrCB(addrFunc, &ctx);

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
  netlinkRouteSocket->
    syncIfAddress(ifIndex, std::move(addrs), AF_INET6).get();
  ctx.results.clear();
  rtnlAddrCB(addrFunc, &ctx);

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
  netlinkRouteSocket->syncIfAddress(ifIndex, std::move(addrs), AF_INET).get();

  // No v4 address
  ctx.results.clear();
  rtnlAddrCB(addrFunc, &ctx);
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

TEST_F(NetlinkIfFixture, DeconsTest) {
  for (int i = 0; i < 100; ++i) {
    auto evl_ = new fbzmq::ZmqEventLoop();
    auto eventLoopThread_ = std::thread([evl = evl_]() {
      evl->run();
    });
    evl_->waitUntilRunning();
    std::unique_ptr<NetlinkRouteSocket> p =
      std::make_unique<NetlinkRouteSocket>(evl_);
      evl_->stop();
      evl_->waitUntilStopped();
      eventLoopThread_.join();
    delete evl_;
  }
}

TEST_F(NetlinkIfFixture, GetAddrsTest) {
  folly::CIDRNetwork prefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
  folly::CIDRNetwork prefix3{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix4{folly::IPAddress("192.168.0.12"), 32};
  IfAddressBuilder builder;

  int ifIndex = netlinkRouteSocket->getIfIndex(kVethNameX).get();
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
  netlinkRouteSocket->
    syncIfAddress(ifIndex, std::move(addrs)).get();

  // Add new address
  netlinkRouteSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkRouteSocket->addIfAddress(std::move(ifAddr4)).get();

  // Check get V6 address
  auto v6Addrs = netlinkRouteSocket->getIfAddrs(ifIndex, AF_INET6).get();
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
  auto v4Addrs = netlinkRouteSocket->getIfAddrs(ifIndex, AF_INET).get();
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
  auto allAddrs = netlinkRouteSocket->getIfAddrs(ifIndex).get();
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

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
