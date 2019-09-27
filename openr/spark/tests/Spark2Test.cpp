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

const int ifIndex1{1};
const int ifIndex2{2};

const folly::CIDRNetwork ip1V4 =
    folly::IPAddress::createNetwork("192.168.0.1", 24, false /* apply mask */);
const folly::CIDRNetwork ip2V4 =
    folly::IPAddress::createNetwork("192.168.0.2", 24, false /* apply mask */);

const folly::CIDRNetwork ip1V6 = folly::IPAddress::createNetwork("fe80::1/128");
const folly::CIDRNetwork ip2V6 = folly::IPAddress::createNetwork("fe80::2/128");

// Domain name (same for all Tests except in DomainTest)
const std::string kDomainName("Fire_and_Blood");

// the URL for the spark server
const std::string kSparkReportUrl("inproc://spark_server_report");

// the URL for the spark server
const std::string kSparkCounterCmdUrl("inproc://spark_server_counter_cmd");

// the hold time we use during the tests
const std::chrono::milliseconds kHoldTime(500);

// the keep-alive for spark2 hello messages
const std::chrono::milliseconds kKeepAliveTime(50);

// the time interval for spark2 handhshake msg
const std::chrono::milliseconds kHandshakeTime(50);

// the time interval for spark2 handhshake msg
const std::chrono::milliseconds kHeartbeatTime(50);

// the hold time for spark2 negotiate stage
const std::chrono::milliseconds kNegotiateHoldTime(500);

// the hold time for spark2 heartbeat msg
const std::chrono::milliseconds kHeartbeatHoldTime(500);
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
  createSpark2(
      std::string const& domainName,
      std::string const& myNodeName,
      uint32_t spark2Id,
      std::chrono::milliseconds holdTime = kHoldTime,
      std::chrono::milliseconds keepAliveTime = kKeepAliveTime,
      std::chrono::milliseconds fastInitKeepAliveTime = kKeepAliveTime,
      std::pair<uint32_t, uint32_t> version = std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
      std::chrono::milliseconds myHandshakeTime = kHandshakeTime,
      std::chrono::milliseconds myHeartbeatTime = kHeartbeatTime,
      std::chrono::milliseconds myNegotiateHoldTime = kNegotiateHoldTime,
      std::chrono::milliseconds myHeartbeatHoldTime = kHeartbeatHoldTime) {
    return std::make_unique<SparkWrapper>(
        domainName,
        myNodeName,
        holdTime,
        keepAliveTime,
        fastInitKeepAliveTime,
        true,
        true,
        SparkReportUrl{folly::sformat("{}-{}", kSparkReportUrl, spark2Id)},
        MonitorSubmitUrl{
            folly::sformat("{}-{}", kSparkCounterCmdUrl, spark2Id)},
        version,
        context,
        mockIoProvider,
        folly::none, // no area support yet
        true, // Spark2 enabled
        myHandshakeTime,
        myHeartbeatTime,
        myNegotiateHoldTime,
        myHeartbeatHoldTime);
  }

  fbzmq::Context context;
  std::shared_ptr<MockIoProvider> mockIoProvider{nullptr};
  std::unique_ptr<std::thread> mockIoProviderThread{nullptr};
  CompactSerializer serializer_;
};

TEST_F(Spark2Fixture, UnidirectionTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fxiture UnidirectionTest finished";
  };

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start one spark2 instance
  auto node1 = createSpark2(kDomainName, "node-1", 1);

  // start another spark2 instance
  auto node2 = createSpark2(kDomainName, "node-2", 2);

  // start tracking iface1
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));

  // start tracking iface2
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  LOG(INFO) << "Start to receive messages from Spark2";

  // Now wait for sparks to detect each other
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(iface1, event->ifName);
    EXPECT_EQ("node-2", event->neighbor.nodeName);
    EXPECT_EQ(
        std::make_pair(ip2V4.first, ip2V6.first),
        SparkWrapper::getTransportAddrs(*event));
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(iface2, event->ifName);
    EXPECT_EQ("node-1", event->neighbor.nodeName);
    EXPECT_EQ(
        std::make_pair(ip1V4.first, ip1V6.first),
        SparkWrapper::getTransportAddrs(*event));
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  LOG(INFO) << "Stopping communications from iface2 to iface1";

  // stop packet flowing iface2 -> iface1. Expect both ends drops
  //  1. node1 drops due to: heartbeat hold timer expired
  //  2. node2 drops due to: helloMsg doesn't contains neighborInfo
  connectedPairs = {
      {iface1, {{iface2, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // wait for sparks to lose each other
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported down adjacency to node-2";
  }

  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported down adjacency to node-1";
  }
}

TEST_F(Spark2Fixture, GRTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture GracefulRestartTest finished";
  };

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start one spark2 instance
  auto node1 = createSpark2(
      kDomainName,
      "node-1",
      1,
      std::chrono::milliseconds(1000),
      std::chrono::milliseconds(200));

  // start another spark2 instance
  auto node2 = createSpark2(
      kDomainName,
      "node-2",
      2,
      std::chrono::milliseconds(1000),
      std::chrono::milliseconds(200));

  // start tracking iface1
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));

  // start tracking iface2
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  LOG(INFO) << "Start to receive messages from Spark2";

  // Now wait for sparks to detect each other
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  // Kill node2
  LOG(INFO) << "Kill and restart node-2";

  node2.reset();

  // node-1 should report node-2 as 'RESTARTING' when it received GRMsg from
  // node-2
  {
    auto event = node1->waitForEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported node-2 as RESTARTING";
  }

  node2 = createSpark2(
      kDomainName,
      "node-2",
      3, /* spark2Id change */
      std::chrono::milliseconds(1000), /* GR hold time */
      std::chrono::milliseconds(200) /* helloMsg interval */);

  LOG(INFO) << "Adding iface2 to node-2 to let it start helloMsg adverstising";

  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  // node-1 should report node-2 as 'RESTARTED' when it receive helloMsg
  // with wrapped seqNum
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED);
    ASSERT_TRUE(event.hasValue());
  }

  // node-2 should ultimately report node-1 as 'UP'
  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
  }

  // should NOT receive any event( e.g.NEIGHBOR_DOWN)
  {
    EXPECT_TRUE(node1->recvNeighborEvent(kHoldTime * 2).hasError());
    EXPECT_TRUE(node2->recvNeighborEvent(kHoldTime * 2).hasError());
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
