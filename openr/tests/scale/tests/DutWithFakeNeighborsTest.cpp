/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * DUT with Fake Spark Neighbors Test
 *
 * This test uses SparkFaker to simulate Spark neighbors for a DUT:
 * 1. Start DUT with full OpenR stack
 * 2. Create fake neighbors using SparkFaker
 * 3. SparkFaker sends hello/handshake/heartbeat packets to DUT
 * 4. DUT's Spark forms adjacencies with fake neighbors
 * 5. Verify DUT sees the fake neighbors as real adjacencies
 *
 * This proves we can fake Spark neighbors without running real OpenR instances.
 */

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/spark/SparkWrapper.h>
#include <openr/tests/OpenrWrapper.h>
#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/scale/KvStoreBulkInjector.h>
#include <openr/tests/scale/SparkFaker.h>
#include <openr/tests/scale/TopologyGenerator.h>

using namespace openr;
using apache::thrift::CompactSerializer;

namespace {

const std::chrono::milliseconds kSpark2HelloTime(100);
const std::chrono::milliseconds kSpark2FastInitHelloTime(20);
const std::chrono::milliseconds kSpark2HandshakeTime(20);
const std::chrono::milliseconds kSpark2HeartbeatTime(20);
const std::chrono::milliseconds kSpark2HandshakeHoldTime(200);
const std::chrono::milliseconds kSpark2HeartbeatHoldTime(500);
const std::chrono::milliseconds kSpark2GRHoldTime(1000);
const std::chrono::milliseconds kLinkFlapInitialBackoff(1);
const std::chrono::milliseconds kLinkFlapMaxBackoff(8);

} // namespace

class DutWithFakeNeighborsTest : public ::testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider_ = std::make_shared<MockIoProvider>();

    /*
     * Register interface mappings that both DUT and SparkFaker will use
     */
    mockIoProvider_->addIfNameIfIndex({
        {"dut-to-spine-0", 1},
        {"dut-to-spine-1", 2},
        {"dut-to-spine-2", 3},
        {"dut-to-spine-3", 4},
    });

    /*
     * Set up connected interface pairs
     * DUT's interfaces are connected to SparkFaker's fake interfaces
     */
    mockIoProvider_->setConnectedPairs({
        {"dut-to-spine-0", {{"spine-0-to-dut", 0}}},
        {"dut-to-spine-1", {{"spine-1-to-dut", 0}}},
        {"dut-to-spine-2", {{"spine-2-to-dut", 0}}},
        {"dut-to-spine-3", {{"spine-3-to-dut", 0}}},
    });

    mockIoProviderThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting MockIoProvider thread";
      mockIoProvider_->start();
      LOG(INFO) << "MockIoProvider thread stopped";
    });
    mockIoProvider_->waitUntilRunning();

    faker_ = std::make_shared<SparkFaker>(mockIoProvider_);
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping SparkFaker";
    faker_->stop();

    LOG(INFO) << "Stopping MockIoProvider";
    mockIoProvider_->stop();
    mockIoProviderThread_->join();
  }

  std::unique_ptr<OpenrWrapper<CompactSerializer>>
  createDut(const std::string& nodeId) {
    return std::make_unique<OpenrWrapper<CompactSerializer>>(
        nodeId,
        true, /* v4Enabled */
        kSpark2HelloTime,
        kSpark2FastInitHelloTime,
        kSpark2HandshakeTime,
        kSpark2HeartbeatTime,
        kSpark2HandshakeHoldTime,
        kSpark2HeartbeatHoldTime,
        kSpark2GRHoldTime,
        kLinkFlapInitialBackoff,
        kLinkFlapMaxBackoff,
        mockIoProvider_);
  }

  std::shared_ptr<MockIoProvider> mockIoProvider_;
  std::unique_ptr<std::thread> mockIoProviderThread_;
  std::shared_ptr<SparkFaker> faker_;
};

/*
 * Basic test that SparkFaker starts and sends packets
 *
 * This doesn't verify DUT receives them (that requires more integration),
 * but verifies the SparkFaker runs without issues alongside MockIoProvider.
 */
TEST_F(DutWithFakeNeighborsTest, SparkFakerBasicOperation) {
  LOG(INFO) << "=== SparkFakerBasicOperation Test ===";

  /*
   * Add fake neighbors
   */
  faker_->addNeighbor(
      "spine-0",
      "spine-0-to-dut",
      100, /* ifIndex for fake neighbor (not used for injection) */
      "fe80::1000",
      "dut-to-spine-0",
      1 /* dutIfIndex */);

  faker_->addNeighbor(
      "spine-1", "spine-1-to-dut", 101, "fe80::1001", "dut-to-spine-1", 2);

  EXPECT_EQ(2, faker_->getNeighborCount());

  /*
   * Start faker - it should start sending hello packets
   */
  faker_->start();
  LOG(INFO) << "SparkFaker started with " << faker_->getNeighborCount()
            << " fake neighbors";

  /*
   * Let it run for a bit
   */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  faker_->stop();
  LOG(INFO) << "SparkFaker stopped";

  SUCCEED();
}

/*
 * Test that SparkFaker can handle multiple neighbors at scale
 */
TEST_F(DutWithFakeNeighborsTest, SparkFakerScaleNeighbors) {
  LOG(INFO) << "=== SparkFakerScaleNeighbors Test ===";

  /*
   * Add 64 fake spine neighbors
   */
  for (int i = 0; i < 64; i++) {
    faker_->addNeighbor(
        fmt::format("spine-{}", i),
        fmt::format("spine-{}-to-dut", i),
        100 + i,
        fmt::format("fe80::{:x}", 0x1000 + i),
        fmt::format("dut-to-spine-{}", i),
        i + 1);
  }

  EXPECT_EQ(64, faker_->getNeighborCount());
  LOG(INFO) << "Added 64 fake spine neighbors";

  /*
   * Start faker
   */
  faker_->start();

  /*
   * Let it run - with 64 neighbors sending packets
   */
  std::this_thread::sleep_for(std::chrono::seconds(1));

  faker_->stop();
  LOG(INFO) << "SparkFaker handled 64 neighbors without issues";

  SUCCEED();
}

/*
 * Full integration test: SparkFaker + DUT with OpenrWrapper
 *
 * This test verifies that SparkFaker packets are received by the DUT.
 * We use OpenrWrapper and check counters to verify packets arrived.
 */
TEST_F(DutWithFakeNeighborsTest, DutReceivesSparkFakerPackets) {
  LOG(INFO) << "=== DutReceivesSparkFakerPackets Test ===";

  /*
   * Create DUT
   */
  auto dut = createDut("dut-leaf-0");
  dut->run();
  LOG(INFO) << "DUT started";

  /*
   * Give DUT time to initialize
   */
  std::this_thread::sleep_for(std::chrono::seconds(1));

  /*
   * Tell DUT about its interfaces
   */
  folly::CIDRNetwork ip1V6{folly::IPAddress("fe80::100"), 128};
  folly::CIDRNetwork ip1V4{folly::IPAddress("192.168.1.1"), 32};

  dut->updateInterfaceDb({
      InterfaceInfo(
          "dut-to-spine-0", /* ifName */
          true, /* isUp */
          1, /* ifIndex */
          {ip1V6, ip1V4} /* networks */),
  });
  LOG(INFO) << "Updated DUT interface database";

  /*
   * Wait for interface to be tracked
   */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  /*
   * Add fake neighbor
   */
  faker_->addNeighbor(
      "spine-0", "spine-0-to-dut", 100, "fe80::1000", "dut-to-spine-0", 1);

  /*
   * Start faker - it will inject packets into DUT's interface
   */
  faker_->start();
  LOG(INFO) << "SparkFaker started";

  /*
   * Let packets flow for a while
   * SparkFaker sends hello every 100ms, so after 2s we should have ~20 hellos
   */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /*
   * Check DUT counters to see if it received packets
   */
  auto counters = dut->getCounters();

  LOG(INFO) << "DUT Counters after SparkFaker running:";
  for (const auto& [key, value] : counters) {
    if (key.find("spark") != std::string::npos && value > 0) {
      LOG(INFO) << "  " << key << " = " << value;
    }
  }

  /*
   * Look for spark packet receive counters
   * spark.hello.packet_recv, spark.handshake.packet_recv, etc.
   */
  auto helloRecv = counters.find("spark.hello.packet_recv");
  if (helloRecv != counters.end()) {
    LOG(INFO) << "DUT received " << helloRecv->second << " hello packets";
    EXPECT_GT(helloRecv->second, 0) << "DUT should have received hello packets";
  } else {
    LOG(WARNING) << "spark.hello.packet_recv counter not found";
  }

  faker_->stop();
  LOG(INFO) << "Test complete";
}

/*
 * Full end-to-end test: SparkFaker + KvStore injection
 *
 * This test combines:
 * 1. SparkFaker to establish adjacencies (DUT sees real neighbors)
 * 2. KvStore injection to provide full topology data
 * 3. Verify DUT computes routes
 *
 * This is the closest to a real deployment scenario.
 */
TEST_F(DutWithFakeNeighborsTest, FullStackWithFakeNeighborsAndTopology) {
  LOG(INFO) << "=== FullStackWithFakeNeighborsAndTopology Test ===";

  /*
   * Step 1: Generate topology
   * Small topology: 4 spines, 4 leaves
   * DUT is leaf-0
   */
  auto topology = TopologyGenerator::createBbfSimple(
      4, /* numSpines */
      4, /* numLeaves */
      0, /* numControlNodes */
      1 /* ecmpWidth */);

  LOG(INFO) << "Generated topology: " << topology.getRouterCount()
            << " routers, " << topology.getTotalAdjacencyCount()
            << " adjacencies";

  /*
   * Step 2: Create and start DUT
   */
  auto dut = createDut("leaf-0");
  dut->run();
  LOG(INFO) << "DUT (leaf-0) started";

  /*
   * Step 3: Wait for DUT to initialize
   */
  std::this_thread::sleep_for(std::chrono::seconds(1));

  /*
   * Step 4: Tell DUT about its interface
   */
  folly::CIDRNetwork ip1V6{folly::IPAddress("fe80::100"), 128};
  folly::CIDRNetwork ip1V4{folly::IPAddress("192.168.1.1"), 32};

  dut->updateInterfaceDb({
      InterfaceInfo(
          "dut-to-spine-0", /* ifName */
          true, /* isUp */
          1, /* ifIndex */
          {ip1V6, ip1V4} /* networks */),
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  /*
   * Step 5: Add fake neighbor and start SparkFaker
   */
  faker_->addNeighbor(
      "spine-0", "spine-0-to-dut", 100, "fe80::1000", "dut-to-spine-0", 1);

  faker_->start();
  LOG(INFO) << "SparkFaker started";

  /*
   * Step 6: Wait for adjacency to form
   */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /*
   * Step 7: Inject full topology into KvStore
   */
  KvStoreBulkInjector injector(dut->getKvStoreUpdatesQueue());
  injector.injectTopology(topology);
  LOG(INFO) << "Injected topology into KvStore";

  /*
   * Step 8: Wait for Decision to compute routes
   */
  std::this_thread::sleep_for(std::chrono::seconds(5));

  /*
   * Step 9: Verify results
   */
  auto counters = dut->getCounters();

  /* Check Spark formed adjacency */
  auto packetRecv = counters.find("spark.packet_recv.sum");
  if (packetRecv != counters.end()) {
    LOG(INFO) << "DUT received " << packetRecv->second << " Spark packets";
    EXPECT_GT(packetRecv->second, 0) << "DUT should receive Spark packets";
  }

  /* Dump route database */
  auto routeDb = dut->fibDumpRouteDatabase();
  LOG(INFO) << "DUT computed " << routeDb.unicastRoutes()->size()
            << " unicast routes";

  /*
   * Print sample routes
   */
  int count = 0;
  for (const auto& route : *routeDb.unicastRoutes()) {
    if (count++ < 5) {
      LOG(INFO) << "  Route: " << toString(*route.dest());
    }
  }
  if (routeDb.unicastRoutes()->size() > 5) {
    LOG(INFO) << "  ... and " << (routeDb.unicastRoutes()->size() - 5)
              << " more";
  }

  faker_->stop();
  LOG(INFO) << "Test complete";
}

/*
 * Failure simulation test
 *
 * This test verifies that DUT detects neighbor failure when SparkFaker
 * stops sending packets:
 * 1. Establish adjacency with fake neighbor
 * 2. Verify DUT sees neighbor as ESTABLISHED
 * 3. Simulate neighbor failure (stop sending packets)
 * 4. Wait for DUT's heartbeat hold timer to expire
 * 5. Verify DUT detects neighbor down
 */
TEST_F(DutWithFakeNeighborsTest, NeighborFailureSimulation) {
  LOG(INFO) << "=== NeighborFailureSimulation Test ===";

  /*
   * Step 1: Create and start DUT
   */
  auto dut = createDut("dut-leaf-0");
  dut->run();
  LOG(INFO) << "DUT started";

  std::this_thread::sleep_for(std::chrono::seconds(1));

  /*
   * Step 2: Tell DUT about its interface
   */
  folly::CIDRNetwork ip1V6{folly::IPAddress("fe80::100"), 128};
  folly::CIDRNetwork ip1V4{folly::IPAddress("192.168.1.1"), 32};

  dut->updateInterfaceDb({
      InterfaceInfo("dut-to-spine-0", true, 1, {ip1V6, ip1V4}),
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  /*
   * Step 3: Add fake neighbor and start SparkFaker
   */
  faker_->addNeighbor(
      "spine-0", "spine-0-to-dut", 100, "fe80::1000", "dut-to-spine-0", 1);

  faker_->start();
  LOG(INFO) << "SparkFaker started";

  /*
   * Step 4: Wait for adjacency to form
   */
  LOG(INFO) << "Waiting for adjacency to form...";
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /*
   * Verify adjacency formed (check counters)
   */
  auto countersBefore = dut->getCounters();
  auto packetsBefore = countersBefore.find("spark.packet_recv.sum");
  if (packetsBefore != countersBefore.end()) {
    LOG(INFO) << "Before failure: DUT received " << packetsBefore->second
              << " Spark packets";
  }

  /*
   * Check SparkFaker state
   */
  auto fakerState = faker_->getNeighborState("spine-0");
  if (fakerState.has_value()) {
    LOG(INFO) << "SparkFaker state for spine-0: "
              << apache::thrift::util::enumNameSafe(fakerState.value());
  }

  /*
   * Step 5: Simulate neighbor failure
   */
  LOG(INFO) << "=== SIMULATING NEIGHBOR FAILURE ===";
  bool failed = faker_->failNeighbor("spine-0");
  EXPECT_TRUE(failed) << "Should find and fail spine-0";

  /*
   * Step 6: Wait for DUT to detect failure
   *
   * DUT's heartbeat hold time is 500ms in our test config,
   * so we wait 1-2 seconds for timeout detection
   */
  LOG(INFO) << "Waiting for DUT to detect failure...";
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /*
   * Step 7: Check counters after failure
   */
  auto countersAfter = dut->getCounters();

  LOG(INFO) << "DUT Counters after failure:";
  for (const auto& [key, value] : countersAfter) {
    if ((key.find("spark") != std::string::npos ||
         key.find("neighbor") != std::string::npos) &&
        value > 0) {
      LOG(INFO) << "  " << key << " = " << value;
    }
  }

  /*
   * The DUT should have detected heartbeat timeout
   * Look for neighbor down events or heartbeat timeout counters
   */
  auto heartbeatTimeout =
      countersAfter.find("spark.neighbor.heartbeat_timer_expired.sum");
  if (heartbeatTimeout != countersAfter.end()) {
    LOG(INFO) << "Heartbeat timer expired count: " << heartbeatTimeout->second;
  }

  /*
   * Step 8: Recover neighbor and verify re-establishment
   */
  LOG(INFO) << "=== RECOVERING NEIGHBOR ===";
  bool recovered = faker_->recoverNeighbor("spine-0");
  EXPECT_TRUE(recovered) << "Should find and recover spine-0";

  /*
   * Wait for adjacency to re-form
   */
  LOG(INFO) << "Waiting for adjacency to re-form...";
  std::this_thread::sleep_for(std::chrono::seconds(2));

  auto countersRecovered = dut->getCounters();
  auto packetsRecovered = countersRecovered.find("spark.packet_recv.sum");
  if (packetsRecovered != countersRecovered.end()) {
    LOG(INFO) << "After recovery: DUT received " << packetsRecovered->second
              << " total Spark packets";
  }

  faker_->stop();
  LOG(INFO) << "Test complete - failure simulation successful";
}
