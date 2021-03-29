/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/ScopeGuard.h>
#include <folly/Subprocess.h>
#include <folly/gen/Base.h>
#include <folly/io/async/EventBase.h>
#include <folly/system/Shell.h>
#include <folly/test/TestUtils.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <openr/nl/NetlinkSocket.h>

extern "C" {
#include <net/if.h>
}

using namespace openr;
using namespace openr::fbnl;
using namespace folly::literals::shell_literals;

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");

// too bad 0xFB (251) is taken by gated/ospfase
// We will use this as the proto for our routes
const uint8_t kAqRouteProtoId = 99;
const uint8_t kAqRouteProtoId1 = 159;
const uint32_t kAqRouteProtoIdPriority = 10;
const uint32_t kAqRouteProtoId1Priority = 255;

} // namespace

//
// [TO BE DEPRECATED] Migrating all NetlinkSocketTest towards
// NetlinkProtocolSocketTest and remove `NetlinkSocket` wrapper.
//

// This fixture creates virtual interface (veths)
// which the UT can use to add routes (via interface)
class NetlinkSocketFixture : public testing::Test {
 public:
  NetlinkSocketFixture() = default;
  ~NetlinkSocketFixture() override = default;

  void
  SetUp() override {
    if (getuid()) {
      SKIP() << "Must run this test as root";
      return;
    }

    // cleanup old interfaces in any
    auto cmd = "ip link del {}"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();

    // add veth interface pair
    cmd = "ip link add {} type veth peer name {}"_shellify(
        kVethNameX.c_str(), kVethNameY.c_str());
    folly::Subprocess proc1(std::move(cmd));
    EXPECT_EQ(0, proc1.wait().exitStatus());

    addAddress(kVethNameX, "169.254.0.101");
    addAddress(kVethNameY, "169.254.0.102");
    addAddress(kVethNameY, "169.254.0.1");
    addAddress(kVethNameY, "169.254.0.2");
    addAddress(kVethNameY, "169.254.0.3");
    addAddress(kVethNameY, "169.254.0.4");

    // set interface status to up
    bringUpIntf(kVethNameX);
    bringUpIntf(kVethNameY);

    nlProtocolSocket = std::make_unique<openr::fbnl::NetlinkProtocolSocket>(
        &evb, netlinkEventsQ);
    nlProtocolSocketThread = std::thread([&]() { evb.loopForever(); });
    evb.waitUntilRunning();

    // create netlink route socket
    netlinkSocket =
        std::make_unique<NetlinkSocket>(&evl, std::move(nlProtocolSocket));

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
    if (getuid()) {
      // Nothing to cleanup if not-root
      return;
    }

    if (evl.isRunning()) {
      evl.stop();
      eventThread.join();
    }
    if (evb.isRunning()) {
      evb.terminateLoopSoon();
      nlProtocolSocketThread.join();
    }

    netlinkSocket.reset();

    // cleanup virtual interfaces
    auto cmd = "ip link del {} 2>/dev/null"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();
  }

 protected:
  Route
  buildRoute(
      int ifIndex,
      int protocolId,
      const std::vector<folly::IPAddress>& nexthops,
      const folly::CIDRNetwork& dest) {
    fbnl::RouteBuilder rtBuilder;
    auto route = rtBuilder.setDestination(dest).setProtocolId(protocolId);
    fbnl::NextHopBuilder nhBuilder;
    for (const auto& nh : nexthops) {
      nhBuilder.setIfIndex(ifIndex).setGateway(nh);
      rtBuilder.addNextHop(nhBuilder.build());
      nhBuilder.reset();
    }
    if (protocolId == kAqRouteProtoId) {
      rtBuilder.setPriority(kAqRouteProtoIdPriority);
    } else if (protocolId == kAqRouteProtoId1) {
      rtBuilder.setPriority(kAqRouteProtoId1Priority);
    }
    return rtBuilder.build();
  }

  std::unique_ptr<NetlinkSocket> netlinkSocket;
  std::unique_ptr<openr::fbnl::NetlinkProtocolSocket> nlProtocolSocket;
  messaging::ReplicateQueue<openr::fbnl::NetlinkEvent> netlinkEventsQ;

  fbzmq::ZmqEventLoop evl;
  folly::EventBase evb;
  std::thread eventThread;
  std::thread nlProtocolSocketThread;

 private:
  void
  addAddress(const std::string& ifName, const std::string& address) {
    auto cmd =
        "ip addr add {} dev {}"_shellify(address.c_str(), ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  static void
  bringUpIntf(const std::string& ifName) {
    auto cmd = "ip link set dev {} up"_shellify(ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

 protected:
  bool
  CompareNextHops(std::vector<folly::IPAddress>& nexthops, const Route& route) {
    std::vector<folly::IPAddress> actual;
    for (const auto& nh : route.getNextHops()) {
      if (!nh.getGateway().has_value()) {
        return false;
      }
      actual.push_back(nh.getGateway().value());
    }
    sort(nexthops.begin(), nexthops.end());
    sort(actual.begin(), actual.end());
    return nexthops == actual;
  }

  void
  doUpdateRouteTest(bool isV4) {
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
    uint32_t ifIndex = netlinkSocket->getIfIndex(kVethNameY).get();
    LOG(INFO) << "ifindex " << ifIndex;

    // Add a route with single nextHop
    netlinkSocket
        ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix1))
        .get();

    auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix1));
    const Route& rt = routes.at(prefix1);
    EXPECT_EQ(1, rt.getNextHops().size());
    EXPECT_EQ(nexthops[0], rt.getNextHops().begin()->getGateway());

    // Check kernel
    auto kernelRoutes = netlinkSocket->getAllRoutes();
    int count{0};
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix1 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
          r.getNextHops().begin()->getGateway() == nexthops[0]) {
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
    netlinkSocket
        ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix1))
        .get();

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
    netlinkSocket
        ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix1))
        .get();

    // The route should now have both nh1 and nh2
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix1));
    const Route& rt2 = routes.at(prefix1);
    EXPECT_EQ(2, rt2.getNextHops().size());

    // Check kernel
    kernelRoutes = netlinkSocket->getAllRoutes();
    count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix1 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 2) {
        count++;
      }
    }
    EXPECT_EQ(1, count);

    // Delete the route via both nextHops
    netlinkSocket
        ->delRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix1))
        .get();
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
    netlinkSocket
        ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix2))
        .get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix2));
    const Route& rt3 = routes.at(prefix2);
    EXPECT_EQ(2, rt3.getNextHops().size());

    // Remove 1 of nextHops from the route
    nexthops.pop_back();
    netlinkSocket
        ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix2))
        .get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix2));
    const Route& rt4 = routes.at(prefix2);
    EXPECT_EQ(1, rt4.getNextHops().size());
    EXPECT_EQ(nexthops[0], rt4.getNextHops().begin()->getGateway());

    // Check kernel
    kernelRoutes = netlinkSocket->getAllRoutes();
    count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix2 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
          r.getNextHops().begin()->getGateway() == nexthops[0]) {
        count++;
      }
    }
    EXPECT_EQ(1, count);
    // Delete the same route
    netlinkSocket
        ->delRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix2))
        .get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(0, routes.size());

    kernelRoutes = netlinkSocket->getAllRoutes();
    count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix2 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
          r.getNextHops().begin()->getGateway() == nexthops[0]) {
        count++;
      }
    }
    EXPECT_EQ(0, count);
  }

  void
  doUpdateMultiRouteTest(bool isV4) {
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
    int ifIndex = netlinkSocket->getIfIndex(kVethNameY).get();

    // Add a route with 3 nextHops
    netlinkSocket
        ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix))
        .get();
    auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    EXPECT_EQ(1, routes.size());
    EXPECT_EQ(1, routes.count(prefix));
    const Route& rt = routes.at(prefix);
    EXPECT_EQ(3, rt.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops, rt));

    auto kernelRoutes = netlinkSocket->getAllRoutes();
    int count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 3) {
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
    netlinkSocket
        ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix))
        .get();

    // The route now has nextHop 1 and nextHop 2
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix));
    const Route& rt1 = routes.at(prefix);
    EXPECT_EQ(2, rt1.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops, rt1));

    kernelRoutes = netlinkSocket->getAllRoutes();
    count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 2) {
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
    netlinkSocket
        ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix))
        .get();

    // The route now has nextHop 1, 2, and 4
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(1, routes.size());
    ASSERT_EQ(1, routes.count(prefix));
    const Route& rt2 = routes.at(prefix);
    EXPECT_EQ(3, rt2.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops, rt2));

    kernelRoutes = netlinkSocket->getAllRoutes();
    count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 3) {
        count++;
      }
    }
    EXPECT_EQ(1, count);

    // Delete the route
    netlinkSocket
        ->delRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops, prefix))
        .get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(0, routes.size());

    kernelRoutes = netlinkSocket->getAllRoutes();
    count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 3) {
        count++;
      }
    }
    EXPECT_EQ(0, count);
  }

  void
  doSyncRouteTest(bool isV4) {
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
    int ifIndex = netlinkSocket->getIfIndex(kVethNameY).get();
    routeDb.emplace(
        prefix1, buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1));
    routeDb.emplace(
        prefix2, buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2));

    // Sync routeDb
    netlinkSocket->syncUnicastRoutes(kAqRouteProtoId, std::move(routeDb)).get();
    auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

    // Check in kernel
    auto kernelRoutes = netlinkSocket->getAllRoutes();
    int count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix1 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 2) {
        count++;
      }
      if (r.getDestination() == prefix2 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1) {
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

    kernelRoutes = netlinkSocket->getAllRoutes();
    count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix1 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1) {
        count++;
      }
      if (r.getDestination() == prefix2 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 2) {
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

    kernelRoutes = netlinkSocket->getAllRoutes();
    count = 0;
    for (const auto& r : kernelRoutes) {
      if (r.getDestination() == prefix1 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1) {
        count++;
      }
      if (r.getDestination() == prefix2 &&
          r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 2) {
        count++;
      }
    }
    EXPECT_EQ(1, count);

    EXPECT_EQ(1, routes.count(prefix2));
    const Route& rt5 = routes.at(prefix2);
    EXPECT_EQ(2, rt5.getNextHops().size());
    EXPECT_TRUE(CompareNextHops(nexthops2, rt5));

    // Delete the remaining route
    netlinkSocket
        ->delRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2))
        .get();
    routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
    EXPECT_EQ(0, routes.size());
  }
};

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
  int ifIndex = netlinkSocket->getIfIndex(kVethNameY).get();

  // Add a route1
  netlinkSocket
      ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1))
      .get();
  auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  const Route& rt = routes.at(prefix1);
  EXPECT_EQ(2, rt.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nexthops1, rt));

  // Add a route2
  netlinkSocket
      ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2))
      .get();
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
  auto kernelRoutes = netlinkSocket->getAllRoutes();
  int count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId &&
        r.getNextHops().size() == 2) {
      count++;
    }
    if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId &&
        r.getNextHops().size() == 2) {
      count++;
    }
  }
  EXPECT_EQ(2, count);

  // Delete the routes
  netlinkSocket
      ->delRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.count(prefix2));
  const Route& rt4 = routes.at(prefix2);
  EXPECT_EQ(2, rt4.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nexthops2, rt4));

  netlinkSocket
      ->delRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops2, prefix2))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId &&
        r.getNextHops().size() == 2) {
      count++;
    }
    if (r.getDestination() == prefix2 && r.getProtocolId() == kAqRouteProtoId &&
        r.getNextHops().size() == 2) {
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
  int ifIndex = netlinkSocket->getIfIndex(kVethNameY).get();

  // Add a route via a single nextHop nh1
  netlinkSocket
      ->addRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1))
      .get();
  auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();

  SCOPE_EXIT {
    EXPECT_EQ(0, routes.size());
  };

  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1));
  const Route& rt = routes.at(prefix1);
  EXPECT_EQ(1, rt.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nexthops1, rt));

  // Try deleting the route with a non existing prefix2
  netlinkSocket
      ->delRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix2))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());

  // Check kernel
  auto kernelRoutes = netlinkSocket->getAllRoutes();
  int count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId &&
        r.getNextHops().size() == 1) {
      count++;
    }
  }
  EXPECT_EQ(1, count);

  // Delete the route
  netlinkSocket
      ->delRoute(buildRoute(ifIndex, kAqRouteProtoId, nexthops1, prefix1))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1 && r.getProtocolId() == kAqRouteProtoId &&
        r.getNextHops().size() == 1) {
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
  int ifIndexX = netlinkSocket->getIfIndex(kVethNameX).get();
  int ifIndexY = netlinkSocket->getIfIndex(kVethNameY).get();
  // V4 routes for protocol 159
  folly::CIDRNetwork prefix1V4{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2V4{folly::IPAddress("192.168.0.12"), 32};
  auto nh1V4 = folly::IPAddress("169.254.0.1");
  auto nh2V4 = folly::IPAddress("169.254.0.2");

  std::vector<folly::IPAddress> nextHopsV6{nh1V6};
  // Add routes with single nextHop for protocol 99
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6))
      .get();
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6))
      .get();
  auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
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
  netlinkSocket
      ->addRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4))
      .get();
  netlinkSocket
      ->addRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix2V4))
      .get();
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
  auto kernelRoutes = netlinkSocket->getAllRoutes();
  int count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix1V4 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix2V4 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1) {
      count++;
    }
  }
  EXPECT_EQ(4, count);

  // Change nexthop to nh2
  nextHopsV6.clear();
  nextHopsV6.push_back(nh2V6);

  // Update the same route with new nextHop nh2 for protocol 99
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6))
      .get();

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
  netlinkSocket
      ->addRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix2V4))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix2V4));
  const Route& rt6 = routes.at(prefix2V4);
  EXPECT_EQ(1, rt6.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V4, rt6));

  // Add back nexthop nh1
  nextHopsV6.push_back(nh1V6);
  // Update the same route with new nextHop nh1
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6))
      .get();

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
  netlinkSocket
      ->addRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4))
      .get();

  // The route should now have both nh3 and nh4
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));
  const Route& rt8 = routes.at(prefix1V4);
  EXPECT_EQ(2, rt8.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V4, rt8));

  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 2) {
      count++;
    }
    if (r.getDestination() == prefix1V4 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 2) {
      count++;
    }
    if (r.getDestination() == prefix2V4 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1) {
      count++;
    }
  }
  EXPECT_EQ(4, count);

  // Delete the route
  netlinkSocket
      ->delRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  netlinkSocket
      ->delRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  nextHops1V4.clear();
  nextHops1V4.push_back(nh2V4);
  nextHops1V4.push_back(nh1V4);
  netlinkSocket
      ->delRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  nextHops1V4.clear();
  nextHops1V4.push_back(nh2V4);
  netlinkSocket
      ->delRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix2V4))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());

  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 || r.getDestination() == prefix2V6 ||
        r.getDestination() == prefix1V4 || r.getDestination() == prefix2V4) {
      count++;
    }
  }

  EXPECT_EQ(0, count);
}

TEST_F(NetlinkSocketFixture, MultiProtocolUnicastTestDecisionTest) {
  // V6 routes for protocol 99
  folly::CIDRNetwork prefix1V6{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::CIDRNetwork prefix2V6{folly::IPAddress("fc00:cafe:3::4"), 128};
  auto nh1V6 = folly::IPAddress("fe80::1");
  auto nh2V6 = folly::IPAddress("fe80::2");
  int ifIndexX = netlinkSocket->getIfIndex(kVethNameX).get();
  int ifIndexY = netlinkSocket->getIfIndex(kVethNameY).get();
  // V4 routes for protocol 159
  folly::CIDRNetwork prefix1V4{folly::IPAddress("192.168.0.11"), 32};
  folly::CIDRNetwork prefix2V4{folly::IPAddress("192.168.0.12"), 32};
  auto nh1V4 = folly::IPAddress("169.254.0.1");

  std::vector<folly::IPAddress> nextHopsV6{nh1V6};
  // Add routes with single nextHop for protocol 99
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6))
      .get();
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6))
      .get();
  auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
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
  std::vector<folly::IPAddress> nextHops1V6{nh2V6};
  netlinkSocket
      ->addRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V6, prefix1V6))
      .get();
  netlinkSocket
      ->addRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V6, prefix2V6))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(2, routes.size());
  ASSERT_EQ(1, routes.count(prefix1V6));
  ASSERT_EQ(1, routes.count(prefix2V6));
  const Route& rt3 = routes.at(prefix1V6);
  const Route& rt4 = routes.at(prefix2V6);
  EXPECT_EQ(1, rt3.getNextHops().size());
  EXPECT_EQ(1, rt4.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V6, rt3));
  EXPECT_TRUE(CompareNextHops(nextHops1V6, rt4));

  // Check kernel kAqRouteProtoId should be selected
  auto kernelRoutes = netlinkSocket->getAllRoutes();
  int count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoIdPriority) {
      count++;
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoIdPriority) {
      count++;
    }
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoId1Priority) {
      count++;
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoId1Priority) {
      count++;
    }
  }
  EXPECT_EQ(4, count);

  // Add same route again, should not affect result
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6))
      .get();

  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6))
      .get();

  // Check kernel kAqRouteProtoId should be selected
  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1) {
      count++;
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1) {
      count++;
    }
  }
  EXPECT_EQ(2, count);

  // delete the route in routing table, system should choose the backup route
  netlinkSocket
      ->delRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6))
      .get();

  netlinkSocket
      ->delRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6))
      .get();

  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoId1Priority) {
      count++;
      LOG(INFO) << static_cast<int>(r.getProtocolId());
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoId1Priority) {
      count++;
    }
  }
  EXPECT_EQ(2, count);

  // add route back to see if it replace
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6))
      .get();
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6))
      .get();

  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoIdPriority) {
      count++;
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoIdPriority) {
      count++;
    }
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoId1Priority) {
      count++;
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId1 && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoId1Priority) {
      count++;
    }
  }
  EXPECT_EQ(4, count);

  // delete protocol1 route
  netlinkSocket
      ->delRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V6, prefix1V6))
      .get();

  netlinkSocket
      ->delRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V6, prefix2V6))
      .get();

  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoIdPriority) {
      count++;
    }
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId && r.getNextHops().size() == 1 &&
        r.getPriority().value() == kAqRouteProtoIdPriority) {
      count++;
    }
  }
  EXPECT_EQ(2, count);

  // delete protocol route
  netlinkSocket
      ->delRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix1V6))
      .get();

  netlinkSocket
      ->delRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHopsV6, prefix2V6))
      .get();

  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;

  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6) {
      count++;
    }
    if (r.getDestination() == prefix2V6) {
      count++;
    }
  }
  EXPECT_EQ(0, count);
}

TEST_F(NetlinkSocketFixture, MultiProtocolSyncUnicastRouteTest) {
  int ifIndexX = netlinkSocket->getIfIndex(kVethNameX).get();
  int ifIndexY = netlinkSocket->getIfIndex(kVethNameY).get();

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
  netlinkSocket
      ->addRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHops2V6, prefix1V6))
      .get();
  auto routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
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
      prefix1V4,
      buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4));
  routeDbV4.emplace(
      prefix2V4,
      buildRoute(ifIndexY, kAqRouteProtoId1, nextHops2V4, prefix2V4));

  // Pre-add unicast routes
  netlinkSocket
      ->addRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops2V4, prefix1V4))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V4));

  // Check kernel
  auto kernelRoutes = netlinkSocket->getAllRoutes();
  int count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix1V6 &&
        r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if (r.getDestination() == prefix1V4 &&
        r.getProtocolId() == kAqRouteProtoId1) {
      count++;
    }
  }
  EXPECT_EQ(2, count);

  // Sync routeDb
  netlinkSocket->syncUnicastRoutes(kAqRouteProtoId, std::move(routeDbV6)).get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(2, routes.size());
  EXPECT_EQ(1, routes.count(prefix1V6));
  EXPECT_EQ(1, routes.count(prefix2V6));
  const Route& rt1 = routes.at(prefix1V6);
  const Route& rt2 = routes.at(prefix2V6);
  EXPECT_EQ(2, rt1.getNextHops().size());
  EXPECT_EQ(1, rt2.getNextHops().size());
  EXPECT_TRUE(CompareNextHops(nextHops1V6, rt1));
  EXPECT_TRUE(CompareNextHops(nextHops2V6, rt2));

  netlinkSocket->syncUnicastRoutes(kAqRouteProtoId1, std::move(routeDbV4))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
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
  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if ((r.getDestination() == prefix1V6 || r.getDestination() == prefix2V6) &&
        r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if ((r.getDestination() == prefix1V4 || r.getDestination() == prefix2V4) &&
        r.getProtocolId() == kAqRouteProtoId1) {
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
  netlinkSocket->syncUnicastRoutes(kAqRouteProtoId, std::move(routeDbV6)).get();
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
      prefix1V4,
      buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4));
  routeDbV4.emplace(
      prefix2V4,
      buildRoute(ifIndexY, kAqRouteProtoId1, nextHops2V4, prefix2V4));

  // Sync routeDb V4
  netlinkSocket->syncUnicastRoutes(kAqRouteProtoId1, std::move(routeDbV4))
      .get();
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
  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if (r.getDestination() == prefix2V6 &&
        r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
  }
  EXPECT_EQ(1, count);

  // Delete V4 route
  netlinkSocket
      ->delRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops1V4, prefix1V4))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(1, routes.size());
  nextHops2V4.clear();
  netlinkSocket
      ->delRoute(buildRoute(ifIndexY, kAqRouteProtoId1, nextHops2V4, prefix2V4))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId1).get();
  EXPECT_EQ(0, routes.size());

  // Delete V6 route
  netlinkSocket
      ->delRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHops1V6, prefix1V6))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(1, routes.size());
  nextHops2V6.clear();
  netlinkSocket
      ->delRoute(buildRoute(ifIndexX, kAqRouteProtoId, nextHops2V6, prefix2V6))
      .get();
  routes = netlinkSocket->getCachedUnicastRoutes(kAqRouteProtoId).get();
  EXPECT_EQ(0, routes.size());

  // Check kernel
  kernelRoutes = netlinkSocket->getAllRoutes();
  count = 0;
  for (const auto& r : kernelRoutes) {
    if ((r.getDestination() == prefix1V6 || r.getDestination() == prefix2V6) &&
        r.getProtocolId() == kAqRouteProtoId) {
      count++;
    }
    if ((r.getDestination() == prefix1V4 || r.getDestination() == prefix2V4) &&
        r.getProtocolId() == kAqRouteProtoId1) {
      count++;
    }
  }
  EXPECT_EQ(0, count);
}

TEST_F(NetlinkSocketFixture, IfNametoIfIndexTest) {
  int ifIndex = netlinkSocket->getIfIndex(kVethNameX).get();
  EXPECT_EQ(netlinkSocket->getIfIndex(kVethNameX).get(), ifIndex);
  EXPECT_EQ(kVethNameX, netlinkSocket->getIfName(ifIndex).get());

  ifIndex = netlinkSocket->getIfIndex(kVethNameY).get();
  EXPECT_EQ(netlinkSocket->getIfIndex(kVethNameY).get(), ifIndex);
  EXPECT_EQ(kVethNameY, netlinkSocket->getIfName(ifIndex).get());
}

TEST_F(NetlinkSocketFixture, AddDelIfAddressBaseTest) {
  folly::CIDRNetwork prefixV6{folly::IPAddress("fc00:cafe:3::3"), 128};
  IfAddressBuilder builder;

  int ifIndex = netlinkSocket->getIfIndex(kVethNameX).get();
  ASSERT_NE(0, ifIndex);
  auto ifAddr = builder.setPrefix(prefixV6).setIfIndex(ifIndex).build();

  builder.reset();
  const folly::CIDRNetwork prefixV4{folly::IPAddress("192.168.0.11"), 32};
  auto ifAddr1 = builder.setPrefix(prefixV4).setIfIndex(ifIndex).build();

  auto kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();
  int before = kernelAddresses.size();

  // Add address
  netlinkSocket->addIfAddress(std::move(ifAddr)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();

  kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();
  int cnt = 0;
  for (const auto& addr : kernelAddresses) {
    if (addr.getPrefix() == prefixV6 && addr.getIfIndex() == ifIndex) {
      cnt++;
    } else if (addr.getPrefix() == prefixV4 && addr.getIfIndex() == ifIndex) {
      cnt++;
    }
  }
  EXPECT_EQ(2, cnt);

  // delete addresses
  ifAddr = builder.setPrefix(prefixV6).setIfIndex(ifIndex).build();
  builder.reset();
  ifAddr1 = builder.setPrefix(prefixV4).setIfIndex(ifIndex).build();
  netlinkSocket->delIfAddress(std::move(ifAddr)).get();
  netlinkSocket->delIfAddress(std::move(ifAddr1)).get();

  kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();

  EXPECT_EQ(before, kernelAddresses.size());

  bool found = false;
  for (const auto& addr : kernelAddresses) {
    // No more added address
    if ((addr.getPrefix() == prefixV6 && addr.getIfIndex() == ifIndex) ||
        (addr.getPrefix() == prefixV4 && addr.getIfIndex() == ifIndex)) {
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
  auto ifAddr = builder.setPrefix(prefix).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr1 = builder.setPrefix(prefix).setIfIndex(ifIndex).build();

  auto kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();
  size_t before = kernelAddresses.size();

  // Add new address
  netlinkSocket->addIfAddress(std::move(ifAddr)).get();
  // Add duplicated address. EXISTS error is suppressed.
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();

  kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();
  // one more addresse
  EXPECT_EQ(before + 1, kernelAddresses.size());

  int cnt = 0;
  for (const auto& addr : kernelAddresses) {
    if (addr.getPrefix() == prefix && addr.getIfIndex() == ifIndex) {
      cnt++;
    }
  }
  EXPECT_EQ(1, cnt);

  // delete addresses
  ifAddr = builder.setPrefix(prefix).setIfIndex(ifIndex).build();
  builder.reset();
  ifAddr1 = builder.setPrefix(prefix).setIfIndex(ifIndex).build();
  netlinkSocket->delIfAddress(std::move(ifAddr)).get();
  // Double delete. EADDRNOTAVAIL error is suppressed.
  netlinkSocket->delIfAddress(std::move(ifAddr1)).get();

  kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();

  EXPECT_EQ(before, kernelAddresses.size());

  bool found = false;
  for (const auto& addr : kernelAddresses) {
    // No more added address
    if ((addr.getPrefix() == prefix && addr.getIfIndex() == ifIndex)) {
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
  auto ifAddr1 = builder.setPrefix(prefix1).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr2 = builder.setPrefix(prefix2).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr3 = builder.setPrefix(prefix3).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr4 = builder.setPrefix(prefix4).setIfIndex(ifIndex).build();

  // Add new address
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr4)).get();

  auto kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();

  bool p1 = false, p2 = false, p3 = false, p4 = false, p5 = false;
  // Check contains all prefix
  for (const auto& addr : kernelAddresses) {
    if (addr.getPrefix() == prefix1) {
      p1 = true;
    } else if (addr.getPrefix() == prefix2) {
      p2 = true;
    } else if (addr.getPrefix() == prefix3) {
      p3 = true;
    } else if (addr.getPrefix() == prefix4) {
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
  addrs.emplace_back(builder.setIfIndex(ifIndex).setPrefix(prefix2).build());
  EXPECT_THROW(
      netlinkSocket
          ->syncIfAddress(
              ifIndex, std::move(addrs), AF_UNSPEC, RT_SCOPE_NOWHERE)
          .get(),
      std::exception);

  // Prefix not set will throw exception
  builder.reset();
  addrs.clear();
  addrs.emplace_back(builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  builder.reset();
  addrs.emplace_back(builder.setIfIndex(ifIndex).build());
  EXPECT_THROW(
      netlinkSocket
          ->syncIfAddress(
              ifIndex, std::move(addrs), AF_UNSPEC, RT_SCOPE_NOWHERE)
          .get(),
      std::exception);

  // Sync v6 address
  builder.reset();
  addrs.clear();
  addrs.emplace_back(builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  netlinkSocket
      ->syncIfAddress(ifIndex, std::move(addrs), AF_INET6, RT_SCOPE_NOWHERE)
      .get();

  // Sync v4 address
  builder.reset();
  addrs.clear();
  addrs.emplace_back(builder.setIfIndex(ifIndex).setPrefix(prefix4).build());
  netlinkSocket
      ->syncIfAddress(ifIndex, std::move(addrs), AF_INET, RT_SCOPE_NOWHERE)
      .get();

  kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();

  // Check no prefix2, prefix3
  p1 = false, p2 = false, p3 = false, p4 = false;
  for (const auto& addr : kernelAddresses) {
    if (addr.getPrefix() == prefix1) {
      p1 = true;
    } else if (addr.getPrefix() == prefix2) {
      p2 = true;
    } else if (addr.getPrefix() == prefix3) {
      p3 = true;
    } else if (addr.getPrefix() == prefix4) {
      p4 = true;
    }
  }
  EXPECT_TRUE(p1);
  EXPECT_FALSE(p2);
  EXPECT_FALSE(p3);
  EXPECT_TRUE(p4);

  builder.reset();
  ifAddr2 = builder.setPrefix(prefix2).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr5 = builder.setPrefix(prefix5).setIfIndex(ifIndex).build();
  netlinkSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr5)).get();

  builder.reset();
  addrs.clear();
  addrs.emplace_back(builder.setIfIndex(ifIndex).setPrefix(prefix5).build());
  addrs.emplace_back(builder.setIfIndex(ifIndex).setPrefix(prefix1).build());
  netlinkSocket
      ->syncIfAddress(ifIndex, std::move(addrs), AF_INET6, RT_SCOPE_NOWHERE)
      .get();

  kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();

  // Check no prefix2, prefix3
  p1 = false, p2 = false, p3 = false, p4 = false;
  for (const auto& addr : kernelAddresses) {
    if (addr.getPrefix() == prefix1) {
      p1 = true;
    } else if (addr.getPrefix() == prefix2) {
      p2 = true;
    } else if (addr.getPrefix() == prefix3) {
      p3 = true;
    } else if (addr.getPrefix() == prefix4) {
      p4 = true;
    } else if (addr.getPrefix() == prefix5) {
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
  auto ifAddr1 = builder.setPrefix(prefix1).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr2 = builder.setPrefix(prefix2).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr3 = builder.setPrefix(prefix3).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr4 = builder.setPrefix(prefix4).setIfIndex(ifIndex).build();

  // Add new address
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr4)).get();

  auto kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();

  int ipV4cnt = 0, ipV6cnt = 0;
  for (const auto& addr : kernelAddresses) {
    if (addr.getFamily() == AF_INET && addr.getIfIndex() == ifIndex) {
      ipV4cnt++;
    }
    if (addr.getFamily() == AF_INET6 && addr.getIfIndex() == ifIndex) {
      ipV6cnt++;
    }
  }

  // delete IPV6 address family
  std::vector<IfAddress> addrs;
  netlinkSocket
      ->syncIfAddress(ifIndex, std::move(addrs), AF_INET6, RT_SCOPE_NOWHERE)
      .get();
  kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();

  // No V6 address found
  int actualV4cnt = 0, actualV6cnt = 0;
  for (const auto& addr : kernelAddresses) {
    // No more added address
    if (addr.getFamily() == AF_INET && addr.getIfIndex() == ifIndex) {
      actualV4cnt++;
    }
    if (addr.getFamily() == AF_INET6 && addr.getIfIndex() == ifIndex &&
        addr.getScope().value() == RT_SCOPE_UNIVERSE) {
      actualV6cnt++;
    }
  }
  EXPECT_EQ(ipV4cnt, actualV4cnt);
  EXPECT_EQ(0, actualV6cnt);

  addrs.clear();
  netlinkSocket
      ->syncIfAddress(ifIndex, std::move(addrs), AF_INET, RT_SCOPE_NOWHERE)
      .get();

  // No v4 address
  kernelAddresses =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();
  actualV4cnt = 0, actualV6cnt = 0;
  for (const auto& addr : kernelAddresses) {
    // No more added address
    if (addr.getFamily() == AF_INET && addr.getIfIndex() == ifIndex) {
      actualV4cnt++;
    }
    if (addr.getFamily() == AF_INET6 && addr.getIfIndex() == ifIndex &&
        addr.getScope().value() == RT_SCOPE_UNIVERSE) {
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
  auto ifAddr1 = builder.setPrefix(prefix1).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr2 = builder.setPrefix(prefix2).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr3 = builder.setPrefix(prefix3).setIfIndex(ifIndex).build();
  builder.reset();
  auto ifAddr4 = builder.setPrefix(prefix4).setIfIndex(ifIndex).build();

  // Cleaup all address
  std::vector<IfAddress> addrs;
  netlinkSocket
      ->syncIfAddress(ifIndex, std::move(addrs), AF_UNSPEC, RT_SCOPE_NOWHERE)
      .get();

  // Add new address
  netlinkSocket->addIfAddress(std::move(ifAddr1)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr2)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr3)).get();
  netlinkSocket->addIfAddress(std::move(ifAddr4)).get();

  // Check get V6 address
  auto v6Addrs =
      netlinkSocket->getIfAddrs(ifIndex, AF_INET6, RT_SCOPE_NOWHERE).get();
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
  auto v4Addrs =
      netlinkSocket->getIfAddrs(ifIndex, AF_INET, RT_SCOPE_NOWHERE).get();
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
  auto allAddrs =
      netlinkSocket->getIfAddrs(ifIndex, AF_UNSPEC, RT_SCOPE_NOWHERE).get();
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

// Check if NetlinkSocket returns the index of the loopback interface
// which is used for MPLS route programming
TEST_F(NetlinkSocketFixture, LoopbackTest) {
  LOG(INFO) << "Get all links and check if loopback index is set";
  netlinkSocket->getAllLinks();
  EXPECT_TRUE(netlinkSocket->getLoopbackIfIndex().get().has_value());
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
