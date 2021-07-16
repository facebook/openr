/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <thread>

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sodium.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <fb303/ServiceData.h>
#include <openr/common/Constants.h>
#include <openr/common/MplsUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/spark/SparkWrapper.h>
#include <openr/tests/mocks/MockIoProvider.h>

using namespace openr;

namespace fb303 = facebook::fb303;

namespace {
const std::string iface1{"iface1"};
const std::string iface2{"iface2"};
const std::string iface3{"iface3"};

const int ifIndex1{1};
const int ifIndex2{2};
const int ifIndex3{3};

const std::string area1{"area1"};
const std::string area2{"area2"};
const std::string area3{"area3"};
const std::string defaultArea{thrift::Types_constants::kDefaultArea()};

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
const auto NB_UP = NeighborEventType::NEIGHBOR_UP;
const auto NB_DOWN = NeighborEventType::NEIGHBOR_DOWN;
const auto NB_RESTARTING = NeighborEventType::NEIGHBOR_RESTARTING;
const auto NB_RESTARTED = NeighborEventType::NEIGHBOR_RESTARTED;
const auto NB_RTT_CHANGE = NeighborEventType::NEIGHBOR_RTT_CHANGE;

// alias for neighbor state
const auto WARM = SparkNeighState::WARM;
const auto NEGOTIATE = SparkNeighState::NEGOTIATE;
const auto ESTABLISHED = SparkNeighState::ESTABLISHED;
const auto RESTART = SparkNeighState::RESTART;

// Domain name (same for all Tests except in DomainTest)
const std::string kDomainName("Fire_and_Blood");
}; // namespace

class SparkFixture : public testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider_ = std::make_shared<MockIoProvider>();

    // Start mock IoProvider thread
    mockIoProviderThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting mockIoProvider thread.";
      mockIoProvider_->start();
      LOG(INFO) << "mockIoProvider thread got stopped.";
    });
    mockIoProvider_->waitUntilRunning();
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping mockIoProvider thread.";
    mockIoProvider_->stop();
    mockIoProviderThread_->join();
  }

  std::shared_ptr<SparkWrapper>
  createSpark(
      std::string const& myNodeName,
      std::shared_ptr<const Config> config = nullptr,
      std::pair<uint32_t, uint32_t> version = std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion)) {
    return std::make_unique<SparkWrapper>(
        myNodeName, version, mockIoProvider_, config);
  }

  std::shared_ptr<MockIoProvider> mockIoProvider_{nullptr};
  std::unique_ptr<std::thread> mockIoProviderThread_{nullptr};
};

class SimpleSparkFixture : public SparkFixture {
 protected:
  void
  createAndConnect() {
    // Define interface names for the test
    mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

    // connect interfaces directly
    ConnectedIfPairs connectedPairs = {
        {iface1, {{iface2, 10}}},
        {iface2, {{iface1, 10}}},
    };
    mockIoProvider_->setConnectedPairs(connectedPairs);

    auto tConfig1 = getBasicOpenrConfig("node-1", kDomainName);
    auto config1 = std::make_shared<Config>(tConfig1);

    auto tConfig2 = getBasicOpenrConfig("node-2", kDomainName);
    auto config2 = std::make_shared<Config>(tConfig2);

    // start one spark2 instance
    node1 = createSpark("node-1", config1);

    // start another spark2 instance
    node2 = createSpark("node-2", config2);

    // start tracking iface1
    node1->updateInterfaceDb({InterfaceInfo(
        iface1 /* ifName */,
        true /* isUp */,
        ifIndex1 /* ifIndex */,
        {ip1V4, ip1V6} /* networks */)});

    // start tracking iface2
    node2->updateInterfaceDb({InterfaceInfo(
        iface2 /* ifName */,
        true /* isUp */,
        ifIndex2 /* ifIndex */,
        {ip2V4, ip2V6} /* networks */)});

    LOG(INFO) << "Start to receive messages from Spark";

    // Now wait for sparks to detect each other
    {
      auto event = node1->waitForEvent(NB_UP);
      ASSERT_TRUE(event.has_value());
      EXPECT_EQ(iface1, *event.value().info.localIfName_ref());
      EXPECT_EQ("node-2", *event.value().info.nodeName_ref());
      EXPECT_EQ(
          std::make_pair(ip2V4.first, ip2V6.first),
          SparkWrapper::getTransportAddrs(*event));
      LOG(INFO) << "node-1 reported adjacency to node-2";
    }

    {
      auto event = node2->waitForEvent(NB_UP);
      ASSERT_TRUE(event.has_value());
      EXPECT_EQ(iface2, *event.value().info.localIfName_ref());
      EXPECT_EQ("node-1", *event.value().info.nodeName_ref());
      EXPECT_EQ(
          std::make_pair(ip1V4.first, ip1V6.first),
          SparkWrapper::getTransportAddrs(*event));
      LOG(INFO) << "node-2 reported adjacency to node-1";
    }
  }

  void
  checkCounters() {
    auto counters = fb303::fbData->getCounters();
    // Verify the counter keys exist
    ASSERT_EQ(1, counters.count("slo.neighbor_discovery.time_ms.avg"));
    ASSERT_EQ(1, counters.count("slo.neighbor_discovery.time_ms.avg.3600"));
    ASSERT_EQ(1, counters.count("slo.neighbor_discovery.time_ms.avg.60"));
    ASSERT_EQ(1, counters.count("slo.neighbor_discovery.time_ms.avg.600"));

    ASSERT_EQ(1, counters.count("slo.neighbor_restart.time_ms.avg"));
    ASSERT_EQ(1, counters.count("slo.neighbor_restart.time_ms.avg.3600"));
    ASSERT_EQ(1, counters.count("slo.neighbor_restart.time_ms.avg.60"));
    ASSERT_EQ(1, counters.count("slo.neighbor_restart.time_ms.avg.600"));

    // Neighbor discovery should be less than 3 secs
    ASSERT_GE(3000, counters["slo.neighbor_discovery.time_ms.avg"]);
    ASSERT_GE(3000, counters["slo.neighbor_discovery.time_ms.avg.3600"]);
    ASSERT_GE(3000, counters["slo.neighbor_discovery.time_ms.avg.60"]);
    ASSERT_GE(3000, counters["slo.neighbor_discovery.time_ms.avg.600"]);
  }

  std::shared_ptr<SparkWrapper> node1;
  std::shared_ptr<SparkWrapper> node2;
};

//
// Start 2 Spark instances and wait them forming adj.
// Verify public API works as expected and check neighbor state.
//
TEST_F(SimpleSparkFixture, GetNeighborsTest) {
  // create Spark instances and establish connections
  createAndConnect();

  // get SparkNeigborDb via public API
  auto db1 = *(node1->get()->getNeighbors().get());
  auto db2 = *(node2->get()->getNeighbors().get());

  EXPECT_EQ(1, db1.size());
  EXPECT_EQ(1, db2.size());

  // verify db content for individual neighbor
  auto neighbor1 = db2.back();
  auto neighbor2 = db1.back();

  EXPECT_EQ(*neighbor1.state_ref(), Spark::toStr(ESTABLISHED));
  EXPECT_EQ(*neighbor2.state_ref(), Spark::toStr(ESTABLISHED));
  EXPECT_EQ(*neighbor1.localIfName_ref(), iface2);
  EXPECT_EQ(*neighbor1.remoteIfName_ref(), iface1);
  EXPECT_EQ(*neighbor2.localIfName_ref(), iface1);
  EXPECT_EQ(*neighbor2.remoteIfName_ref(), iface2);
}

//
// Start 2 Spark instances and wait them forming adj. Then
// force to send helloMsg with restarting flag indicating GR.
// Verify public API works as expected and check neighbor state.
//
TEST_F(SimpleSparkFixture, ForceGRMsgTest) {
  // create Spark instances and establish connections
  createAndConnect();

  // force to send out helloMsg with restarting flag
  node1->get()->floodRestartingMsg();
  node2->get()->floodRestartingMsg();

  // should report each other as 'RESTARTING'
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";

  {
    auto event1 = node1->waitForEvent(NB_RESTARTING);
    auto neighState1 = node1->getSparkNeighState(iface1, nodeName2);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(iface1, *event1.value().info.localIfName_ref());
    EXPECT_TRUE(nodeName2 == *event1.value().info.nodeName_ref());
    EXPECT_TRUE(neighState1 == RESTART);

    LOG(INFO) << "node-1 reported node-2 as RESTARTING";
  }

  {
    auto event2 = node2->waitForEvent(NB_RESTARTING);
    auto neighState2 = node2->getSparkNeighState(iface2, nodeName1);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(iface2, *event2.value().info.localIfName_ref());
    EXPECT_TRUE(nodeName1 == *event2.value().info.nodeName_ref());
    EXPECT_TRUE(neighState2 == RESTART);

    LOG(INFO) << "node-2 reported node-1 as RESTARTING";
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// increase/decrease RTT, expect NEIGHBOR_RTT_CHANGE event
//
TEST_F(SimpleSparkFixture, RttTest) {
  // create Spark instances and establish connections
  createAndConnect();

  LOG(INFO) << "Change rtt between nodes to 40ms (asymmetric)";

  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 15}}},
      {iface2, {{iface1, 25}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // wait for spark nodes to detecct Rtt change
  {
    auto event = node1->waitForEvent(NB_RTT_CHANGE);
    ASSERT_TRUE(event.has_value());
    // 25% tolerance
    auto rtt = *event->info.rttUs_ref();
    EXPECT_GE(rtt, (40 - 10) * 1000);
    EXPECT_LE(rtt, (40 + 10) * 1000);
    LOG(INFO) << "node-1 reported new RTT to node-2 to be " << rtt / 1000.0
              << "ms";
  }

  {
    auto event = node2->waitForEvent(NB_RTT_CHANGE);
    ASSERT_TRUE(event.has_value());
    // 25% tolerance
    auto rtt = *event->info.rttUs_ref();
    EXPECT_GE(rtt, (40 - 10) * 1000);
    EXPECT_LE(rtt, (40 + 10) * 1000);
    LOG(INFO) << "node-2 reported new RTT to node-1 to be " << rtt / 1000.0
              << "ms";
  }

  checkCounters();
}

//
// Start 2 Spark instances and wait them forming adj. Then
// make it uni-directional, expect both side to lose adj
// due to missing node info in `ReflectedNeighborInfo`
//
TEST_F(SimpleSparkFixture, UnidirectionTest) {
  // create Spark instances and establish connections
  createAndConnect();

  LOG(INFO) << "Stopping communications from iface2 to iface1";

  // stop packet flowing iface2 -> iface1. Expect both ends drops
  //  1. node1 drops due to: heartbeat hold timer expired
  //  2. node2 drops due to: helloMsg doesn't contains neighborInfo
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

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
TEST_F(SimpleSparkFixture, GRTest) {
  // create Spark instances and establish connections
  createAndConnect();

  // Kill node2
  LOG(INFO) << "Kill and restart node-2";

  node2.reset();

  // node-1 should report node-2 as 'RESTARTING'
  {
    EXPECT_TRUE(node1->waitForEvent(NB_RESTARTING).has_value());
    LOG(INFO) << "node-1 reported node-2 as RESTARTING";
  }

  auto tConfig2 = getBasicOpenrConfig("node-2", kDomainName);
  auto config2 = std::make_shared<Config>(tConfig2);

  node2 = createSpark("node-2", config2);

  LOG(INFO) << "Adding iface2 to node-2 to let it start helloMsg adverstising";

  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

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
    const auto& graceful_restart_time_s1 = std::chrono::seconds(
        folly::copy(*node1->getSparkConfig().graceful_restart_time_s_ref()));
    const auto& graceful_restart_time_s2 = std::chrono::seconds(
        folly::copy(*node2->getSparkConfig().graceful_restart_time_s_ref()));
    EXPECT_FALSE(
        node1
            ->waitForEvent(
                NB_DOWN, graceful_restart_time_s1, graceful_restart_time_s1 * 2)
            .has_value());
    EXPECT_FALSE(
        node2
            ->waitForEvent(
                NB_DOWN, graceful_restart_time_s2, graceful_restart_time_s2 * 2)
            .has_value());
  }

  checkCounters();
}

//
// Start 2 Spark instances and wait them forming adj. Then
// gracefully shut down one of them but NOT bring it back,
// make sure we get neighbor "DOWN" event due to GR timer expiring.
//
TEST_F(SimpleSparkFixture, GRTimerExpireTest) {
  // create Spark instances and establish connections
  createAndConnect();

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
    const auto& graceful_restart_time_s1 = std::chrono::seconds(
        folly::copy(*node1->getSparkConfig().graceful_restart_time_s_ref()));
    ASSERT_TRUE(endTime - startTime >= graceful_restart_time_s1);

    ASSERT_TRUE(
        endTime - startTime <= graceful_restart_time_s1 +
            std::chrono::seconds(*node1->getSparkConfig().hold_time_s_ref()));
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// stop the bi-direction communication from each other.
// Observe neighbor going DOWN due to hold timer expiration.
//
TEST_F(SimpleSparkFixture, HeartbeatTimerExpireTest) {
  // create Spark instances and establish connections
  createAndConnect();

  // record time for future comparison
  auto startTime = std::chrono::steady_clock::now();

  // remove underneath connections between to nodes
  ConnectedIfPairs connectedPairs = {};
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // wait for sparks to lose each other
  {
    LOG(INFO) << "Waiting for both nodes to time out with each other";

    EXPECT_TRUE(node1->waitForEvent(NB_DOWN).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_DOWN).has_value());

    // record time for expiration time test
    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        endTime - startTime >=
        std::chrono::seconds(*node1->getSparkConfig().hold_time_s_ref()));
    ASSERT_TRUE(
        endTime - startTime <=
        std::chrono::seconds(
            *node1->getSparkConfig().graceful_restart_time_s_ref()));
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// update interface from one instance's perspective. Due to same
// interface, there should be no interface removal/adding.
//
TEST_F(SimpleSparkFixture, InterfaceUpdateTest) {
  // create Spark instances and establish connections
  createAndConnect();

  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});

  // since the removal of intf happens instantly. down event should
  // be reported ASAP.
  auto waitTime = std::chrono::seconds(
      *node1->getSparkConfig().graceful_restart_time_s_ref());

  EXPECT_FALSE(
      node1->waitForEvent(NB_DOWN, waitTime, waitTime * 2).has_value());
  EXPECT_FALSE(node1->waitForEvent(NB_UP, waitTime, waitTime * 2).has_value());
}

//
// Start 2 Spark instances and wait them forming adj. Then
// remove/add interface from one instance's perspective
//
TEST_F(SimpleSparkFixture, InterfaceRemovalTest) {
  // create Spark instances and establish connections
  createAndConnect();

  auto startTime = std::chrono::steady_clock::now();
  auto waitTime = std::chrono::seconds(
      *node1->getSparkConfig().graceful_restart_time_s_ref());

  // tell node1 to remove interface to mimick request from linkMonitor
  node1->updateInterfaceDb({});

  LOG(INFO) << "Waiting for node-1 to report loss of adj to node-2";

  // since the removal of intf happens instantly. down event should
  // be reported ASAP.
  {
    EXPECT_TRUE(node1->waitForEvent(NB_DOWN).has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        endTime - startTime <=
        std::min(
            waitTime,
            std::chrono::seconds(*node1->getSparkConfig().hold_time_s_ref())));
    LOG(INFO)
        << "node-1 reported down adjacency to node-2 due to interface removal";
  }

  {
    EXPECT_TRUE(node2->waitForEvent(NB_DOWN).has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime <= waitTime);
    LOG(INFO)
        << "node-2 reported down adjacency to node-2 due to heartbeat expired";
  }

  {
    // should NOT receive any event after down adj
    EXPECT_FALSE(node1->recvNeighborEvent(waitTime).has_value());
    EXPECT_FALSE(node2->recvNeighborEvent(waitTime).has_value());
  }

  // Resume interface connection
  LOG(INFO) << "Bringing iface-1 back online";

  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  startTime = std::chrono::steady_clock::now();

  {
    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        endTime - startTime <=
        std::chrono::seconds(*node1->getSparkConfig().hold_time_s_ref()) +
            std::chrono::seconds(
                *node1->getSparkConfig().keepalive_time_s_ref()));
    LOG(INFO) << "node-1 reported up adjacency to node-2";
  }

  {
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        endTime - startTime <=
        std::chrono::seconds(*node1->getSparkConfig().hold_time_s_ref()) +
            std::chrono::seconds(
                *node1->getSparkConfig().keepalive_time_s_ref()));
    LOG(INFO) << "node-2 reported up adjacency to node-1";
  }
}

//
// Start 2 Spark instances for different versions but within supported
// range. Make sure they will form adjacency. Then add node3 with out-of-range
// version. Confirm node3 can't form adjacency with neither of node1/node2
// bi-directionally.
//
TEST_F(SparkFixture, VersionTest) {
  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex(
      {{iface1, ifIndex1}, {iface2, ifIndex2}, {iface3, ifIndex3}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}, {iface3, 10}}},
      {iface2, {{iface1, 10}, {iface3, 10}}},
      {iface3, {{iface1, 10}, {iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // start node1, node2 with different but within supported range
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";
  const std::string nodeName3 = "node-3";

  auto tConfig1 = getBasicOpenrConfig(nodeName1, kDomainName);
  auto config1 = std::make_shared<Config>(tConfig1);

  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName);
  auto config2 = std::make_shared<Config>(tConfig2);

  auto tConfig3 = getBasicOpenrConfig(nodeName3, kDomainName);
  auto config3 = std::make_shared<Config>(tConfig3);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  // start tracking interfaces
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  {
    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());
  }

  LOG(INFO) << "Starting: " << nodeName3;

  auto node3 = createSpark(
      nodeName3,
      config3,
      std::make_pair(
          Constants::kOpenrSupportedVersion - 1,
          Constants::kOpenrSupportedVersion - 1));

  // start tracking interfaces
  node3->updateInterfaceDb({InterfaceInfo(
      iface3 /* ifName */,
      true /* isUp */,
      ifIndex3 /* ifIndex */,
      {ip3V4, ip3V6} /* networks */)});

  // node3 can't form adj with neither node1 nor node2
  {
    const auto& restart_time_s1 = std::chrono::seconds(
        *config1->getSparkConfig().graceful_restart_time_s_ref());
    const auto& restart_time_s2 = std::chrono::seconds(
        *config2->getSparkConfig().graceful_restart_time_s_ref());
    const auto& restart_time_s3 = std::chrono::seconds(
        *config3->getSparkConfig().graceful_restart_time_s_ref());

    EXPECT_FALSE(
        node1->waitForEvent(NB_UP, restart_time_s1, restart_time_s1 * 2)
            .has_value());
    EXPECT_FALSE(
        node2->waitForEvent(NB_UP, restart_time_s2, restart_time_s2 * 2)
            .has_value());
    EXPECT_FALSE(
        node3->waitForEvent(NB_UP, restart_time_s3, restart_time_s3 * 2)
            .has_value());
  }
}

//
// Start 2 Spark instances within different domains. Then
// make sure they can't form adj as helloMsg being ignored.
//
TEST_F(SparkFixture, DomainTest) {
  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // start 2 spark instances within different domain
  std::string domainLannister = "A_Lannister_Always_Pays_His_Debts";
  std::string domainStark = "Winter_Is_Coming";
  std::string nodeLannister = "Lannister";
  std::string nodeStark = "Stark";

  auto tConfig1 = getBasicOpenrConfig(nodeLannister, domainLannister);
  auto config1 = std::make_shared<Config>(tConfig1);

  auto tConfig2 = getBasicOpenrConfig(nodeStark, domainStark);
  auto config2 = std::make_shared<Config>(tConfig2);

  auto node1 = createSpark(nodeLannister, config1);
  auto node2 = createSpark(nodeStark, config2);

  // start tracking iface1 and iface2
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  {
    const auto& restart_time_s1 = std::chrono::seconds(
        *config1->getSparkConfig().graceful_restart_time_s_ref());
    const auto& restart_time_s2 = std::chrono::seconds(
        *config2->getSparkConfig().graceful_restart_time_s_ref());

    EXPECT_FALSE(
        node1->waitForEvent(NB_UP, restart_time_s1, restart_time_s1 * 2)
            .has_value());
    EXPECT_FALSE(
        node2->waitForEvent(NB_UP, restart_time_s2, restart_time_s2 * 2)
            .has_value());
    EXPECT_EQ(
        node1->getSparkNeighState(iface1, nodeStark).value(),
        SparkNeighState::WARM);
    EXPECT_EQ(
        node2->getSparkNeighState(iface2, nodeLannister).value(),
        SparkNeighState::WARM);
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
TEST_F(SparkFixture, HubAndSpokeTopology) {
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
  mockIoProvider_->addIfNameIfIndex(
      {{iface1_2, ifIndex1_2},
       {iface1_3, ifIndex1_3},
       {iface2, ifIndex2},
       {iface3, ifIndex3}});

  ConnectedIfPairs connectedPairs = {
      {iface1_2, {{iface2, 10}}},
      {iface1_3, {{iface3, 10}}},
      {iface2, {{iface1_2, 10}}},
      {iface3, {{iface1_3, 10}}}};
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // start spark2 instances
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";
  const std::string nodeName3 = "node-3";

  auto tConfig1 = getBasicOpenrConfig(nodeName1, kDomainName);
  auto config1 = std::make_shared<Config>(tConfig1);

  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName);
  auto config2 = std::make_shared<Config>(tConfig2);

  auto tConfig3 = getBasicOpenrConfig(nodeName3, kDomainName);
  auto config3 = std::make_shared<Config>(tConfig3);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);
  auto node3 = createSpark(nodeName3, config3);

  node1->updateInterfaceDb({
      InterfaceInfo(
          iface1_2 /* ifName */,
          true /* isUp */,
          ifIndex1_2 /* ifIndex */,
          {ip1V4_2, ip1V6_2} /* networks */),
      InterfaceInfo(
          iface1_3 /* ifName */,
          true /* isUp */,
          ifIndex1_3 /* ifIndex */,
          {ip1V4_3, ip1V6_3} /* networks */),
  });
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});
  node3->updateInterfaceDb({InterfaceInfo(
      iface3 /* ifName */,
      true /* isUp */,
      ifIndex3 /* ifIndex */,
      {ip3V4, ip3V6} /* networks */)});

  // node-1 should hear from node-2 and node-3 on diff interfaces respectively
  {
    std::map<std::string, NeighborEvent> events;
    for (size_t i = 0; i < 2; i++) {
      auto maybeEvent = node1->waitForEvent(NB_UP);
      EXPECT_TRUE(maybeEvent.has_value());
      events.emplace(
          *maybeEvent.value().info.nodeName_ref(), maybeEvent.value());
    }

    ASSERT_EQ(1, events.count(nodeName2));
    ASSERT_EQ(1, events.count(nodeName3));

    auto event1 = events.at(nodeName2);
    EXPECT_EQ(iface1_2, *event1.info.localIfName_ref());
    EXPECT_TRUE(nodeName2 == *event1.info.nodeName_ref());
    EXPECT_EQ(
        std::make_pair(ip2V4.first, ip2V6.first),
        SparkWrapper::getTransportAddrs(event1));

    auto event2 = events.at(nodeName3);
    EXPECT_EQ(iface1_3, *event2.info.localIfName_ref());
    EXPECT_TRUE(nodeName3 == *event2.info.nodeName_ref());
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
    EXPECT_TRUE(*event1.value().info.nodeName_ref() == nodeName1);

    auto event2 = node3->waitForEvent(NB_RESTARTING);
    ASSERT_TRUE(event2.has_value());
    EXPECT_TRUE(*event2.value().info.nodeName_ref() == nodeName1);

    // eventually will lose adjacency as node1 never come back
    EXPECT_TRUE(node2->waitForEvent(NB_DOWN).has_value());
    EXPECT_TRUE(node3->waitForEvent(NB_DOWN).has_value());
  }
}

TEST_F(SparkFixture, FastInitTest) {
  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // By default, helloMsg is sent out every "kFastInitHelloTime" interval
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";

  auto tConfig1 = getBasicOpenrConfig(nodeName1, kDomainName);
  auto config1 = std::make_shared<Config>(tConfig1);

  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName);
  auto config2 = std::make_shared<Config>(tConfig2);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  {
    // start tracking interfaces
    node1->updateInterfaceDb({InterfaceInfo(
        iface1 /* ifName */,
        true /* isUp */,
        ifIndex1 /* ifIndex */,
        {ip1V4, ip1V6} /* networks */)});
    node2->updateInterfaceDb({InterfaceInfo(
        iface2 /* ifName */,
        true /* isUp */,
        ifIndex2 /* ifIndex */,
        {ip2V4, ip2V6} /* networks */)});

    // record current timestamp
    const auto startTime = std::chrono::steady_clock::now();

    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());

    // make sure total time used is limited
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime);
    EXPECT_GE(
        5 * *config1->getSparkConfig().fastinit_hello_time_ms_ref(),
        duration.count());
  }

  // kill and restart node-2
  LOG(INFO) << "Killing and restarting: " << nodeName2;

  node2.reset();
  node2 = createSpark(nodeName2, config2);

  {
    // start tracking interfaces
    node2->updateInterfaceDb({InterfaceInfo(
        iface2 /* ifName */,
        true /* isUp */,
        ifIndex2 /* ifIndex */,
        {ip2V4, ip2V6} /* networks */)});

    // record current timestamp
    const auto startTime = std::chrono::steady_clock::now();

    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());

    // make sure total time used is limited
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime);
    EXPECT_GE(
        5 * *config2->getSparkConfig().fastinit_hello_time_ms_ref(),
        duration.count());
  }
}

//
// Start 2 Spark instances and make sure they form adjacency. Then
// start another Spark instance connecting over the same interface,
// make sure node-1/2 can form adj with node-3 and vice versa.
// Shut down node-3 and make sure adjacency between node-1 and node-2
// is NOT affected.
//
TEST_F(SparkFixture, MultiplePeersOverSameInterface) {
  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex(
      {{iface1, ifIndex1}, {iface2, ifIndex2}, {iface3, ifIndex3}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}, {iface3, 10}}},
      {iface2, {{iface1, 10}, {iface3, 10}}},
      {iface3, {{iface1, 10}, {iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // start spark2 instances
  const std::string nodeName1 = "node-1";
  const std::string nodeName2 = "node-2";
  const std::string nodeName3 = "node-3";

  auto tConfig1 = getBasicOpenrConfig(
      nodeName1,
      kDomainName,
      {},
      true /* enable v4 */,
      true /* enable segment routing */,
      true /* dryrun */,
      false /* enable v4 over v6 nh */,
      true /* enable adj labels */);
  auto config1 = std::make_shared<Config>(tConfig1);

  auto tConfig2 = getBasicOpenrConfig(
      nodeName2,
      kDomainName,
      {},
      true /* enable v4 */,
      true /* enable segment routing */,
      true /* dryrun */,
      false /* enable v4 over v6 nh */,
      true /* enable adj labels */);
  auto config2 = std::make_shared<Config>(tConfig2);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  // start tracking interfaces
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  {
    EXPECT_TRUE(node1->waitForEvent(NB_UP).has_value());
    EXPECT_TRUE(node2->waitForEvent(NB_UP).has_value());
  }

  // add third instance
  LOG(INFO) << "Creating and starting " << nodeName3;

  auto tConfig3 = getBasicOpenrConfig(
      nodeName3,
      kDomainName,
      {},
      true /* enable v4 */,
      true /* enable segment routing */,
      true /* dryrun */,
      false /* enable v4 over v6 nh */,
      true /* enable adj labels */);

  auto config3 = std::make_shared<Config>(tConfig3);

  auto node3 = createSpark(nodeName3, config3);
  node3->updateInterfaceDb({InterfaceInfo(
      iface3 /* ifName */,
      true /* isUp */,
      ifIndex3 /* ifIndex */,
      {ip3V4, ip3V6} /* networks */)});

  // node-1 and node-2 should hear from node-3
  {
    auto event1 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(iface1, *event1->info.localIfName_ref());
    EXPECT_EQ(nodeName3, *event1->info.nodeName_ref());
    // ifIndex already used for assigning label to node-2 via iface1. So next
    // label will be assigned from the end.
    EXPECT_EQ(MplsConstants::kSrLocalRange.second, *event1->info.label_ref());
    LOG(INFO) << nodeName1 << " reported adjacency to " << nodeName3;

    auto event2 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(iface2, *event2->info.localIfName_ref());
    EXPECT_EQ(nodeName3, *event2->info.nodeName_ref());
    // ifIndex already used for assigning label to node-1 via iface2. So next
    // label will be assigned from the end.
    EXPECT_EQ(MplsConstants::kSrLocalRange.second, *event2->info.label_ref());
    LOG(INFO) << nodeName2 << " reported adjacency to " << nodeName3;
  }

  // node-3 should hear from node-1 and node-2 on iface3
  {
    std::map<std::string, NeighborEvent> events;
    for (int i = 0; i < 2; i++) {
      auto maybeEvent = node3->waitForEvent(NB_UP);
      EXPECT_TRUE(maybeEvent.has_value());
      events.emplace(
          *maybeEvent.value().info.nodeName_ref(), maybeEvent.value());
    }

    std::set<int32_t> expectedLabels = {
        MplsConstants::kSrLocalRange.first + ifIndex3,
        MplsConstants::kSrLocalRange.second,
    };

    ASSERT_EQ(1, events.count(nodeName1));
    ASSERT_EQ(1, events.count(nodeName2));

    auto event1 = events.at(nodeName1);
    EXPECT_EQ(iface3, *event1.info.localIfName_ref());
    EXPECT_TRUE(nodeName1 == *event1.info.nodeName_ref());
    EXPECT_EQ(
        std::make_pair(ip1V4.first, ip1V6.first),
        SparkWrapper::getTransportAddrs(event1));
    ASSERT_TRUE(expectedLabels.count(*event1.info.label_ref()));

    auto event2 = events.at(nodeName2);
    EXPECT_EQ(iface3, *event2.info.localIfName_ref());
    EXPECT_TRUE(nodeName2 == *event2.info.nodeName_ref());
    EXPECT_EQ(
        std::make_pair(ip2V4.first, ip2V6.first),
        SparkWrapper::getTransportAddrs(event2));
    ASSERT_TRUE(expectedLabels.count(*event2.info.label_ref()));

    // Label of discovered neighbors must be different on the same interface
    EXPECT_NE(*event1.info.label_ref(), *event2.info.label_ref());

    LOG(INFO) << "node-3 reported adjacencies to node-1, node-2";
  }

  // Now stop spark3
  LOG(INFO) << "Stopping " << nodeName3 << " now...";
  node3.reset();

  // node-1 and node-2 should report node-3 down
  {
    auto event1 = node1->waitForEvent(NB_DOWN);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ("node-3", *event1->info.nodeName_ref());
    LOG(INFO) << nodeName1 << " reported down adjacency towards " << nodeName3;

    auto event2 = node2->waitForEvent(NB_DOWN);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ("node-3", *event2->info.nodeName_ref());
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
TEST_F(SparkFixture, IgnoreUnidirectionalPeer) {
  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // start spark2 instances
  auto tConfig1 = getBasicOpenrConfig("node-1", kDomainName);
  auto config1 = std::make_shared<Config>(tConfig1);

  auto tConfig2 = getBasicOpenrConfig("node-2", kDomainName);
  auto config2 = std::make_shared<Config>(tConfig2);

  auto node1 = createSpark("node-1", config1);
  auto node2 = createSpark("node-2", config2);

  auto waitTime = std::chrono::seconds(
      *config1->getSparkConfig().graceful_restart_time_s_ref());

  // start tracking interfaces
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  {
    EXPECT_FALSE(node1->recvNeighborEvent(waitTime * 2).has_value());
    LOG(INFO) << "node-1 doesn't have any neighbor event";

    EXPECT_FALSE(node2->recvNeighborEvent(waitTime * 2).has_value());
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
TEST_F(SparkFixture, LoopedHelloPktTest) {
  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}});

  // connect iface1 directly with itself to mimick
  // self-looped helloPkt
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface1, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // start one spark2 instance

  auto tConfig1 = getBasicOpenrConfig("node-1", kDomainName);
  auto config1 = std::make_shared<Config>(tConfig1);
  auto node1 = createSpark("node-1", config1);

  // start tracking iface1.
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});

  // should NOT receive any event( e.g.NEIGHBOR_DOWN)
  {
    const auto& graceful_restart_time_s1 = std::chrono::seconds(
        *config1->getSparkConfig().graceful_restart_time_s_ref());
    EXPECT_FALSE(
        node1
            ->waitForEvent(
                NB_DOWN, graceful_restart_time_s1, graceful_restart_time_s1 * 2)
            .has_value());
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
TEST_F(SparkFixture, LinkDownWithoutAdjFormed) {
  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // start spark2 instances
  auto tConfig1 = getBasicOpenrConfig("node-1", kDomainName);
  auto config1 = std::make_shared<Config>(tConfig1);

  auto tConfig2 = getBasicOpenrConfig("node-2", kDomainName);
  auto config2 = std::make_shared<Config>(tConfig2);

  auto node1 = createSpark("node-1", config1);
  auto node2 = createSpark("node-2", config2);

  // enable v4 subnet validation to put adddres in different /31 subnet
  // on purpose.
  const folly::CIDRNetwork ip1V4WithSubnet =
      folly::IPAddress::createNetwork("192.168.0.2", 31);
  const folly::CIDRNetwork ip2V4WithSameSubnet =
      folly::IPAddress::createNetwork("192.168.0.3", 31);
  const folly::CIDRNetwork ip2V4WithDiffSubnet =
      folly::IPAddress::createNetwork("192.168.0.4", 31);

  // start tracking interfaces
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4WithSubnet, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4WithDiffSubnet, ip2V6} /* networks */)});

  // won't form adj as v4 validation should fail
  {
    const auto& graceful_restart_time_s1 = std::chrono::seconds(
        *config1->getSparkConfig().graceful_restart_time_s_ref());
    const auto& graceful_restart_time_s2 = std::chrono::seconds(
        *config2->getSparkConfig().graceful_restart_time_s_ref());
    EXPECT_FALSE(
        node1
            ->waitForEvent(
                NB_UP, graceful_restart_time_s1, graceful_restart_time_s1 * 2)
            .has_value());

    EXPECT_FALSE(
        node2
            ->waitForEvent(
                NB_UP, graceful_restart_time_s2, graceful_restart_time_s2 * 2)
            .has_value());
  }

  {
    // bring down interface of node1 to make sure no crash happened
    node1->updateInterfaceDb({});

    // bring up interface of node1 to make sure no crash happened
    node1->updateInterfaceDb({InterfaceInfo(
        iface1 /* ifName */,
        true /* isUp */,
        ifIndex1 /* ifIndex */,
        {ip1V4WithSubnet, ip1V6} /* networks */)});
  }

  {
    // bring up interface with SAME subnet and verify ADJ UP event
    node2->updateInterfaceDb({InterfaceInfo(
        iface2 /* ifName */,
        true /* isUp */,
        ifIndex2 /* ifIndex */,
        {ip2V4WithSameSubnet, ip2V6} /* networks */)});

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
TEST_F(SparkFixture, InvalidV4Subnet) {
  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // start spark2 instances
  std::string nodeName1 = "node-1";
  std::string nodeName2 = "node-2";

  auto tConfig1 = getBasicOpenrConfig(nodeName1, kDomainName);
  auto config1 = std::make_shared<Config>(tConfig1);

  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName);
  auto config2 = std::make_shared<Config>(tConfig2);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  // enable v4 subnet validation to put adddres in different /31 subnet
  // on purpose.
  const folly::CIDRNetwork ip1V4WithSubnet =
      folly::IPAddress::createNetwork("192.168.0.2", 31);
  const folly::CIDRNetwork ip2V4WithDiffSubnet =
      folly::IPAddress::createNetwork("192.168.0.4", 31);

  // start tracking iface1 and iface2
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4WithSubnet, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4WithDiffSubnet, ip2V6} /* networks */)});

  // won't form adj as v4 validation should fail
  {
    const auto& graceful_restart_time_s1 = std::chrono::seconds(
        *config1->getSparkConfig().graceful_restart_time_s_ref());

    const auto& graceful_restart_time_s2 = std::chrono::seconds(
        *config2->getSparkConfig().graceful_restart_time_s_ref());

    EXPECT_FALSE(
        node1
            ->waitForEvent(
                NB_UP, graceful_restart_time_s1, graceful_restart_time_s1 * 2)
            .has_value());

    EXPECT_FALSE(
        node2
            ->waitForEvent(
                NB_DOWN, graceful_restart_time_s2, graceful_restart_time_s2 * 2)
            .has_value());
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
TEST_F(SparkFixture, AreaMatch) {
  // Explicitly set regex to be capital letters to make sure
  // regex is NOT case-sensative
  auto areaConfig11 = createAreaConfig(area1, {"RSW.*"}, {".*"});
  auto areaConfig12 = createAreaConfig(area2, {"FSW.*"}, {".*"});
  auto areaConfig21 = createAreaConfig(area1, {"FSW.*"}, {".*"});
  auto areaConfig22 = createAreaConfig(area2, {"RSW.*"}, {".*"});
  // overlaps with area2 config. node2 should choose aree2 config as it is lower
  // alphabetically
  auto areaConfig23 = createAreaConfig(area3, {"RSW.*"}, {".*"});

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";

  // RSW: { 1 -> "RSW.*", 2 -> "FSW.*"}
  // FSW: { 1 -> "FSW.*", 2 -> "RSW.*"}

  std::vector<openr::thrift::AreaConfig> vec1 = {areaConfig11, areaConfig12};
  std::vector<openr::thrift::AreaConfig> vec2 = {
      areaConfig21, areaConfig22, areaConfig23};

  auto tConfig1 = getBasicOpenrConfig(nodeName1, kDomainName, vec1);
  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName, vec2);

  auto config1 = std::make_shared<Config>(tConfig1);
  auto config2 = std::make_shared<Config>(tConfig2);

  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking iface1 and iface2
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  // RSW001 and FSW002 node should form adj in area 2 due to regex matching
  {
    auto event1 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(*event1->info.nodeName_ref(), nodeName2);
    EXPECT_EQ(*event1->info.area_ref(), area2);

    auto event2 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(*event2->info.nodeName_ref(), nodeName1);
    EXPECT_EQ(*event2->info.area_ref(), area2);

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
TEST_F(SparkFixture, NoAreaMatch) {
  // AreaConfig:
  //  rsw001: { 1 -> "RSW.*"}
  //  fsw002: { 1 -> "FSW.*"}
  //
  //  rsw001 and fsw002 will receive each other's helloMsg, but won't proceed.
  //  rsw001 can ONLY pair with "RSW.*", whereas fsw002 can ONLY pair with
  //  "FSW.*".
  auto areaConfig1 = createAreaConfig(area1, {"RSW.*"}, {".*"});
  auto areaConfig2 = createAreaConfig(area1, {"FSW.*"}, {".*"});

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  std::vector<openr::thrift::AreaConfig> vec1 = {areaConfig1};
  std::vector<openr::thrift::AreaConfig> vec2 = {areaConfig2};

  auto tConfig1 = getBasicOpenrConfig(nodeName1, kDomainName, vec1);
  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName, vec2);

  auto config1 = std::make_shared<Config>(tConfig1);
  auto config2 = std::make_shared<Config>(tConfig2);

  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking iface1 and iface2
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  {
    const auto& graceful_restart_time_s1 = std::chrono::seconds(
        *config1->getSparkConfig().graceful_restart_time_s_ref());
    const auto& graceful_restart_time_s2 = std::chrono::seconds(
        *config2->getSparkConfig().graceful_restart_time_s_ref());

    EXPECT_FALSE(
        node1
            ->waitForEvent(
                NB_UP, graceful_restart_time_s1, graceful_restart_time_s1 * 2)
            .has_value());
    EXPECT_FALSE(
        node2
            ->waitForEvent(
                NB_UP, graceful_restart_time_s2, graceful_restart_time_s2 * 2)
            .has_value());
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
TEST_F(SparkFixture, InconsistentAreaNegotiation) {
  // AreaConfig:
  //  rsw001: { 1 -> "FSW.*"}
  //  fsw002: { 2 -> "RSW.*"}
  //
  //  rsw001 and fsw002 will receive each other's helloMsg and proceed to
  //  NEGOTIATE stage. However, rsw001 thinks fsw002 should reside in
  //  area "1", whereas fsw002 thinks rsw001 should be in area "2".
  //
  //  AREA negotiation won't go through. Will fall back to WARM
  auto areaConfig1 = createAreaConfig(area1, {"FSW.*"}, {".*"});
  auto areaConfig2 = createAreaConfig(area2, {"RSW.*"}, {".*"});

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";

  std::vector<openr::thrift::AreaConfig> vec1 = {areaConfig1};
  std::vector<openr::thrift::AreaConfig> vec2 = {areaConfig2};

  auto tConfig1 = getBasicOpenrConfig(nodeName1, kDomainName, vec1);
  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName, vec2);

  auto config1 = std::make_shared<Config>(tConfig1);
  auto config2 = std::make_shared<Config>(tConfig2);

  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking iface1 and iface2
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  {
    const auto& graceful_restart_time_s1 = std::chrono::seconds(
        *config1->getSparkConfig().graceful_restart_time_s_ref());
    const auto& graceful_restart_time_s2 = std::chrono::seconds(
        *config2->getSparkConfig().graceful_restart_time_s_ref());

    EXPECT_FALSE(
        node1
            ->waitForEvent(
                NB_UP, graceful_restart_time_s1, graceful_restart_time_s1 * 2)
            .has_value());
    EXPECT_FALSE(
        node2
            ->waitForEvent(
                NB_UP, graceful_restart_time_s2, graceful_restart_time_s2 * 2)
            .has_value());

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
TEST_F(SparkFixture, NoAreaSupportNegotiation) {
  // AreaConfig:
  //  rsw001: {}
  //  fsw002: { 2 -> "RSW.*"}
  //
  //  rsw001 doesn't know anything about AREA, whereas fsw002 is configured
  //  with areaConfig. Make sure AREA negotiation will go through
  //  rsw001 form adj inside `defaultArea`.
  //  fsw002 form adj inside `2`
  auto areaConfig2 = createAreaConfig(area2, {"RSW.*"}, {".*"});

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  std::vector<openr::thrift::AreaConfig> vec2 = {areaConfig2};

  auto tConfig1 = getBasicOpenrConfig(
      nodeName1,
      kDomainName,
      {createAreaConfig(
          thrift::Types_constants::kDefaultArea(), {".*"}, {".*"})});
  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName, vec2);

  auto config1 = std::make_shared<Config>(tConfig1);
  auto config2 = std::make_shared<Config>(tConfig2);

  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking iface1 and iface2
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  {
    auto event1 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(*event1->info.nodeName_ref(), nodeName2);
    EXPECT_EQ(*event1->info.area_ref(), defaultArea);

    auto event2 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(*event2->info.nodeName_ref(), nodeName1);
    EXPECT_EQ(*event2->info.area_ref(), area2);
  }
}

//
// Start 2 Spark with AREA config supported and make sure they can
// form adj. Then add another Spark. Make sure 3rd Spark instance
// can form adj with different peers within different area over the
// same interface.
//
TEST_F(SparkFixture, MultiplePeersWithDiffAreaOverSameLink) {
  // AreaConfig:
  //  rsw001: { 1 -> {"FSW.*"}, 2 -> {"SSW.*"}}
  //  fsw002: { 1 -> {"RSW.*", "SSW.*"}}
  //  ssw003: { 1 -> {"FSW.*"}, 2 -> {"RSW.*"}}
  //
  //  Based on topology setup, expected adj pairs:
  //    rsw001 <==> fsw002
  //    fsw002 <==> ssw003
  //    ssw003 <==> rsw001
  auto areaConfig11 = createAreaConfig(area1, {"FSW.*"}, {".*"});
  auto areaConfig12 = createAreaConfig(area2, {"SSW.*"}, {".*"});
  auto areaConfig2 = createAreaConfig(area1, {"RSW.*", "SSW.*"}, {".*"});
  auto areaConfig31 = createAreaConfig(area1, {"fsw.*"}, {".*"});
  auto areaConfig32 = createAreaConfig(area2, {"rsw.*"}, {".*"});

  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  std::string nodeName3 = "ssw003";

  std::vector<openr::thrift::AreaConfig> vec1 = {areaConfig11, areaConfig12};
  std::vector<openr::thrift::AreaConfig> vec2 = {areaConfig2};
  std::vector<openr::thrift::AreaConfig> vec3 = {areaConfig31, areaConfig32};

  auto tConfig1 = getBasicOpenrConfig(nodeName1, kDomainName, vec1);
  auto tConfig2 = getBasicOpenrConfig(nodeName2, kDomainName, vec2);

  auto tConfig3 = getBasicOpenrConfig(nodeName3, kDomainName, vec3);

  auto config1 = std::make_shared<Config>(tConfig1);
  auto config2 = std::make_shared<Config>(tConfig2);
  auto config3 = std::make_shared<Config>(tConfig3);

  // Define interface names for the test
  mockIoProvider_->addIfNameIfIndex(
      {{iface1, ifIndex1}, {iface2, ifIndex2}, {iface3, ifIndex3}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}, {iface3, 10}}},
      {iface2, {{iface1, 10}, {iface3, 10}}},
      {iface3, {{iface1, 10}, {iface2, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  auto node1 = createSpark(nodeName1, config1);
  auto node2 = createSpark(nodeName2, config2);

  LOG(INFO) << nodeName1 << " and " << nodeName2 << " started...";

  // start tracking interfaces
  node1->updateInterfaceDb({InterfaceInfo(
      iface1 /* ifName */,
      true /* isUp */,
      ifIndex1 /* ifIndex */,
      {ip1V4, ip1V6} /* networks */)});
  node2->updateInterfaceDb({InterfaceInfo(
      iface2 /* ifName */,
      true /* isUp */,
      ifIndex2 /* ifIndex */,
      {ip2V4, ip2V6} /* networks */)});

  {
    auto event1 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(iface1, *event1->info.localIfName_ref());
    EXPECT_EQ(nodeName2, *event1->info.nodeName_ref());
    EXPECT_EQ(*event1->info.area_ref(), area1);
    LOG(INFO) << nodeName1 << " reported adjacency to " << nodeName2;

    auto event2 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(iface2, *event2->info.localIfName_ref());
    EXPECT_EQ(nodeName1, *event2->info.nodeName_ref());
    EXPECT_EQ(*event1->info.area_ref(), area1);
    LOG(INFO) << nodeName2 << " reported adjacency to " << nodeName1;
  }

  // add third instance

  auto node3 = createSpark(nodeName3, config3);
  node3->updateInterfaceDb({InterfaceInfo(
      iface3 /* ifName */,
      true /* isUp */,
      ifIndex3 /* ifIndex */,
      {ip3V4, ip3V6} /* networks */)});

  LOG(INFO) << nodeName3 << " being started...";

  // rsw001 and fsw002 should form adj with ssw003 in area2, area1 respectively
  {
    auto event1 = node2->waitForEvent(NB_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(iface2, *event1.value().info.localIfName_ref());
    EXPECT_EQ(nodeName3, *event1.value().info.nodeName_ref());
    EXPECT_EQ(*event1.value().info.area_ref(), area1);
    LOG(INFO) << nodeName2 << " reported adjacency to " << nodeName3;

    auto event2 = node1->waitForEvent(NB_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(iface1, *event2.value().info.localIfName_ref());
    EXPECT_EQ(nodeName3, *event2.value().info.nodeName_ref());
    EXPECT_EQ(*event2.value().info.area_ref(), area2);
    LOG(INFO) << nodeName1 << " reported adjacency to " << nodeName3;
  }

  // ssw003 should hear from rsw001 and fsw002 on iface3 in DIFF area
  {
    std::map<std::string, NeighborEvent> events;
    for (int i = 0; i < 2; i++) {
      auto maybeEvent = node3->waitForEvent(NB_UP);
      EXPECT_TRUE(maybeEvent.has_value());
      events.emplace(
          *maybeEvent.value().info.nodeName_ref(), maybeEvent.value());
    }

    auto& info1 = events.at(nodeName1).info;
    EXPECT_EQ(iface3, *info1.localIfName_ref());
    EXPECT_EQ(nodeName1, *info1.nodeName_ref());
    EXPECT_EQ(area2, *info1.area_ref());
    LOG(INFO) << nodeName3 << " reported adjacency to " << nodeName1;

    auto& info2 = events.at(nodeName2).info;
    EXPECT_EQ(iface3, *info2.localIfName_ref());
    EXPECT_TRUE(nodeName2 == *info2.nodeName_ref());
    EXPECT_EQ(area1, *info2.area_ref());
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
