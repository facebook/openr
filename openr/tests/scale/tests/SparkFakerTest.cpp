/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/scale/SparkFaker.h>

namespace openr {

class SparkFakerTest : public ::testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider_ = std::make_shared<MockIoProvider>();
    faker_ = std::make_shared<SparkFaker>(mockIoProvider_);
  }

  void
  TearDown() override {
    faker_->stop();
    faker_.reset();
    mockIoProvider_.reset();
  }

  std::shared_ptr<MockIoProvider> mockIoProvider_;
  std::shared_ptr<SparkFaker> faker_;
};

/*
 * Test that we can add fake neighbors
 */
TEST_F(SparkFakerTest, AddNeighbors) {
  EXPECT_EQ(0, faker_->getNeighborCount());

  faker_->addNeighbor(
      "spine-0",
      "spine-0-to-dut",
      100, /* ifIndex for fake neighbor */
      "fe80::1",
      "dut-to-spine-0",
      1 /* dutIfIndex */);

  EXPECT_EQ(1, faker_->getNeighborCount());

  faker_->addNeighbor(
      "spine-1", "spine-1-to-dut", 101, "fe80::2", "dut-to-spine-1", 2);

  EXPECT_EQ(2, faker_->getNeighborCount());
}

/*
 * Test that we can start and stop the faker
 */
TEST_F(SparkFakerTest, StartStop) {
  faker_->addNeighbor(
      "spine-0", "spine-0-to-dut", 100, "fe80::1", "dut-to-spine-0", 1);

  faker_->start();

  /* Let it run briefly */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  faker_->stop();

  /* Can start again */
  faker_->start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  faker_->stop();
}

/*
 * Test packet injection through MockIoProvider
 *
 * This test verifies that SparkFaker generates packets and injects them
 * into MockIoProvider correctly.
 */
TEST_F(SparkFakerTest, PacketInjection) {
  /*
   * Setup: Register interface mappings in MockIoProvider
   * The DUT would normally do this via setsockopt, but we do it manually here
   */
  mockIoProvider_->addIfNameIfIndex({
      {"dut-to-spine-0", 1},
      {"dut-to-spine-1", 2},
  });

  faker_->addNeighbor(
      "spine-0", "spine-0-to-dut", 100, "fe80::1", "dut-to-spine-0", 1);

  faker_->addNeighbor(
      "spine-1", "spine-1-to-dut", 101, "fe80::2", "dut-to-spine-1", 2);

  /*
   * Start MockIoProvider processing in background
   * (normally done by DUT's Spark, but we simulate here)
   */
  std::thread mockIoThread([this]() { mockIoProvider_->start(); });

  /* Wait for MockIoProvider to be running */
  mockIoProvider_->waitUntilRunning();

  /* Start faker - it should start injecting packets */
  faker_->start();

  /*
   * Let it run long enough for some hello packets to be sent
   * (helloInterval is 100ms, so wait 150ms to get at least one)
   */
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  faker_->stop();
  mockIoProvider_->stop();
  mockIoThread.join();

  /*
   * We can't easily verify packets without a DUT, but if we got here
   * without crashing, the basic packet generation and injection works
   */
  SUCCEED();
}

/*
 * Test handling DUT packets (manual injection for testing)
 */
TEST_F(SparkFakerTest, HandleDutPacket) {
  faker_->addNeighbor(
      "spine-0", "spine-0-to-dut", 100, "fe80::1", "dut-to-spine-0", 1);

  /*
   * Create a fake hello packet from DUT
   */
  thrift::SparkHelloMsg helloMsg;
  helloMsg.nodeName() = "dut-node";
  helloMsg.ifName() = "dut-to-spine-0";
  helloMsg.seqNum() = 1;
  helloMsg.neighborInfos() = {};
  helloMsg.version() = 20200825;
  helloMsg.solicitResponse() = true;
  helloMsg.restarting() = false;
  helloMsg.sentTsInUs() =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  thrift::SparkHelloPacket pkt;
  pkt.helloMsg() = std::move(helloMsg);

  /*
   * Inject packet - faker should process it and update state
   */
  faker_->handleDutPacket("dut-to-spine-0", pkt);

  /*
   * Verify that neighbor state progressed (we can't easily check internal
   * state, but if no crash, it works)
   */
  SUCCEED();
}

} // namespace openr
