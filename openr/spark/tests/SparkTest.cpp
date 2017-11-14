/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockIoProvider.h"

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

#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/spark/IoProvider.h>
#include <openr/spark/SparkWrapper.h>

DEFINE_bool(stress_test, false, "pass this to run the stress test");

using namespace std;
using namespace openr;

using apache::thrift::CompactSerializer;

namespace {
const string iface1{"iface1"};
const string iface2{"iface2"};
const string iface3{"iface3"};

const int ifIndex1{1};
const int ifIndex2{2};
const int ifIndex3{3};

const folly::IPAddress ip1V4{"192.168.0.1"};
const folly::IPAddress ip2V4{"192.168.0.2"};
const folly::IPAddress ip3V4{"192.168.0.3"};

const folly::IPAddress ip1V6{"fe80::1"};
const folly::IPAddress ip2V6{"fe80::2"};
const folly::IPAddress ip3V6{"fe80::3"};

// Domain name (same for all Tests except in DomainTest)
const std::string kDomainName("terragraph");

// the URL for the spark server
const std::string kSparkReportUrl("inproc://spark_server_report");

// the URL for the spark server
const std::string kSparkCmdUrl("inproc://spark_server_cmd");

// the URL for the spark server
const std::string kSparkCounterCmdUrl("inproc://spark_server_counter_cmd");

// the hold time we use during the tests
const std::chrono::milliseconds kHoldTime(100);

// the keep-alive for spark hello messages
const std::chrono::milliseconds kKeepAliveTime(20);

// produce a random string of given length - for value generation
std::string
genRandomStr(const int len) {
  std::string s;
  s.resize(len);

  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

  for (int i = 0; i < len; ++i) {
    s[i] = alphanum[folly::Random::rand32() % (sizeof(alphanum) - 1)];
  }

  return s;
}

//
// Lame-ass attempt to skip unexpected messages, such as RTT
// change event. Trying 3 times is a wild guess, no logic.
//
folly::Optional<thrift::SparkNeighborEvent>
waitForEvent(
    std::shared_ptr<SparkWrapper> const spark,
    const thrift::SparkNeighborEventType eventType) noexcept {
  // XXX: hardcode_it
  for (auto i = 0; i < 3; i++) {
    auto maybeEvent = spark->recvNeighborEvent();
    if (maybeEvent.hasError()) {
      LOG(ERROR) << "recvNeighborEvent failed: " << maybeEvent.error();
      continue;
    }
    auto event = maybeEvent.value();
    if (eventType == event.eventType) {
      return event;
    }
  }
  return folly::none;
};

} // namespace

//
// This fixture has common variables
//
class SparkFixture : public testing::Test {
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

  // extract IPs from a spark neighbor event
  pair<folly::IPAddress, folly::IPAddress>
  getTransportAddrs(const thrift::SparkNeighborEvent& event) {
    return {toIPAddress(event.neighbor.transportAddressV4),
            toIPAddress(event.neighbor.transportAddressV6)};
  }

  // helper function to create a spark instance
  shared_ptr<SparkWrapper>
  createSpark(
      std::string const& domainName,
      std::string const& myNodeName,
      KeyPair keyPair,
      KnownKeysStore* knownKeysStore,
      int sparkNum,
      std::chrono::milliseconds holdTime = kHoldTime,
      std::chrono::milliseconds keepAliveTime = kKeepAliveTime,
      std::chrono::milliseconds fastInitKeepAliveTime = kKeepAliveTime) {
    return std::make_unique<SparkWrapper>(
        domainName,
        myNodeName,
        holdTime /* myHoldTime */,
        keepAliveTime /* myKeepAliveTime */,
        fastInitKeepAliveTime,
        keyPair,
        knownKeysStore,
        true /* enable v4 */,
        true /* enable packet signature */,
        SparkReportUrl{folly::sformat("{}-{}", kSparkReportUrl, sparkNum)},
        SparkCmdUrl{folly::sformat("{}-{}", kSparkCmdUrl, sparkNum)},
        MonitorSubmitUrl{
            folly::sformat("{}-{}", kSparkCounterCmdUrl, sparkNum)},
        context,
        mockIoProvider);
  }

  fbzmq::Context context;

  shared_ptr<MockIoProvider> mockIoProvider{nullptr};

  unique_ptr<std::thread> mockIoProviderThread{nullptr};

  CompactSerializer serializer_;

  KeyPair keyPair1 = fbzmq::util::genKeyPair();
  KeyPair keyPair2 = fbzmq::util::genKeyPair();
  KeyPair keyPair3 = fbzmq::util::genKeyPair();
};

//
// Validate the signing functions
//
TEST(SparkTest, SignatureTest) {
  auto text = genRandomStr(1024);
  auto keyPair = fbzmq::util::genKeyPair();

  auto sig = Spark::signMessage(text, keyPair.privateKey);
  EXPECT_TRUE(Spark::validateSignature(sig, text, keyPair.publicKey));
}

//
// Start two sparks, let them form adjacency, then make it unidirectional
// We expect both to lose the adjacency after a timeout, even though
// packets are flowing one-way
//
TEST_F(SparkFixture, UnidirectionalTest) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture unidirectional failure test";
  };

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark2
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(iface1, event->ifName);
    EXPECT_EQ("node-2", event->neighbor.nodeName);
    EXPECT_EQ(keyPair2.publicKey, event->neighbor.publicKey);
    EXPECT_EQ(make_pair(ip2V4, ip2V6), getTransportAddrs(*event));
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(iface2, event->ifName);
    EXPECT_EQ("node-1", event->neighbor.nodeName);
    EXPECT_EQ(keyPair1.publicKey, event->neighbor.publicKey);
    EXPECT_EQ(make_pair(ip1V4, ip1V6), getTransportAddrs(*event));
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  //
  // Now make communications uni-directional, stop packets from flowing
  // iface1 -> iface2 direction
  //
  connectedPairs = {
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  LOG(INFO) << "Stopping communications from iface1 to iface2";

  //
  // wait for sparks to lose each other
  //

  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported down adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported down adjacency to node-1";
  }
}

//
// Start two sparks, make sure they form adjacency. Restart
// one of the sparks within the hold-time window, make
// sure we still get the restart event due to wrapping SeqNum
// or key change
//
TEST_F(SparkFixture, GracefulRestart) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture sequence number reset test";
  };

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(
      kDomainName,
      "node-1",
      keyPair1,
      nullptr,
      1,
      std::chrono::milliseconds(1000) /* hold time */,
      std::chrono::milliseconds(200) /* my keep alive time */);

  // start spark2
  auto spark2 = createSpark(
      kDomainName,
      "node-2",
      keyPair2,
      nullptr,
      2,
      std::chrono::milliseconds(1000) /* hold time */,
      std::chrono::milliseconds(200) /* my keep alive time */);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  //
  // Kill and restart spark2
  //

  LOG(INFO) << "Killing and restarting node-2";

  // We have to use different URL to bind/connect here. ZMQ socket close is
  // async operation and `socket->close()` call returns immediately. There are
  // chances that bind-address might still be in use if ZMQ Reaper thread hasn't
  // cleaned it up.
  spark2 = createSpark(
      kDomainName,
      "node-2",
      keyPair2,
      nullptr,
      3 /* changed */,
      std::chrono::milliseconds(1000) /* hold time */,
      std::chrono::milliseconds(200) /* my keep alive time */);

  LOG(INFO) << "Adding iface2 to node-2";

  // re-add interface
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  // node-1 should report node-2 as restarting because of sequence number
  // wrapping
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_RESTART);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported node-2 as RESTARTING";
  }

  //
  // node-2 will eventually report node-1 as up
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }
}

//
// Start two sparks, make sure they form adjacency. Restart
// one of the sparks outside the hold-time window, make sure we get
// peer-down event followed by forming of adjacency.
//
TEST_F(SparkFixture, HoldTimerExpired) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture sequence number reset test";
  };

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark2
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(iface1, event->ifName);
    EXPECT_EQ("node-2", event->neighbor.nodeName);
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(iface2, event->ifName);
    EXPECT_EQ("node-1", event->neighbor.nodeName);
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  //
  // Kill and restart spark2
  //

  LOG(INFO) << "Killing and restarting node-2.";
  spark2 = nullptr;

  LOG(INFO) << "Waiting for hold-timer to get expired on node-1.";
  /* sleep override */
  std::this_thread::sleep_for(kHoldTime * 3 / 2);

  spark2 =
      createSpark(kDomainName, "node-2", keyPair2, nullptr, 3 /* changed */);

  LOG(INFO) << "Adding iface2 to node-2";

  // re-add interface
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  // node-1 should report node-2 as down because of hold timer expired
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported node-2 as down because hold-timer expired";
  }

  // node-1 should report node-2 as up
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  // node-2 will eventually report node-1 as up
  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }
}

//
// Start two peers, but block one from hearing another. Shutdown
// the peer that cannot hear, and make sure there is no DOWN
// event generated for this one
//
TEST_F(SparkFixture, IgnoreUnidirectionalPeer) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture ignore unidirectional peer test";
  };

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark1
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  //
  // Now wait for sparks to NOT report anything
  //

  // wait for another half kHoldTime to account for processing delay
  EXPECT_TRUE(spark1->recvNeighborEvent(kHoldTime * 3 / 2).hasError());

  EXPECT_TRUE(spark2->recvNeighborEvent(kHoldTime * 3 / 2).hasError());

  // stop tracking iface2 in spark2
  // Empty interface list
  EXPECT_TRUE(spark2->updateInterfaceDb({}));

  // node-1 should not report anything
  EXPECT_TRUE(spark1->recvNeighborEvent(kHoldTime * 3 / 2).hasError());
}

//
// Start two sparks, let them form adjacency, then remove/add
// interface on one side
//
TEST_F(SparkFixture, IfaceRemovalTest) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture interface removal test";
  };

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark2
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  LOG(INFO) << "Telling node-1 to remove an iface";

  //
  // Tell spark1 to remove interface
  // Empty interface list
  //
  EXPECT_TRUE(spark1->updateInterfaceDb({}));

  LOG(INFO) << "Waiting for node-1 to report loss of neighbor";

  //
  // First node will immediately report loss of neighbor
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported down adjacency to node-2";
  }

  LOG(INFO) << "Waiting for node-2 to time-out node-1";

  // second node will time out

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported down adjacency to node-1";
  }

  //
  // Now bring iface1 back up and wait for nodes to re-acquire each other
  //

  LOG(INFO) << "Bringing iface1 back up...";

  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  //
  // Wait for UP event from both neighbors
  //

  LOG(INFO) << "Waiting for node-1 and node-2 to report UP again";

  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported UP adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported UP adjacency to node-2";
  }
}

//
// Start two sparks on same network, then add third one. Next, shutdown
// the third one, make sure the two others detect it. Also make sure that
// labels are generate/allocated appropriately
//
TEST_F(SparkFixture, TestAdjUpDownChanges) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture three peers, single network";
  };

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex(
      {{iface1, ifIndex1}, {iface2, ifIndex2}, {iface3, ifIndex3}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}, {iface3, 100}}},
      {iface2, {{iface1, 100}, {iface3, 100}}},
      {iface3, {{iface1, 100}, {iface2, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark2
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  //
  // Add third spark
  //
  LOG(INFO) << "Creating and starting spark-3";

  // start spark3
  auto spark3 = createSpark(kDomainName, "node-3", keyPair3, nullptr, 3);

  // start tracking iface3
  EXPECT_TRUE(spark3->updateInterfaceDb(
      {{iface3, ifIndex3, ip3V4.asV4(), ip3V6.asV6()}}));

  //
  // Spark1 should hear from spark-3
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(iface1, event->ifName);
    EXPECT_EQ("node-3", event->neighbor.nodeName);
    EXPECT_EQ(keyPair3.publicKey, event->neighbor.publicKey);
    // ifIndex already used for assigning label to node-2 via iface1. So next
    // label will be assigned from the end.
    EXPECT_EQ(Constants::kSrLocalRange.second, event->label);
    LOG(INFO) << "node-1 reported adjacency to node-3";
  }

  //
  // Spark2 should hear from spark-3
  //
  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(iface2, event->ifName);
    EXPECT_EQ("node-3", event->neighbor.nodeName);
    EXPECT_EQ(keyPair3.publicKey, event->neighbor.publicKey);
    // ifIndex already used for assigning label to node-1 via iface2. So next
    // label will be assigned from the end.
    EXPECT_EQ(Constants::kSrLocalRange.second, event->label);
    LOG(INFO) << "node-2 reported adjacency to node-3";
  }

  //
  // Spark3 should hear from spark-1 and spark-2 on iface3
  //
  {
    std::map<std::string, thrift::SparkNeighborEvent> events;

    for (int i = 0; i < 2; i++) {
      auto maybeEvent = spark3->recvNeighborEvent();
      EXPECT_TRUE(maybeEvent.hasValue());
      auto event = maybeEvent.value();
      events[event.neighbor.nodeName] = event;
    }

    std::set<int32_t> expectedLabels = {
        Constants::kSrLocalRange.first + ifIndex3,
        Constants::kSrLocalRange.second,
    };

    thrift::SparkNeighborEvent event;

    event = events.at("node-1");
    auto label2 = event.label;

    EXPECT_EQ(thrift::SparkNeighborEventType::NEIGHBOR_UP, event.eventType);
    EXPECT_EQ(iface3, event.ifName);
    EXPECT_EQ("node-1", event.neighbor.nodeName);
    EXPECT_EQ(keyPair1.publicKey, event.neighbor.publicKey);
    EXPECT_EQ(make_pair(ip1V4, ip1V6), getTransportAddrs(event));
    EXPECT_EQ(1, expectedLabels.count(event.label));

    event = events.at("node-2");
    auto label3 = event.label;

    EXPECT_EQ(thrift::SparkNeighborEventType::NEIGHBOR_UP, event.eventType);
    EXPECT_EQ(iface3, event.ifName);
    EXPECT_EQ("node-2", event.neighbor.nodeName);
    EXPECT_EQ(keyPair2.publicKey, event.neighbor.publicKey);
    EXPECT_EQ(make_pair(ip2V4, ip2V6), getTransportAddrs(event));
    EXPECT_EQ(1, expectedLabels.count(event.label));

    // Label of discovered neighbors must be different on the same interface
    EXPECT_NE(label2, label3);

    LOG(INFO) << "node-3 reported adjacencies to node-1, node-2";
  }

  //
  // Now stop spark3
  //
  LOG(INFO) << "Stopping spark-3 now...";
  spark3 = nullptr;

  //
  // Spark1 should report spark-3 down
  //
  {
    LOG(INFO) << "Waiting for node-1 to report down adjacency to node-3";
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ("node-3", event->neighbor.nodeName);
    LOG(INFO) << "node-1 reported down adjacency for node-3";
  }

  //
  // Spark2 should report spark-3 down
  //
  {
    LOG(INFO) << "Waiting for node-2 to report down adjacency to node-3";
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ("node-3", event->neighbor.nodeName);
    LOG(INFO) << "node-2 reported down adjacency for node-3";
  }
}

//
// Start three sparks in hub-and-spoke topology. We prohibit
// node-2 and node-3 to talk to each other. We make node-1
// use two different interfaces for communications.
//
TEST_F(SparkFixture, HubAndSpoke) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture three peers, hub and spoke topology";
  };

  const string iface1_2{"iface1_2"};
  const string iface1_3{"iface1_3"};
  const int ifIndex1_2{12};
  const int ifIndex1_3{13};
  const folly::IPAddress ip1V4_2{"192.168.12.1"};
  const folly::IPAddress ip1V4_3{"192.168.13.1"};
  const folly::IPAddress ip1V6_2{"fe80::12:1"};
  const folly::IPAddress ip1V6_3{"fe80::13:1"};

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1_2, ifIndex1_2},
                                    {iface1_3, ifIndex1_3},
                                    {iface2, ifIndex2},
                                    {iface3, ifIndex3}});

  ConnectedIfPairs connectedPairs = {{iface1_2, {{iface2, 100}}},
                                     {iface1_3, {{iface3, 100}}},
                                     {iface2, {{iface1_2, 100}}},
                                     {iface3, {{iface1_3, 100}}}};
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark2
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  // start spark3
  auto spark3 = createSpark(kDomainName, "node-3", keyPair3, nullptr, 3);

  // tell spark1 to start hello on two interfaces
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1_2, ifIndex1_2, ip1V4_2.asV4(), ip1V6_2.asV6()},
       {iface1_3, ifIndex1_3, ip1V4_3.asV4(), ip1V6_3.asV6()}}));

  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  EXPECT_TRUE(spark3->updateInterfaceDb(
      {{iface3, ifIndex3, ip3V4.asV4(), ip3V6.asV6()}}));

  //
  // node-1 should hear from both node-2 and node-3
  //
  {
    std::map<std::string, thrift::SparkNeighborEvent> events;

    for (int i = 0; i < 2; i++) {
      auto maybeEvent = spark1->recvNeighborEvent();
      EXPECT_TRUE(maybeEvent.hasValue());
      auto event = maybeEvent.value();
      events[event.neighbor.nodeName] = event;
    }

    thrift::SparkNeighborEvent event;

    event = events["node-2"];

    EXPECT_EQ(thrift::SparkNeighborEventType::NEIGHBOR_UP, event.eventType);
    EXPECT_EQ(iface1_2, event.ifName);
    EXPECT_EQ("node-2", event.neighbor.nodeName);
    EXPECT_EQ(keyPair2.publicKey, event.neighbor.publicKey);
    EXPECT_EQ(make_pair(ip2V4, ip2V6), getTransportAddrs(event));

    event = events["node-3"];

    EXPECT_EQ(thrift::SparkNeighborEventType::NEIGHBOR_UP, event.eventType);
    EXPECT_EQ(iface1_3, event.ifName);
    EXPECT_EQ("node-3", event.neighbor.nodeName);
    EXPECT_EQ(keyPair3.publicKey, event.neighbor.publicKey);
    EXPECT_EQ(make_pair(ip3V4, ip3V6), getTransportAddrs(event));

    LOG(INFO) << "node-1 reported adjacencies UP to node-2, node-3";
  }

  LOG(INFO) << "Stopping node-2 and node-3";

  //
  // Stop node-2 and node-3
  //
  spark2 = nullptr;
  spark3 = nullptr;

  //
  // node-1 should lose both node-2 and node-3
  //
  {
    std::map<std::string, thrift::SparkNeighborEvent> events;

    for (int i = 0; i < 2; i++) {
      auto maybeEvent = spark1->recvNeighborEvent();
      EXPECT_TRUE(maybeEvent.hasValue());
      auto event = maybeEvent.value();
      events[event.neighbor.nodeName] = event;
      LOG(INFO) << "Received a message from node-1";
    }

    thrift::SparkNeighborEvent event;

    event = events["node-2"];

    EXPECT_EQ(iface1_2, event.ifName);
    EXPECT_EQ(thrift::SparkNeighborEventType::NEIGHBOR_DOWN, event.eventType);
    EXPECT_EQ("node-2", event.neighbor.nodeName);

    event = events["node-3"];

    EXPECT_EQ(thrift::SparkNeighborEventType::NEIGHBOR_DOWN, event.eventType);
    EXPECT_EQ(iface1_3, event.ifName);
    EXPECT_EQ("node-3", event.neighbor.nodeName);

    LOG(INFO) << "node-1 reported adjacencies DOWN to node-2, node-3";
  }
}

//
// Authenticate by checking if a peer's key is known
//
TEST_F(SparkFixture, KnownKeysAuth) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture ignore unwhitelisted test";
  };

  KnownKeysStore knownKeysStore;

  knownKeysStore.setKeyByName("node-1", keyPair1.publicKey);
  knownKeysStore.setKeyByName("node-2", keyPair2.publicKey);

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(
      kDomainName,
      "node-1",
      keyPair1,
      &knownKeysStore /* known keys auth */,
      1);

  // start spark2
  auto spark2 = createSpark(
      kDomainName,
      "node-2",
      keyPair2,
      &knownKeysStore /* known keys auth */,
      2);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO)
      << "Preparing to receive the messages from sparks since keys are known";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  //
  // case 1): Fail authentication as node-2's key mismatches known one
  //

  // Note: assume private and public keys are different in a key pair
  knownKeysStore.setKeyByName("node-2", keyPair2.privateKey);

  LOG(INFO) << "Preparing to receive the DOWN messages from sparks due to key "
               "mismatch with known keys";

  //
  // node-1 should report node-2 as down, after ignoring its hello packets
  // since the public key does not match known one
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported down adjacency to node-2";
  }

  //
  // case 2): Fail authentication as node-100's key is unknown
  //
  //
  // Kill and restart spark2
  //

  LOG(INFO) << "Killing and restarting node-2 with a different name";

  spark2 =
      createSpark(kDomainName, "node-100", keyPair2, nullptr, 4 /* changed */);

  LOG(INFO) << "Adding iface2 to node-100";

  // re-add interface
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO)
      << "Preparing to NOT receive the messages from sparks due to neighbor "
         "not existent in known keys";

  //
  // Now wait for sparks to NOT report anything
  //
  EXPECT_TRUE(spark1->recvNeighborEvent(kHoldTime * 3 / 2).hasError());

  EXPECT_TRUE(spark2->recvNeighborEvent(kHoldTime * 3 / 2).hasError());
}

//
// Fail authentication when key changes
//
TEST_F(SparkFixture, KeyChangeTest) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture key change test";
  };

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark2
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  LOG(INFO) << "Change node-2's key";

  // stop it first
  spark2->stop();
  spark2->setKeyPair(fbzmq::util::genKeyPair());
  // resume
  spark2->run();

  LOG(INFO) << "Waiting for node-1 to report loss of neighbor";

  //
  // First node will immediately report loss of neighbor
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported down adjacency to node-2";
  }

  //
  // node-1 will use node-2's new key after kicking it out and re-acquire it
  //

  LOG(INFO) << "Waiting for node-1 to report UP again";

  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported UP adjacency to node-2";
  }
}

//
// Start two sparks, let them form adjacency, then increase and decrease RTT and
// see we get NEIGHBOR_RTT_CHANGE event.
//
TEST_F(SparkFixture, RttTest) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture RTT Measurement test";
  };

  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark2
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    // 25% tolerance
    EXPECT_GE(event->rttUs, (200 - 50) * 1000);
    EXPECT_LE(event->rttUs, (200 + 50) * 1000);
    LOG(INFO) << "node-2 reported adjacency to node-1. rtt: "
              << event->rttUs / 1000.0 << "ms.";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    // 25% tolerance
    EXPECT_GE(event->rttUs, (200 - 50) * 1000);
    EXPECT_LE(event->rttUs, (200 + 50) * 1000);
    LOG(INFO) << "node-1 reported adjacency to node-2. rtt: "
              << event->rttUs / 1000.0 << "ms.";
  }

  //
  // Now make change RTT between iface to 220ms (asymmetric)
  //
  connectedPairs = {
      {iface1, {{iface2, 102}}},
      {iface2, {{iface1, 118}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);
  LOG(INFO) << "Changing RTT to 220ms";

  //
  // wait for sparks to detect RTT change. We will get only single update
  //
  {
    auto event = waitForEvent(
        spark1, thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE);
    ASSERT_TRUE(event.hasValue());
    // 25% tolerance
    EXPECT_GE(event->rttUs, (220 - 55) * 1000);
    EXPECT_LE(event->rttUs, (220 + 55) * 1000);
    LOG(INFO) << "node-1 reported new RTT to node-2 to be "
              << event->rttUs / 1000.0 << "ms.";
  }

  {
    auto event = waitForEvent(
        spark2, thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE);
    ASSERT_TRUE(event.hasValue());
    // 25% tolerance
    EXPECT_GE(event->rttUs, (220 - 55) * 1000);
    EXPECT_LE(event->rttUs, (220 + 55) * 1000);
    LOG(INFO) << "node-2 reported new RTT to node-1 to be "
              << event->rttUs / 1000.0 << "ms.";
  }
}

//
// Start two sparks with 200+ interfaces and only enable ECC signature on one
// of them.
//
TEST_F(SparkFixture, StressTest) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture RTT Measurement test";
  };

  if (!FLAGS_stress_test) {
    return;
  }

  //
  // Define interface names for the test. Both spark have `ifaceCount`
  // (configurable) interfaces. As of now 2000 for stress testing
  //
  int ifaceCount = 2000;
  ConnectedIfPairs connectedPairs;

  for (int i = 0; i < ifaceCount; i++) {
    auto ifName = folly::sformat("iface{}", i);
    auto v4Addr = folly::sformat("192.168.{}.{}", i / 256, i % 256);
    auto v6Addr = folly::sformat("fe80::{}:{}", i / 256, i % 256);
    connectedPairs[ifName] = {};
  }

  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1
  auto spark1 = createSpark(kDomainName, "node-1", keyPair1, nullptr, 1);

  // start spark2
  auto spark2 = createSpark(kDomainName, "node-2", keyPair2, nullptr, 2);

  //
  // Add Interfaces to both sparks
  //

  std::vector<InterfaceEntry> interfaceEntries;
  for (int i = 0; i < ifaceCount; i++) {
    auto ifName = folly::sformat("iface{}", i);
    auto ifIndex = i + 1;
    auto v4Addr =
        folly::IPAddressV4(folly::sformat("192.168.{}.{}", i / 256, i % 256));
    auto v6Addr =
        folly::IPAddressV6(folly::sformat("fe80::{}:{}", i / 256, i % 256));
    mockIoProvider->addIfNameIfIndex({{ifName, ifIndex}});
    interfaceEntries.emplace_back(
        InterfaceEntry{ifName, ifIndex, v4Addr, v6Addr});
  }
  EXPECT_TRUE(spark1->updateInterfaceDb(interfaceEntries));
  EXPECT_TRUE(spark2->updateInterfaceDb(interfaceEntries));

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(30));
}

//
// Start N sparks everyone connected to everyone else. Put all even sparks
// into one domain and odd ones into another
//
TEST_F(SparkFixture, DomainTest) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture Domain test";
  };

  const int kNumSparks = 10;

  // connect interfaces directly in full mesh
  ConnectedIfPairs connectedPairs;
  for (int i = 0; i < kNumSparks; i++) {
    auto srcIface = folly::sformat("iface{}", i);
    auto& dstIfaces = connectedPairs[srcIface];

    for (int j = 0; j < kNumSparks; j++) {
      if (i == j) {
        continue;
      }

      auto dstIface = folly::sformat("iface{}", j);
      dstIfaces.emplace_back(dstIface, 100);
    }
  }
  mockIoProvider->setConnectedPairs(connectedPairs);

  //
  // Start all spark modules
  //
  vector<shared_ptr<SparkWrapper>> sparks;
  for (int i = 0; i < kNumSparks; i++) {
    auto domainName = folly::sformat("terra-{}", i % 2);
    auto spark = createSpark(
        domainName,
        folly::sformat("node-{}", i) /* myNodeName */,
        fbzmq::util::genKeyPair(),
        nullptr,
        i);
    sparks.push_back(std::move(spark));
  }

  // Inform all sparks about iface UP event. Let the magic begin...
  for (int i = 0; i < kNumSparks; i++) {
    auto ifaceName = folly::sformat("iface{}", i);
    auto ifIndex = i + 1;
    auto ipv4 = folly::IPAddressV4(folly::sformat("192.168.0.{}", i));
    auto ipv6 = folly::IPAddressV6(folly::sformat("fe80::{}", i));
    mockIoProvider->addIfNameIfIndex({{ifaceName, ifIndex}});
    EXPECT_TRUE(
        sparks[i]->updateInterfaceDb({{ifaceName, ifIndex, ipv4, ipv6}}));
  }

  // Read all messages that were queued up on the socket.
  std::map<std::string, std::set<std::string>> ifToNeighbors;
  for (int i = 0; i < kNumSparks; i++) {
    auto ifaceName = folly::sformat("iface{}", i);
    auto domainName = folly::sformat("terra-{}", i % 2);

    // expect all same parity neighbors
    for (int j = 0; j < kNumSparks / 2 - 1;) {
      auto maybeEvent = sparks[i]->recvNeighborEvent();
      EXPECT_TRUE(maybeEvent.hasValue());
      auto event = maybeEvent.value();
      // Ignore all RTT change events
      if (event.eventType ==
          thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE) {
        continue;
      }
      j++;

      // We must only receive NEIGHBOR_UP event.
      EXPECT_EQ(thrift::SparkNeighborEventType::NEIGHBOR_UP, event.eventType);

      // We have only one interface.
      EXPECT_EQ(ifaceName, event.ifName);

      // Neighbor must be in our domain if detected
      EXPECT_EQ(domainName, event.neighbor.domainName);

      ifToNeighbors[ifaceName].insert(event.neighbor.nodeName);
    }
  }

  // Verify ifToNeighbors mapping
  for (int i = 0; i < kNumSparks; i++) {
    auto ifaceName = folly::sformat("iface{}", i);
    auto& neighbors = ifToNeighbors[ifaceName];
    for (int j = 0; j < kNumSparks; j++) {
      // Ignore cross domain nodes or self edge
      if (i == j || (i - j) % 2 != 0) {
        continue;
      }

      auto nodeName = folly::sformat("node-{}", j);
      EXPECT_EQ(1, neighbors.count(nodeName));
    }
  }

  // Just for debugging
  VLOG(1) << "Discovered neighbors information.";
  for (auto& kv : ifToNeighbors) {
    VLOG(1) << kv.first;
    for (auto& neighbor : kv.second) {
      VLOG(1) << "\t" << neighbor;
    }
  }
}

//
// start spark1 spark2 together first, see if the can form adjacency within 1
// sec. Then kill spark2, wait for spark1 to go over its fast init state. Then
// start spark2, see if they can form adjacency discovery within 1 sec
//
TEST_F(SparkFixture, FastInitTest) {
  SCOPE_EXIT {
    LOG(INFO) << "SparkFixture fast init test";
  };
  using namespace std::chrono;
  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark1, spark2
  auto spark1 = createSpark(
      kDomainName,
      "node-1",
      keyPair1,
      nullptr,
      1,
      milliseconds(6000) /* hold time */,
      milliseconds(2000) /* my keep alive time */,
      milliseconds(200) /* fast keep alive time */);
  auto spark2 = createSpark(
      kDomainName,
      "node-2",
      keyPair2,
      nullptr,
      2,
      milliseconds(6000) /* hold time */,
      milliseconds(2000) /* my keep alive time */,
      milliseconds(200) /* fast keep alive time */);

  auto startTime = steady_clock::now();

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  seconds duration = duration_cast<seconds>(steady_clock::now() - startTime);
  // because of fast init, neighor discovery should be finished within 1 sec
  EXPECT_GE(1, duration.count());

  // fast init will last for "keep alive time" this long. Wait for node-1's
  // fast init state passing by.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  //
  // Kill and restart spark2
  //

  LOG(INFO) << "Killing and restarting node-2";
  startTime = steady_clock::now();

  // We have to use different URL to bind/connect here. ZMQ socket close is
  // async operation and `socket->close()` call returns immediately. There are
  // chances that bind-address might still be in use if ZMQ Reaper thread hasn't
  // cleaned it up.
  spark2 = createSpark(
      kDomainName,
      "node-2",
      keyPair2,
      nullptr,
      3 /* changed */,
      milliseconds(6000) /* hold time */,
      milliseconds(2000) /* my keep alive time */,
      milliseconds(200) /* fast keep alive time */);

  LOG(INFO) << "Adding iface2 to node-2";

  // re-add interface
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  //
  // node-2 will eventually report node-1 as up
  //
  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  duration = duration_cast<seconds>(steady_clock::now() - startTime);
  // because of fast init, neighor discovery should be finished within 1 sec
  EXPECT_GE(1, duration.count());
}

//
// start spark1 spark2 with very short keep-alive time
// should see packets dropped because of this
//
TEST_F(SparkFixture, dropPacketsTest) {
  LOG(INFO) << "dropPacketsTest: starting dropPacketsTest";
  using namespace std::chrono_literals;
  //
  // Define interface names for the test
  //
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 100}}},
      {iface2, {{iface1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // set up a ZMq monitor to get counters
  auto zmqMonitor1 = make_shared<fbzmq::ZmqMonitor>(
      MonitorSubmitUrl{folly::sformat("{}-{}", kSparkCounterCmdUrl, 1)},
      MonitorPubUrl{"inproc://monitor-pub1"},
      context);

  auto zmqMonitor2 = make_shared<fbzmq::ZmqMonitor>(
      MonitorSubmitUrl{folly::sformat("{}-{}", kSparkCounterCmdUrl, 2)},
      MonitorPubUrl{"inproc://monitor-pub2"},
      context);

  auto monitorThread1 = std::make_unique<std::thread>([zmqMonitor1]() {
    LOG(INFO) << "ZmqMonitor thread starting";
    zmqMonitor1->run();
    LOG(INFO) << "ZmqMonitor thread finished";
  });

  auto monitorThread2 = std::make_unique<std::thread>([zmqMonitor2]() {
    LOG(INFO) << "ZmqMonitor thread starting";
    zmqMonitor2->run();
    LOG(INFO) << "ZmqMonitor thread finished";
  });

  zmqMonitor1->waitUntilRunning();
  zmqMonitor2->waitUntilRunning();

  // start spark1, spark2
  auto spark1 = createSpark(
      kDomainName,
      "node-1",
      keyPair1,
      nullptr,
      1,
      600ms /* hold time */,
      200ms /* my keep alive time */,
      20ms /* fast keep alive time */);
  auto spark2 = createSpark(
      kDomainName,
      "node-2",
      keyPair2,
      nullptr,
      2,
      600ms /* hold time */,
      // Make Keep Aive Small so Spark1 will drop some of Spark2's
      // hello packets
      15ms /* my keep alive time */,
      10ms /* fast keep alive time */);

  // start tracking iface1
  EXPECT_TRUE(spark1->updateInterfaceDb(
      {{iface1, ifIndex1, ip1V4.asV4(), ip1V6.asV6()}}));

  // start tracking iface2
  EXPECT_TRUE(spark2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4.asV4(), ip2V6.asV6()}}));

  LOG(INFO) << "Preparing to receive the messages from sparks";

  //
  // Now wait for sparks to detect each other
  //
  {
    auto event =
        waitForEvent(spark1, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        waitForEvent(spark2, thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.hasValue());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  auto zmqMonitorClient1 = std::make_unique<fbzmq::ZmqMonitorClient>(
      context,
      MonitorSubmitUrl{folly::sformat("{}-{}", kSparkCounterCmdUrl, 1)});

  auto zmqMonitorClient2 = std::make_unique<fbzmq::ZmqMonitorClient>(
      context,
      MonitorSubmitUrl{folly::sformat("{}-{}", kSparkCounterCmdUrl, 2)});

  // Get the counters from sparks and see what happened
  auto spark1Counters = zmqMonitorClient1->dumpCounters();
  // Hack to wait for counters to be submitted
  while (spark1Counters.size() == 0) {
    spark1Counters = zmqMonitorClient1->dumpCounters();
  }

  auto spark2Counters = zmqMonitorClient2->dumpCounters();
  while (spark2Counters.size() == 0) {
    spark2Counters = zmqMonitorClient2->dumpCounters();
  }

  std::map<std::basic_string<char>, double> spark1Values, spark2Values;
  for (auto const& kv : spark1Counters) {
    spark1Values[kv.first] = kv.second.value;
  }
  for (auto const& kv : spark2Counters) {
    spark2Values[kv.first] = kv.second.value;
  }

  auto spark1Recv =
      folly::get_default(spark1Values, "spark.hello_packet_recv.sum.0", 0);
  auto spark1Drops =
      folly::get_default(spark1Values, "spark.hello_packet_dropped.sum.0", 0);
  auto spark1Processed =
      folly::get_default(spark1Values, "spark.hello_packet_processed.sum.0", 0);
  auto spark2Recv =
      folly::get_default(spark2Values, "spark.hello_packet_recv.sum.0", 0);
  auto spark2Drops =
      folly::get_default(spark2Values, "spark.hello_packet_dropped.sum.0", 0);
  auto spark2Processed =
      folly::get_default(spark2Values, "spark.hello_packet_processed.sum.0", 0);

  // check that spark1 dropped some packets
  EXPECT_GE(spark1Drops, 1);
  // spark1 was sending packets slowly so spark2 dosen't need to drop any
  EXPECT_EQ(spark2Drops, 0);

  // we should either drop or process every packet we receive
  EXPECT_EQ(spark1Processed + spark1Drops, spark1Recv);
  EXPECT_EQ(spark2Processed + spark2Drops, spark2Recv);

  LOG(INFO) << "Stopping the monitor threads";
  zmqMonitor1->stop();
  monitorThread1->join();
  zmqMonitor2->stop();
  monitorThread2->join();
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
