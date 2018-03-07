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

    evl.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
      VLOG(3) << "Timeout waiting for events... ";
      evl.stop();
    });
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
    eventThread.join();

    rtnl_link_delete(socket_, link_);
    nl_cache_free(linkCache_);
    nl_socket_free(socket_);
    rtnl_link_veth_release(link_);
  }

 protected:
  std::unique_ptr<NetlinkRouteSocket> netlinkRouteSocket;
  fbzmq::ZmqEventLoop evl;
  std::thread eventThread;

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

  struct rtnl_link* link_{nullptr};
  struct nl_sock* socket_{nullptr};
  struct nl_cache* linkCache_{nullptr};
};

TEST_F(NetlinkIfFixture, EmptyRouteTest) {
  auto routes = netlinkRouteSocket->getUnicastRoutes();
  SCOPE_EXIT {
    EXPECT_EQ(0, routes.get().size());
  };
}

// - Add a route
// - verify it is added,
// - Delete it and then verify it is deleted
TEST_F(NetlinkIfFixture, SingleRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  NextHops nextHops{std::make_pair(kVethNameY, folly::IPAddress("fe80::1"))};

  // Add a route
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the same route
  netlinkRouteSocket->deleteUnicastRoute(prefix).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkIfFixture, SingleRouteTestV4) {
  const folly::CIDRNetwork prefix{folly::IPAddress("192.168.0.11"), 32};
  const NextHops nextHops{
      std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"))};

  // Add a route
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the same route
  netlinkRouteSocket->deleteUnicastRoute(prefix).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(0, routes.size());
}

// - Add a route
// - verify it is added
// - Add another path (nexthop) to the same route
// - Verify the route is updated with 2 paths
// - Delete it and then verify it is deleted
TEST_F(NetlinkIfFixture, UpdateSingleRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("fe80::1"));
  NextHops nextHops{nh1};

  // Add a route with single nextHop
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Add another nextHop nh2
  nextHops.clear();
  nextHops.insert(nh2);

  // Update the same route with new nextHop nh2
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();

  // The route should now have only nh2
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the route via both nextHops
  netlinkRouteSocket->deleteUnicastRoute(prefix).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(0, routes.size());
}

TEST_F(NetlinkIfFixture, UpdateSingleRouteTestV4) {
  const folly::CIDRNetwork prefix{folly::IPAddress("192.168.0.11"), 32};
  const auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"));
  const auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("169.254.0.2"));
  NextHops nextHops{nh1};

  // Add a route with single nextHop
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Add another nextHop nh2
  nextHops.clear();
  nextHops.insert(nh2);

  // Update the same route with new nextHop nh2
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();

  // The route should now have only nh2
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the route via both nextHops
  netlinkRouteSocket->deleteUnicastRoute(prefix).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(0, routes.size());
}

// - Add a route with 2 paths (nextHops)
// - verify it is added
// - Delete it and then verify it is deleted
TEST_F(NetlinkIfFixture, MultiRouteTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::"), 64};
  auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("fe80::1"));
  auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("fe80::1"));
  const NextHops nextHops{nh1, nh2};

  // Add a route with 2 nextHops
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the same route
  netlinkRouteSocket->deleteUnicastRoute(prefix).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
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
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();

  // Create socket2 and get routes from it
  NetlinkRouteSocket socket2(&evl);
  auto routes = socket2.getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));
}

TEST_F(NetlinkIfFixture, MultiRouteTestV4) {
  const folly::CIDRNetwork prefix{folly::IPAddress("192.168.0.11"), 32};
  const auto nh1 = std::make_pair(kVethNameY, folly::IPAddress("169.254.0.1"));
  const auto nh2 = std::make_pair(kVethNameX, folly::IPAddress("169.254.0.2"));
  const NextHops nextHops{nh1, nh2};

  // Add a route with 2 nextHops
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the same route
  netlinkRouteSocket->deleteUnicastRoute(prefix).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(0, routes.size());
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
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the path via nextHop 3
  nextHops.clear();
  nextHops.insert(nh1);
  nextHops.insert(nh2);
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();

  // The route now has nextHop 1 and nextHop 2
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Now add a new nextHop 4
  nextHops.insert(nh4);
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();

  // The route now has nextHop 1, 2, and 4
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the route
  nextHops.clear();
  netlinkRouteSocket->deleteUnicastRoute(prefix).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
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
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the path via nextHop 3
  nextHops.clear();
  nextHops.insert(nh1);
  nextHops.insert(nh2);
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();

  // The route now has nextHop 1 and nextHop 2
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Now add a new nextHop 4
  nextHops.insert(nh4);
  netlinkRouteSocket->addUnicastRoute(prefix, nextHops).get();

  // The route now has nextHop 1, 2, and 4
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(1, routes.size());
  ASSERT_EQ(1, routes.count(prefix));
  EXPECT_EQ(nextHops, routes.at(prefix));

  // Delete the route
  nextHops.clear();
  netlinkRouteSocket->deleteUnicastRoute(prefix).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
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
  netlinkRouteSocket->addMulticastRoute(prefix, kVethNameY).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(0, routes.size());

  // Try adding it back now, (it will not be added to system)
  netlinkRouteSocket->addMulticastRoute(prefix, kVethNameY).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();

  EXPECT_EQ(0, routes.size());

  // Try deleting it
  netlinkRouteSocket->deleteMulticastRoute(prefix, kVethNameY).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
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
  netlinkRouteSocket->addUnicastRoute(prefix1, nextHops1).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops1, routes.at(prefix1));

  // Add a route2
  netlinkRouteSocket->addUnicastRoute(prefix2, nextHops2).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops1, routes.at(prefix1));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  // Delete the routes
  netlinkRouteSocket->deleteUnicastRoute(prefix1).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
  EXPECT_EQ(1, routes.count(prefix2));
  EXPECT_EQ(nextHops2, routes.at(prefix2));

  netlinkRouteSocket->deleteUnicastRoute(prefix2).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
}

// Negative Tests
// - Try adding a route with invalid interface
// - Catch error and verify no route is added
TEST_F(NetlinkIfFixture, InvalidIfRouteAddTest) {
  const folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  const folly::CIDRNetwork prefixV4{folly::IPAddress("169.254.1.100"), 16};
  auto nh1 = std::make_pair("invalid-if", folly::IPAddress("fe80::1"));
  NextHops nextHops1{nh1};

  // Add a route with a invalid interface in nextHop
  EXPECT_THROW(
      netlinkRouteSocket->addUnicastRoute(prefix, nextHops1).get(),
      std::exception);

  auto nh2 = std::make_pair("", folly::IPAddress("fe80::2"));
  NextHops nextHops2{nh2};

  // Add a route without interface but using v6 link local address in nextHop
  EXPECT_THROW(
      netlinkRouteSocket->addUnicastRoute(prefix, nextHops2).get(),
      std::exception);

  auto nh3 = std::make_pair("", folly::IPAddress("169.254.0.103"));
  NextHops nextHops3{nh3};

  // Add a route without interface but using v4 link local address in nextHop
  EXPECT_THROW(
      netlinkRouteSocket->addUnicastRoute(prefixV4, nextHops3).get(),
      std::exception);

  // No routes were added
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

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
  netlinkRouteSocket->addUnicastRoute(prefix1, nextHops).get();
  auto routes = netlinkRouteSocket->getUnicastRoutes().get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  EXPECT_EQ(nextHops, routes.at(prefix1));

  // Try deleting the route with a non existing prefix2
  netlinkRouteSocket->deleteUnicastRoute(prefix2).get();

  // Delete the route
  netlinkRouteSocket->deleteUnicastRoute(prefix1).get();
  routes = netlinkRouteSocket->getUnicastRoutes().get();
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
