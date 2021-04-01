/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/async/ZmqEventLoop.h>
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
};

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
