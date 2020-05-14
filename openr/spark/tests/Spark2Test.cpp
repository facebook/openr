/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <forward_list>
#include <mutex>
#include <thread>

#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/ThreadLocal.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sodium.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/spark/IoProvider.h>
#include <openr/spark/SparkWrapper.h>
#include <openr/spark/tests/MockIoProvider.h>

using namespace openr;

using apache::thrift::CompactSerializer;

namespace {
const std::string iface1{"iface1"};
const std::string iface2{"iface2"};
const std::string iface3{"iface3"};

const int ifIndex1{1};
const int ifIndex2{2};
const int ifIndex3{3};

const std::string area1{"area1"};
const std::string area2{"area2"};
const std::string defaultArea{thrift::KvStore_constants::kDefaultArea()};

const folly::CIDRNetwork ip1V4 =
    folly::IPAddress::createNetwork("192.168.0.1", 24, false /* apply mask */);
const folly::CIDRNetwork ip2V4 =
    folly::IPAddress::createNetwork("192.168.0.2", 24, false /* apply mask */);
const folly::CIDRNetwork ip3V4 =
    folly::IPAddress::createNetwork("192.168.0.3", 24, false /* apply mask */);

const folly::CIDRNetwork ip1V6 = folly::IPAddress::createNetwork("fe80::1/128");
const folly::CIDRNetwork ip2V6 = folly::IPAddress::createNetwork("fe80::2/128");
const folly::CIDRNetwork ip3V6 = folly::IPAddress::createNetwork("fe80::3/128");

// alias for neighbor event
const auto NB_UP = thrift::SparkNeighborEventType::NEIGHBOR_UP;
const auto NB_DOWN = thrift::SparkNeighborEventType::NEIGHBOR_DOWN;
const auto NB_RESTARTING = thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING;
const auto NB_RESTARTED = thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED;
const auto NB_RTT_CHANGE = thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE;

// alias for neighbor state
const auto WARM = SparkNeighState::WARM;
const auto NEGOTIATE = SparkNeighState::NEGOTIATE;
const auto ESTABLISHED = SparkNeighState::ESTABLISHED;

// Domain name (same for all Tests except in DomainTest)
const std::string kDomainName("Fire_and_Blood");

// the hold time we use during the tests
const std::chrono::milliseconds kGRHoldTime(500);

// the time interval for spark2 hello msg under fast init mode
const std::chrono::milliseconds kFastInitHelloTime(50);

// the time interval for spark2 hello msg
const std::chrono::milliseconds kHelloTime(200);

// the time interval for spark2 handhshake msg
const std::chrono::milliseconds kHandshakeTime(50);

// the time interval for spark2 heartbeat msg
const std::chrono::milliseconds kHeartbeatTime(50);

// the hold time for spark2 negotiate stage
const std::chrono::milliseconds kNegotiateHoldTime(500);

// the hold time for spark2 heartbeat msg
const std::chrono::milliseconds kHeartbeatHoldTime(200);
}; // namespace

class Spark2Fixture : public testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider = std::make_shared<MockIoProvider>();

    // Start mock IoProvider thread
    mockIoProviderThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting mockIoProvider thread.";
      mockIoProvider->start();
      LOG(INFO) << "mockIoProvider thread got stopped.";
    });
    mockIoProvider->waitUntilRunning();
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping mockIoProvider thread.";
    mockIoProvider->stop();
    mockIoProviderThread->join();
  }

  std::shared_ptr<SparkWrapper>
  createSpark(
      std::string const& domainName,
      std::string const& myNodeName,
      uint32_t spark2Id,
      bool enableSpark2 = true,
      bool increaseHelloInterval = true,
      std::shared_ptr<thrift::OpenrConfig> config = nullptr,
      std::pair<uint32_t, uint32_t> version = std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
      std::chrono::milliseconds grHoldTime = kGRHoldTime,
      // TODO: remove unnecessary argument list when old spark is deprecated
      std::chrono::milliseconds keepAliveTime = kFastInitHelloTime,
      std::chrono::milliseconds fastInitKeepAliveTime = kFastInitHelloTime,
      SparkTimeConfig timeConfig = SparkTimeConfig(
          kHelloTime,
          kFastInitHelloTime,
          kHandshakeTime,
          kHeartbeatTime,
          kNegotiateHoldTime,
          kHeartbeatHoldTime)) {
    return std::make_unique<SparkWrapper>(
        domainName,
        myNodeName,
        grHoldTime,
        keepAliveTime,
        fastInitKeepAliveTime,
        true, /* enableV4 */
        version,
        mockIoProvider,
        config,
        enableSpark2,
        increaseHelloInterval,
        timeConfig);
  }

  std::shared_ptr<MockIoProvider> mockIoProvider{nullptr};
  std::unique_ptr<std::thread> mockIoProviderThread{nullptr};
};

class SimpleSpark2Fixture : public Spark2Fixture {
 protected:
  void
  createAndConnectSpark2Nodes() {
    // Define interface names for the test
    mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

    // connect interfaces directly
    ConnectedIfPairs connectedPairs = {
        {iface1, {{iface2, 10}}},
        {iface2, {{iface1, 10}}},
    };
    mockIoProvider->setConnectedPairs(connectedPairs);

    // start one spark2 instance
    node1 = createSpark(kDomainName, "node-1", 1);

    // start another spark2 instance
    node2 = createSpark(kDomainName, "node-2", 2);

    // start tracking iface1
    EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));

    // start tracking iface2
    EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

    LOG(INFO) << "Start to receive messages from Spark2";

    // Now wait for sparks to detect each other
    {
      auto event = node1->waitForEvent(NB_UP);
      ASSERT_TRUE(event.has_value());
      EXPECT_EQ(iface1, event->ifName);
      EXPECT_EQ("node-2", event->neighbor.nodeName);
      EXPECT_EQ(
          std::make_pair(ip2V4.first, ip2V6.first),
          SparkWrapper::getTransportAddrs(*event));
      LOG(INFO) << "node-1 reported adjacency to node-2";
    }

    {
      auto event = node2->waitForEvent(NB_UP);
      ASSERT_TRUE(event.has_value());
      EXPECT_EQ(iface2, event->ifName);
      EXPECT_EQ("node-1", event->neighbor.nodeName);
      EXPECT_EQ(
          std::make_pair(ip1V4.first, ip1V6.first),
          SparkWrapper::getTransportAddrs(*event));
      LOG(INFO) << "node-2 reported adjacency to node-1";
    }
  }

  std::shared_ptr<SparkWrapper> node1;
  std::shared_ptr<SparkWrapper> node2;
};

//
// Start 2 Spark instances and wait them forming adj. Then
// increase/decrease RTT, expect NEIGHBOR_RTT_CHANGE event
//
TEST_F(SimpleSpark2Fixture, RttTest) {
  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  LOG(INFO) << "Change rtt between nodes to 40ms (asymmetric)";

  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 15}}},
      {iface2, {{iface1, 25}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // wait for spark nodes to detecct Rtt change
  {
    auto event = node1->waitForEvent(NB_RTT_CHANGE);
    ASSERT_TRUE(event.has_value());
    // 25% tolerance
    EXPECT_GE(event->rttUs, (40 - 10) * 1000);
    EXPECT_LE(event->rttUs, (40 + 10) * 1000);
    LOG(INFO) << "node-1 reported new RTT to node-2 to be "
              << event->rttUs / 1000.0 << "ms";
  }

  {
    auto event = node2->waitForEvent(NB_RTT_CHANGE);
    ASSERT_TRUE(event.has_value());
    // 25% tolerance
    EXPECT_GE(event->rttUs, (40 - 10) * 1000);
    EXPECT_LE(event->rttUs, (40 + 10) * 1000);
    LOG(INFO) << "node-2 reported new RTT to node-1 to be "
              << event->rttUs / 1000.0 << "ms";
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// make it uni-directional, expect both side to lose adj
// due to missing node info in `ReflectedNeighborInfo`
//
TEST_F(SimpleSpark2Fixture, UnidirectionTest) {
  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  LOG(INFO) << "Stopping communications from iface2 to iface1";

  // stop packet flowing iface2 -> iface1. Expect both ends drops
  //  1. node1 drops due to: heartbeat hold timer expired
  //  2. node2 drops due to: helloMsg doesn't contains neighborInfo
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // wait for sparks to lose each other
  {
    EXPECT_TRUE(node1->waitForEvent(NB_DOWN).has_value());
    LOG(INFO) << "node-1 reported down adjacency to node-2";
  }

  {
    EXPECT_TRUE(node2->waitForEvent(NB_DOWN).has_value());
    LOG(INFO) << "node-2 reported down adjacency to node-1";
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// restart one of them within GR window, make sure we get neighbor
// "RESTARTED" event due to graceful restart window.
//
TEST_F(SimpleSpark2Fixture, GRTest) {
  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  // Kill node2
  LOG(INFO) << "Kill and restart node-2";

  node2.reset();

  // node-1 should report node-2 as 'RESTARTING'
  {
    EXPECT_TRUE(node1->waitForEvent(NB_RESTARTING).has_value());
    LOG(INFO) << "node-1 reported node-2 as RESTARTING";
  }

  node2 = createSpark(kDomainName, "node-2", 3 /* spark2Id change */);

  LOG(INFO) << "Adding iface2 to node-2 to let it start helloMsg adverstising";

  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  // node-1 should report node-2 as 'RESTARTED' when receiving helloMsg
  // with wrapped seqNum
  {
    EXPECT_TRUE(node1->waitForEvent(NB_RESTARTED).has_value());
    LOG(INFO) << "node-1 reported node-2 as 'RESTARTED'";
  }

  // node-2 should ultimately report node-1 as 'UP'
  {
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  // should NOT receive any event( e.g.NEIGHBOR_DOWN)
  {
    EXPECT_FALSE(
        node1->waitForEvent(NB_DOWN, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(
        node2->waitForEvent(NB_DOWN, kGRHoldTime, kGRHoldTime * 2).has_value());
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// gracefully shut down one of them but NOT bring it back,
// make sure we get neighbor "DOWN" event due to GR timer expiring.
//
TEST_F(SimpleSpark2Fixture, GRTimerExpireTest) {
  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  // Kill node2
  LOG(INFO) << "Kill and restart node-2";

  auto startTime = std::chrono::steady_clock::now();
  node2.reset();

  // Since node2 doesn't come back, will lose adj and declare DOWN
  {
    EXPECT_TRUE(node1->waitForEvent(NB_DOWN).has_value());
    LOG(INFO) << "node-1 reporte down adjacency to node-2";

    // Make sure 'down' event is triggered by GRTimer expire
    // and NOT related with heartbeat holdTimer( no hearbeatTimer started )
    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime >= kGRHoldTime);
    ASSERT_TRUE(endTime - startTime <= kGRHoldTime + kHeartbeatHoldTime);
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// stop the bi-direction communication from each other.
// Observe neighbor going DOWN due to hold timer expiration.
//
TEST_F(SimpleSpark2Fixture, HeartbeatTimerExpireTest) {
  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  // record time for future comparison
  auto startTime = std::chrono::steady_clock::now();

  // remove underneath connections between to nodes
  ConnectedIfPairs connectedPairs = {};
  mockIoProvider->setConnectedPairs(connectedPairs);

  // wait for sparks to lose each other
  {
    LOG(INFO) << "Waiting for both nodes to time out with each other";

    EXPECT_TRUE(node1->waitForEvent(NB_DOWN).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_DOWN).has_value());

    // record time for expiration time test
    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime >= kHeartbeatHoldTime);
    ASSERT_TRUE(endTime - startTime <= kGRHoldTime);
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// remove/add interface from one instance's perspective
//
TEST_F(SimpleSpark2Fixture, InterfaceRemovalTest) {
  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  auto startTime = std::chrono::steady_clock::now();

  // tell node1 to remove interface to mimick request from linkMonitor
  EXPECT_TRUE(node1->updateInterfaceDb({}));

  LOG(INFO) << "Waiting for node-1 to report loss of adj to node-2";

  // since the removal of intf happens instantly. down event should
  // be reported ASAP.
  {
    EXPECT_TRUE(node1->waitForEvent(NB_DOWN).has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        endTime - startTime <= std::min(kGRHoldTime, kHeartbeatHoldTime));
    LOG(INFO)
        << "node-1 reported down adjacency to node-2 due to interface removal";
  }

  {
    EXPECT_TRUE(node2->waitForEvent(NB_DOWN).has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime <= kGRHoldTime);
    LOG(INFO)
        << "node-2 reported down adjacency to node-2 due to heartbeat expired";
  }

  {
    // should NOT receive any event after down adj
    EXPECT_TRUE(node1->recvNeighborEvent(kGRHoldTime).hasError());
    EXPECT_TRUE(node2->recvNeighborEvent(kGRHoldTime).hasError());
  }

  // Resume interface connection
  LOG(INFO) << "Bringing iface-1 back online";

  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  startTime = std::chrono::steady_clock::now();

  {
    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime <= kNegotiateHoldTime + kHeartbeatHoldTime);
    LOG(INFO) << "node-1 reported up adjacency to node-2";
  }

  {
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime <= kNegotiateHoldTime + kHeartbeatHoldTime);
    LOG(INFO) << "node-2 reported up adjacency to node-1";
  }
}

//
// Start 2 Spark instances for different versions but within supported
// range. Make sure they will form adjacency. Then add node3 with out-of-range
// version. Confirm node3 can't form adjacency with neither of node1/node2
// bi-directionally.
//
TEST_F(Spark2Fixture, VersionTest) {
  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex(
      {{iface1, ifIndex1}, {iface2, ifIndex2}, {iface3, ifIndex3}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}, {iface3, 10}}},
      {iface2, {{iface1, 10}, {iface3, 10}}},
      {iface3, {{iface1, 10}, {iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start node1, node2 with different but within supported range
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";
  const std::string nodeName3 = "node-3";
  auto node1 = createSpark(
      kDomainName,
      nodeName1,
      1,
      true,
      true,
      nullptr,
      std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion));
  auto node2 = createSpark(
      kDomainName,
      nodeName2,
      2,
      true,
      true,
      nullptr,
      std::make_pair(
          Constants::kOpenrSupportedVersion,
          Constants::kOpenrSupportedVersion));

  // start tracking interfaces
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());
  }

  LOG(INFO) << "Starting: " << nodeName3;

  auto node3 = createSpark(
      kDomainName,
      nodeName3,
      3,
      true,
      true,
      nullptr,
      std::make_pair(
          Constants::kOpenrSupportedVersion - 1,
          Constants::kOpenrSupportedVersion - 1));

  // start tracking interfaces
  EXPECT_TRUE(node3->updateInterfaceDb({{iface3, ifIndex3, ip3V4, ip3V6}}));

  // node3 can't form adj with neither node1 nor node2
  {
    EXPECT_FALSE(
        node1->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(
        node2->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(
        node3->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
  }
}

//
// Start 2 Spark instances within different domains. Then
// make sure they can't form adj as helloMsg being ignored.
//
TEST_F(Spark2Fixture, DomainTest) {
  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start 2 spark instances within different domain
  std::string domainLannister = "A_Lannister_Always_Pays_His_Debts";
  std::string domainStark = "Winter_Is_Coming";
  std::string nodeLannister = "Lannister";
  std::string nodeStark = "Stark";
  auto node1 = createSpark(domainLannister, nodeLannister, 1);
  auto node2 = createSpark(domainStark, nodeStark, 2);

  // start tracking iface1 and iface2
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_FALSE(
        node1->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(
        node2->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(node1->getSparkNeighState(iface1, nodeStark).has_value());
    EXPECT_FALSE(node2->getSparkNeighState(iface2, nodeLannister).has_value());
  }
}

//
// Start 3 Spark instances in "hub-and-spoke" topology. We prohibit
// node-2 and node-3 to talk to each other. We make node-1
// use two different interfaces for communications.
//
// [node2]  [node3]
//    \       /
//     \     /
//      \   /
//     [node1]
//
TEST_F(Spark2Fixture, HubAndSpokeTopology) {
  const std::string iface1_2{"iface1_2"};
  const std::string iface1_3{"iface1_3"};
  const int ifIndex1_2{12};
  const int ifIndex1_3{13};
  auto ip1V4_2 = folly::IPAddress::createNetwork("192.168.0.12", 24, true);
  auto ip1V4_3 = folly::IPAddress::createNetwork(
      "192.168.0.13", 24, true /* apply mask */);
  auto ip1V6_2 = folly::IPAddress::createNetwork("fe80::12:1/128");
  auto ip1V6_3 = folly::IPAddress::createNetwork("fe80::13:1/128");

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1_2, ifIndex1_2},
                                    {iface1_3, ifIndex1_3},
                                    {iface2, ifIndex2},
                                    {iface3, ifIndex3}});

  ConnectedIfPairs connectedPairs = {{iface1_2, {{iface2, 10}}},
                                     {iface1_3, {{iface3, 10}}},
                                     {iface2, {{iface1_2, 10}}},
                                     {iface3, {{iface1_3, 10}}}};
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark2 instances
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";
  const std::string nodeName3 = "node-3";
  auto node1 = createSpark(kDomainName, nodeName1, 1);
  auto node2 = createSpark(kDomainName, nodeName2, 2);
  auto node3 = createSpark(kDomainName, nodeName3, 3);

  EXPECT_TRUE(
      node1->updateInterfaceDb({{iface1_2, ifIndex1_2, ip1V4_2, ip1V6_2},
                                {iface1_3, ifIndex1_3, ip1V4_3, ip1V6_3}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));
  EXPECT_TRUE(node3->updateInterfaceDb({{iface3, ifIndex3, ip3V4, ip3V6}}));

  // node-1 should hear from node-2 and node-3 on diff interfaces respectively
  {
    std::map<std::string, thrift::SparkNeighborEvent> events;
    for (size_t i = 0; i < 2; i++) {
      auto maybeEvent = node1->waitForEvent(NB_UP);
      EXPECT_TRUE(maybeEvent.has_value());
      events.emplace(maybeEvent.value().neighbor.nodeName, maybeEvent.value());
    }

    ASSERT_EQ(1, events.count(nodeName2));
    ASSERT_EQ(1, events.count(nodeName3));

    auto event1 = events.at(nodeName2);
    EXPECT_EQ(iface1_2, event1.ifName);
    EXPECT_TRUE(nodeName2 == event1.neighbor.nodeName);
    EXPECT_EQ(
        std::make_pair(ip2V4.first, ip2V6.first),
        SparkWrapper::getTransportAddrs(event1));

    auto event2 = events.at(nodeName3);
    EXPECT_EQ(iface1_3, event2.ifName);
    EXPECT_TRUE(nodeName3 == event2.neighbor.nodeName);
    EXPECT_EQ(
        std::make_pair(ip3V4.first, ip3V6.first),
        SparkWrapper::getTransportAddrs(event2));

    LOG(INFO) << nodeName1 << " reported adjacencies to " << nodeName2
              << " and " << nodeName3;
  }

  LOG(INFO) << "Stopping " << nodeName1;
  node1.reset();

  // both node-2 and node-3 should report node1 as restarting &
  // subsequently down after hold-time expiry
  {
    auto event1 = node2->waitForEvent(NB_RESTARTING);
    ASSERT_TRUE(event1.has_value());
    EXPECT_TRUE(event1.value().neighbor.nodeName == nodeName1);

    auto event2 = node3->waitForEvent(NB_RESTARTING);
    ASSERT_TRUE(event2.has_value());
    EXPECT_TRUE(event2.value().neighbor.nodeName == nodeName1);

    // eventually will lose adjacency as node1 never come back
    EXPECT_TRUE(node2->waitForEvent(NB_DOWN).has_value());
    EXPECT_TRUE(node3->waitForEvent(NB_DOWN).has_value());
  }
}

TEST_F(Spark2Fixture, FastInitTest) {
  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // By default, helloMsg is sent out every "kFastInitHelloTime" interval
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";
  auto node1 = createSpark(kDomainName, nodeName1, 1);
  auto node2 = createSpark(kDomainName, nodeName2, 2);

  {
    // Record current timestamp
    const auto startTime = std::chrono::steady_clock::now();

    // start tracking interfaces
    EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
    EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());

    // make sure total time used is limited
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime);
    EXPECT_GE(5 * kFastInitHelloTime.count(), duration.count());
  }

  // kill and restart node-2
  LOG(INFO) << "Killing and restarting: " << nodeName2;

  node2.reset();
  node2 = createSpark(kDomainName, nodeName2, 3 /* changed */);

  {
    const auto startTime = std::chrono::steady_clock::now();
    EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());

    // make sure total time used is limited
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime);
    EXPECT_GE(5 * kFastInitHelloTime.count(), duration.count());
  }
}

//
// Start 2 Spark instances and make sure they form adjacency. Then
// start another Spark instance connecting over the same interface,
// make sure node-1/2 can form adj with node-3 and vice versa.
// Shut down node-3 and make sure adjacency between node-1 and node-2
// is NOT affected.
//
TEST_F(Spark2Fixture, MultiplePeersOverSameInterface) {
  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex(
      {{iface1, ifIndex1}, {iface2, ifIndex2}, {iface3, ifIndex3}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}, {iface3, 10}}},
      {iface2, {{iface1, 10}, {iface3, 10}}},
      {iface3, {{iface1, 10}, {iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark2 instances
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";
  const std::string nodeName3 = "node-3";
  auto node1 = createSpark(kDomainName, nodeName1, 1);
  auto node2 = createSpark(kDomainName, nodeName2, 2);

  // start tracking interfaces
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());
  }

  // add third instance
  LOG(INFO) << "Creating and starting " << nodeName3;

  auto node3 = createSpark(kDomainName, nodeName3, 3);
  EXPECT_TRUE(node3->updateInterfaceDb({{iface3, ifIndex3, ip3V4, ip3V6}}));

  // node-1 and node-2 should hear from node-3
  {
    auto event1 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(iface1, event1->ifName);
    EXPECT_EQ(nodeName3, event1->neighbor.nodeName);
    // ifIndex already used for assigning label to node-2 via iface1. So next
    // label will be assigned from the end.
    EXPECT_EQ(Constants::kSrLocalRange.second, event1->label);
    LOG(INFO) << nodeName1 << " reported adjacency to " << nodeName3;

    auto event2 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(iface2, event2->ifName);
    EXPECT_EQ(nodeName3, event2->neighbor.nodeName);
    // ifIndex already used for assigning label to node-1 via iface2. So next
    // label will be assigned from the end.
    EXPECT_EQ(Constants::kSrLocalRange.second, event2->label);
    LOG(INFO) << nodeName2 << " reported adjacency to " << nodeName3;
  }

  // node-3 should hear from node-1 and node-2 on iface3
  {
    std::map<std::string, thrift::SparkNeighborEvent> events;
    for (int i = 0; i < 2; i++) {
      auto maybeEvent = node3->waitForEvent(NB_UP);
      EXPECT_TRUE(maybeEvent.has_value());
      events.emplace(maybeEvent.value().neighbor.nodeName, maybeEvent.value());
    }

    std::set<int32_t> expectedLabels = {
        Constants::kSrLocalRange.first + ifIndex3,
        Constants::kSrLocalRange.second,
    };

    ASSERT_EQ(1, events.count(nodeName1));
    ASSERT_EQ(1, events.count(nodeName2));

    auto event1 = events.at(nodeName1);
    EXPECT_EQ(iface3, event1.ifName);
    EXPECT_TRUE(nodeName1 == event1.neighbor.nodeName);
    EXPECT_EQ(
        std::make_pair(ip1V4.first, ip1V6.first),
        SparkWrapper::getTransportAddrs(event1));
    ASSERT_TRUE(expectedLabels.count(event1.label));

    auto event2 = events.at(nodeName2);
    EXPECT_EQ(iface3, event2.ifName);
    EXPECT_TRUE(nodeName2 == event2.neighbor.nodeName);
    EXPECT_EQ(
        std::make_pair(ip2V4.first, ip2V6.first),
        SparkWrapper::getTransportAddrs(event2));
    ASSERT_TRUE(expectedLabels.count(event2.label));

    // Label of discovered neighbors must be different on the same interface
    EXPECT_NE(event1.label, event2.label);

    LOG(INFO) << "node-3 reported adjacencies to node-1, node-2";
  }

  // Now stop spark3
  LOG(INFO) << "Stopping " << nodeName3 << " now...";
  node3.reset();

  // node-1 and node-2 should report node-3 down
  {
    auto event1 = node1->waitForEvent(NB_DOWN);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ("node-3", event1->neighbor.nodeName);
    LOG(INFO) << nodeName1 << " reported down adjacency towards " << nodeName3;

    auto event2 = node2->waitForEvent(NB_DOWN);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ("node-3", event2->neighbor.nodeName);
    LOG(INFO) << nodeName2 << " reported down adjacency towards" << nodeName3;
  }

  // node-1 and node-2 should still hold adj with each other
  {
    auto neighState1 = node1->getSparkNeighState(iface1, nodeName2);
    EXPECT_TRUE(neighState1 == ESTABLISHED);

    auto neighState2 = node2->getSparkNeighState(iface2, nodeName1);
    EXPECT_TRUE(neighState2 == ESTABLISHED);
  }
}

//
// Start 2 Spark instances, but block one from hearing another. Then
// shutdown the peer that cannot hear, and make sure there is no DOWN
// event generated for this one.
//
TEST_F(Spark2Fixture, IgnoreUnidirectionalPeer) {
  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark2 instances
  auto node1 = createSpark(kDomainName, "node-1", 1);
  auto node2 = createSpark(kDomainName, "node-2", 2);

  // start tracking interfaces
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_TRUE(node1->recvNeighborEvent(kGRHoldTime * 2).hasError());
    LOG(INFO) << "node-1 doesn't have any neighbor event";

    EXPECT_TRUE(node2->recvNeighborEvent(kGRHoldTime * 2).hasError());
    LOG(INFO) << "node-2 doesn't have any neighbor event";
  }

  {
    // check for neighbor state on node1, should be WARM
    // since will NOT receive helloMsg containing my own info
    EXPECT_TRUE(node1->getSparkNeighState(iface1, "node-2") == WARM);
    LOG(INFO) << "node-1 have neighbor: node-2 in WARM state";

    // check for neighbor state on node2, should return std::nullopt
    // since node2 can't receive pkt from node1
    EXPECT_FALSE(node2->getSparkNeighState(iface2, "node-1").has_value());
    LOG(INFO) << "node-2 doesn't have any neighbor";
  }
}

//
// Start 1 Spark instace and make its interfaces connected to its own
// Make sure pkt loop can be handled gracefully and no ADJ will be formed.
//
TEST_F(Spark2Fixture, LoopedHelloPktTest) {
  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}});

  // connect iface1 directly with itself to mimick
  // self-looped helloPkt
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface1, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start one spark2 instance
  auto node1 = createSpark(kDomainName, "node-1", 1);

  // start tracking iface1.
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));

  // should NOT receive any event( e.g.NEIGHBOR_DOWN)
  {
    EXPECT_FALSE(
        node1->waitForEvent(NB_DOWN, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(node1->getSparkNeighState(iface1, "node-1").has_value());
  }
}

//
// Start 2 Spark instances within different v4 subnet. Then
// make sure they can't form adj as NEGOTIATION failed. Bring
// down the interface and make sure no crash happened for tracked
// neighbors. Then put them in same subnet, make sure instances
// will form adj with each other.
//
TEST_F(Spark2Fixture, LinkDownWithoutAdjFormed) {
  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark2 instances
  auto node1 = createSpark(kDomainName, "node-1", 1);
  auto node2 = createSpark(kDomainName, "node-2", 2);

  // enable v4 subnet validation to put adddres in different /31 subnet
  // on purpose.
  const folly::CIDRNetwork ip1V4WithSubnet =
      folly::IPAddress::createNetwork("192.168.0.2", 31);
  const folly::CIDRNetwork ip2V4WithSameSubnet =
      folly::IPAddress::createNetwork("192.168.0.3", 31);
  const folly::CIDRNetwork ip2V4WithDiffSubnet =
      folly::IPAddress::createNetwork("192.168.0.4", 31);

  // start tracking iface1
  EXPECT_TRUE(
      node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4WithSubnet, ip1V6}}));

  // start tracking iface2
  EXPECT_TRUE(node2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4WithDiffSubnet, ip2V6}}));

  // won't form adj as v4 validation should fail
  {
    EXPECT_FALSE(
        node1->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());

    EXPECT_FALSE(
        node2->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
  }

  {
    // bring down interface of node1 to make sure no crash happened
    EXPECT_TRUE(node1->updateInterfaceDb({}));

    // bring up interface of node1 to make sure no crash happened
    EXPECT_TRUE(
        node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4WithSubnet, ip1V6}}));
  }

  {
    // bring up interface with SAME subnet and verify ADJ UP event
    EXPECT_TRUE(node2->updateInterfaceDb(
        {{iface2, ifIndex2, ip2V4WithSameSubnet, ip2V6}}));

    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());

    LOG(INFO) << "node-1 and node-2 successfully form adjacency";
  }
}

//
// Start 2 Spark instances within different v4 subnet. Then
// make sure they can't form adj as NEGOTIATION failed. Check
// neighbor state within NEGOTIATE/WARM depending on whether
// new helloMsg is received.
//
TEST_F(Spark2Fixture, InvalidV4Subnet) {
  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark2 instances
  std::string nodeName1 = "node-1";
  std::string nodeName2 = "node-2";
  auto node1 = createSpark(kDomainName, nodeName1, 1);
  auto node2 = createSpark(kDomainName, nodeName2, 2);

  // enable v4 subnet validation to put adddres in different /31 subnet
  // on purpose.
  const folly::CIDRNetwork ip1V4WithSubnet =
      folly::IPAddress::createNetwork("192.168.0.2", 31);
  const folly::CIDRNetwork ip2V4WithDiffSubnet =
      folly::IPAddress::createNetwork("192.168.0.4", 31);

  // start tracking iface1 and iface2
  EXPECT_TRUE(
      node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4WithSubnet, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4WithDiffSubnet, ip2V6}}));

  // won't form adj as v4 validation should fail
  {
    EXPECT_FALSE(
        node1->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());

    EXPECT_FALSE(
        node2->waitForEvent(NB_DOWN, kGRHoldTime, kGRHoldTime * 2).has_value());
  }

  // check neighbor state: should be in WARM/NEGOTIATE stage
  {
    auto neighState1 = node1->getSparkNeighState(iface1, nodeName2);
    EXPECT_TRUE(neighState1 == WARM || neighState1 == NEGOTIATE);

    auto neighState2 = node2->getSparkNeighState(iface2, nodeName1);
    EXPECT_TRUE(neighState2 == WARM || neighState2 == NEGOTIATE);
  }
}

//
// Positive case for AREA:
//
// Start 2 Spark instances with areaConfig and make sure they
// can form adj with each other in specified AREA.
//
TEST_F(Spark2Fixture, AreaMatch) {
  // Explicitly set regex to be capital letters to make sure
  // regex is NOT case-sensative
  auto areaConfig11 = SparkWrapper::createAreaConfig(area1, {"RSW.*"}, {".*"});
  auto areaConfig12 = SparkWrapper::createAreaConfig(area2, {"FSW.*"}, {".*"});
  auto areaConfig21 = SparkWrapper::createAreaConfig(area1, {"FSW.*"}, {".*"});
  auto areaConfig22 = SparkWrapper::createAreaConfig(area2, {"RSW.*"}, {".*"});

  // RSW: { 1 -> "RSW.*", 2 -> "FSW.*"}
  // FSW: { 1 -> "FSW.*", 2 -> "RSW.*"}
  auto config1 = std::make_shared<thrift::OpenrConfig>();
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  config1->areas.emplace_back(areaConfig11);
  config1->areas.emplace_back(areaConfig12);
  config2->areas.emplace_back(areaConfig21);
  config2->areas.emplace_back(areaConfig22);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, config1);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking iface1 and iface2
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  // RSW001 and FSW002 node should form adj in area 2 due to regex matching
  {
    auto event1 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(event1.value().neighbor.nodeName, nodeName2);
    EXPECT_EQ(event1.value().area, area2);

    auto event2 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(event2.value().neighbor.nodeName, nodeName1);
    EXPECT_EQ(event2.value().area, area2);

    LOG(INFO) << nodeName1 << " and " << nodeName2
              << " formed adjacency with each other...";
  }
}

//
// Negative case for AREA:
//
// Start 2 Spark instances with areaConfig and make sure they
// can NOT form adj due to wrong AREA regex matching.
//
TEST_F(Spark2Fixture, NoAreaMatch) {
  // AreaConfig:
  //  rsw001: { 1 -> "RSW.*"}
  //  fsw002: { 1 -> "FSW.*"}
  //
  //  rsw001 and fsw002 will receive each other's helloMsg, but won't proceed.
  //  rsw001 can ONLY pair with "RSW.*", whereas fsw002 can ONLY pair with
  //  "FSW.*".
  auto areaConfig1 = SparkWrapper::createAreaConfig(area1, {"RSW.*"}, {".*"});
  auto areaConfig2 = SparkWrapper::createAreaConfig(area1, {"FSW.*"}, {".*"});

  auto config1 = std::make_shared<thrift::OpenrConfig>();
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  config1->areas.emplace_back(areaConfig1);
  config2->areas.emplace_back(areaConfig2);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, config1);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking iface1 and iface2
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_FALSE(
        node1->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(
        node2->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(node1->getSparkNeighState(iface1, nodeName2).has_value());
    EXPECT_FALSE(node2->getSparkNeighState(iface2, nodeName1).has_value());
  }
}

//
// Negative case for AREA:
//
// Start 2 Spark instances with areaConfig and make sure they
// can NOT form adj due to inconsistent AREA negotiation result.
//
TEST_F(Spark2Fixture, InconsistentAreaNegotiation) {
  // AreaConfig:
  //  rsw001: { 1 -> "FSW.*"}
  //  fsw002: { 2 -> "RSW.*"}
  //
  //  rsw001 and fsw002 will receive each other's helloMsg and proceed to
  //  NEGOTIATE stage. However, rsw001 thinks fsw002 should reside in
  //  area "1", whereas fsw002 thinks rsw001 should be in area "2".
  //
  //  AREA negotiation won't go through. Will fall back to WARM
  auto areaConfig1 = SparkWrapper::createAreaConfig(area1, {"FSW.*"}, {".*"});
  auto areaConfig2 = SparkWrapper::createAreaConfig(area2, {"RSW.*"}, {".*"});

  auto config1 = std::make_shared<thrift::OpenrConfig>();
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  config1->areas.emplace_back(areaConfig1);
  config2->areas.emplace_back(areaConfig2);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, config1);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking iface1 and iface2
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_FALSE(
        node1->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());
    EXPECT_FALSE(
        node2->waitForEvent(NB_UP, kGRHoldTime, kGRHoldTime * 2).has_value());

    auto neighState1 = node1->getSparkNeighState(iface1, nodeName2);
    EXPECT_TRUE(neighState1 == WARM || neighState1 == NEGOTIATE);

    auto neighState2 = node2->getSparkNeighState(iface2, nodeName1);
    EXPECT_TRUE(neighState2 == WARM || neighState2 == NEGOTIATE);
  }
}

//
// Positive case for AREA:
//
// Start 1 Spark without AREA config supported, whereas starting
// another Spark with areaConfig passed in. Make sure they can
// form adj in `defaultArea` for backward compatibility.
//
TEST_F(Spark2Fixture, NoAreaSupportNegotiation) {
  // AreaConfig:
  //  rsw001: {}
  //  fsw002: { 2 -> "RSW.*"}
  //
  //  rsw001 doesn't know anything about AREA, whereas fsw002 is configured
  //  with areaConfig. Make sure AREA negotiation will go through and they can
  //  form adj inside `defaultArea`.
  auto areaConfig2 = SparkWrapper::createAreaConfig(area2, {"RSW.*"}, {".*"});
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  config2->areas.emplace_back(areaConfig2);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, nullptr);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking iface1 and iface2
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    auto event1 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(event1.value().neighbor.nodeName, nodeName2);
    EXPECT_EQ(event1.value().area, defaultArea);

    auto event2 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(event2.value().neighbor.nodeName, nodeName1);
    EXPECT_EQ(event2.value().area, defaultArea);
  }
}

//
// Start 2 Spark with AREA config supported and make sure they can
// form adj. Then add another Spark. Make sure 3rd Spark instance
// can form adj with different peers within different area over the
// same interface.
//
TEST_F(Spark2Fixture, MultiplePeersWithDiffAreaOverSameLink) {
  // AreaConfig:
  //  rsw001: { 1 -> {"FSW.*"}, 2 -> {"SSW.*"}}
  //  fsw002: { 1 -> {"RSW.*", "SSW.*"}}
  //  ssw003: { 1 -> {"FSW.*"}, 2 -> {"RSW.*"}}
  //
  //  Based on topology setup, expected adj pairs:
  //    rsw001 <==> fsw002
  //    fsw002 <==> ssw003
  //    ssw003 <==> rsw001
  auto areaConfig11 = SparkWrapper::createAreaConfig(area1, {"FSW.*"}, {".*"});
  auto areaConfig12 = SparkWrapper::createAreaConfig(area2, {"SSW.*"}, {".*"});
  auto areaConfig2 =
      SparkWrapper::createAreaConfig(area1, {"RSW.*", "SSW.*"}, {".*"});
  auto areaConfig31 = SparkWrapper::createAreaConfig(area1, {"fsw.*"}, {".*"});
  auto areaConfig32 = SparkWrapper::createAreaConfig(area2, {"rsw.*"}, {".*"});

  auto config1 = std::make_shared<thrift::OpenrConfig>();
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  auto config3 = std::make_shared<thrift::OpenrConfig>();
  config1->areas.emplace_back(areaConfig11);
  config1->areas.emplace_back(areaConfig12);
  config2->areas.emplace_back(areaConfig2);
  config3->areas.emplace_back(areaConfig31);
  config3->areas.emplace_back(areaConfig32);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex(
      {{iface1, ifIndex1}, {iface2, ifIndex2}, {iface3, ifIndex3}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}, {iface3, 10}}},
      {iface2, {{iface1, 10}, {iface3, 10}}},
      {iface3, {{iface1, 10}, {iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, config1);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking interfaces
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    auto event1 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(iface1, event1->ifName);
    EXPECT_EQ(nodeName2, event1->neighbor.nodeName);
    EXPECT_EQ(event1->area, area1);
    LOG(INFO) << nodeName1 << " reported adjacency to " << nodeName2;

    auto event2 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(iface2, event2->ifName);
    EXPECT_EQ(nodeName1, event2->neighbor.nodeName);
    EXPECT_EQ(event1->area, area1);
    LOG(INFO) << nodeName2 << " reported adjacency to " << nodeName1;
  }

  // add third instance
  std::string nodeName3 = "ssw003";
  auto node3 = createSpark(kDomainName, nodeName3, 3, true, true, config3);
  EXPECT_TRUE(node3->updateInterfaceDb({{iface3, ifIndex3, ip3V4, ip3V6}}));

  LOG(INFO) << nodeName3 << " being started...";

  // rsw001 and fsw002 should form adj with ssw003 in area2, area1 respectively
  {
    auto event1 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(iface2, event1->ifName);
    EXPECT_EQ(nodeName3, event1->neighbor.nodeName);
    EXPECT_EQ(event1->area, area1);
    LOG(INFO) << nodeName2 << " reported adjacency to " << nodeName3;

    auto event2 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(iface1, event2->ifName);
    EXPECT_EQ(nodeName3, event2->neighbor.nodeName);
    EXPECT_EQ(event2->area, area2);
    LOG(INFO) << nodeName1 << " reported adjacency to " << nodeName3;
  }

  // ssw003 should hear from rsw001 and fsw002 on iface3 in DIFF area
  {
    std::map<std::string, thrift::SparkNeighborEvent> events;
    for (int i = 0; i < 2; i++) {
      auto maybeEvent = node3->waitForEvent(NB_UP);
      EXPECT_TRUE(maybeEvent.has_value());
      events.emplace(maybeEvent.value().neighbor.nodeName, maybeEvent.value());
    }

    auto event1 = events.at(nodeName1);
    EXPECT_EQ(iface3, event1.ifName);
    EXPECT_EQ(nodeName1, event1.neighbor.nodeName);
    EXPECT_EQ(area2, event1.area);
    LOG(INFO) << nodeName3 << " reported adjacency to " << nodeName1;

    auto event2 = events.at(nodeName2);
    EXPECT_EQ(iface3, event2.ifName);
    EXPECT_TRUE(nodeName2 == event2.neighbor.nodeName);
    EXPECT_EQ(area1, event2.area);
    LOG(INFO) << nodeName3 << " reported adjacency to " << nodeName2;
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  CHECK(!::sodium_init());

  // Run the tests
  return RUN_ALL_TESTS();
}
