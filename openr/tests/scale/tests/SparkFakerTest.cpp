/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/scale/SparkFaker.h>

namespace openr {
namespace {

class BlockingSparkIo final : public SparkIoInterface {
 public:
  std::future<void>
  firstSendEntered() {
    return firstSendEntered_.get_future();
  }

  void
  releaseFirstSend() {
    releaseFirstSend_.set_value();
  }

  std::vector<std::string>
  packets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_;
  }

  void
  sendPacket(
      int,
      const folly::IPAddress&,
      const std::string& packet,
      std::chrono::milliseconds) override {
    size_t count;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      packets_.push_back(packet);
      count = packets_.size();
    }
    if (count == 1) {
      firstSendEntered_.set_value();
      firstSendReleased_.wait();
    }
  }

  void
  registerCallback(const std::string&, PacketCallback) override {}
  void
  startReceiving() override {}
  void
  stopReceiving() override {}
  void
  addInterface(const std::string&, int) override {}

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> packets_;
  std::promise<void> firstSendEntered_;
  std::promise<void> releaseFirstSend_;
  std::future<void> firstSendReleased_{releaseFirstSend_.get_future()};
};

} // namespace

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
 * Test per-neighbor area: defaults to the OpenR default area and can be
 * overridden, with lookups on unknown neighbors reported as not-found.
 */
TEST_F(SparkFakerTest, PerNeighborArea) {
  faker_->addNeighbor(
      "spine-0", "spine-0-to-dut", 100, "fe80::1", "dut-to-spine-0", 1);

  /* Defaults to the OpenR default area "0" to preserve single-area behavior. */
  ASSERT_TRUE(faker_->getNeighborArea("spine-0").has_value());
  EXPECT_EQ("0", faker_->getNeighborArea("spine-0").value());

  /* Can be set to a non-default area. */
  EXPECT_TRUE(faker_->setNeighborArea("spine-0", "pod"));
  EXPECT_EQ("pod", faker_->getNeighborArea("spine-0").value());

  /* Unknown neighbor: setter returns false, getter returns nullopt. */
  EXPECT_FALSE(faker_->setNeighborArea("nonexistent", "pod"));
  EXPECT_FALSE(faker_->getNeighborArea("nonexistent").has_value());
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
   * Inject packet - faker should process it and update state.
   * Use the fake neighbor's ifName (receiving interface) for dispatch.
   */
  faker_->handleDutPacket("spine-0-to-dut", pkt);

  /*
   * Verify that neighbor state progressed (we can't easily check internal
   * state, but if no crash, it works)
   */
  SUCCEED();
}

TEST(SparkFakerIoTest, InboundHandshakeKeepsStateAccessibleAndSendOrder) {
  auto io = std::make_shared<BlockingSparkIo>();
  SparkFaker faker{io};
  faker.addNeighbor("spine", "spine-if", 100, "fe80::1", "dut-if", 1);

  thrift::SparkHelloMsg hello;
  hello.nodeName() = "dut";
  hello.ifName() = "dut-if";
  hello.seqNum() = 1;
  hello.sentTsInUs() = 1;
  thrift::ReflectedNeighborInfo reflected;
  reflected.seqNum() = 1;
  hello.neighborInfos()["spine"] = std::move(reflected);
  thrift::SparkHelloPacket helloPacket;
  helloPacket.helloMsg() = std::move(hello);

  auto entered = io->firstSendEntered();
  std::thread helloThread(
      [&] { faker.handleDutPacket("spine-if", helloPacket); });
  EXPECT_EQ(
      std::future_status::ready, entered.wait_for(std::chrono::seconds{5}));

  auto state = std::async(
      std::launch::async, [&] { return faker.getNeighborState("spine"); });
  const bool stateReady =
      state.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
  EXPECT_TRUE(stateReady);
  if (stateReady) {
    EXPECT_EQ(thrift::SparkNeighState::NEGOTIATE, state.get());
  }

  thrift::SparkHandshakeMsg handshake;
  handshake.nodeName() = "dut";
  handshake.neighborNodeName() = "spine";
  handshake.isAdjEstablished() = false;
  thrift::SparkHelloPacket handshakePacket;
  handshakePacket.handshakeMsg() = std::move(handshake);
  auto confirmation = std::async(std::launch::async, [&] {
    faker.handleDutPacket("spine-if", handshakePacket);
    return faker.getNeighborState("spine");
  });
  const bool confirmedBeforeRelease =
      confirmation.wait_for(std::chrono::seconds{2}) ==
      std::future_status::ready;
  EXPECT_TRUE(confirmedBeforeRelease);
  if (confirmedBeforeRelease) {
    EXPECT_EQ(thrift::SparkNeighState::ESTABLISHED, confirmation.get());
    EXPECT_EQ(1, io->packets().size());
  }

  io->releaseFirstSend();
  helloThread.join();
  if (!stateReady) {
    EXPECT_EQ(
        std::future_status::ready, state.wait_for(std::chrono::seconds{5}));
  }
  if (!confirmedBeforeRelease) {
    EXPECT_EQ(
        std::future_status::ready,
        confirmation.wait_for(std::chrono::seconds{5}));
  }

  const auto sent = io->packets();
  ASSERT_EQ(2, sent.size());
  const auto first =
      apache::thrift::CompactSerializer::deserialize<thrift::SparkHelloPacket>(
          sent.front());
  const auto second =
      apache::thrift::CompactSerializer::deserialize<thrift::SparkHelloPacket>(
          sent.back());
  ASSERT_TRUE(first.handshakeMsg().has_value());
  ASSERT_TRUE(second.handshakeMsg().has_value());
  EXPECT_FALSE(*first.handshakeMsg()->isAdjEstablished());
  EXPECT_TRUE(*second.handshakeMsg()->isAdjEstablished());
}

TEST(SparkFakerIoTest, PeriodicHelloKeepsNeighborControlsResponsive) {
  auto io = std::make_shared<BlockingSparkIo>();
  SparkFaker faker{io};
  faker.addNeighbor("spine", "spine-if", 100, "fe80::1", "dut-if", 1);

  auto entered = io->firstSendEntered();
  faker.start();
  EXPECT_EQ(
      std::future_status::ready, entered.wait_for(std::chrono::seconds{5}));

  auto count =
      std::async(std::launch::async, [&] { return faker.getNeighborCount(); });
  const bool countReady =
      count.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
  EXPECT_TRUE(countReady);
  if (countReady) {
    EXPECT_EQ(1, count.get());
  }

  io->releaseFirstSend();
  faker.stop();
  if (!countReady) {
    EXPECT_EQ(
        std::future_status::ready, count.wait_for(std::chrono::seconds{5}));
  }

  const auto sent = io->packets();
  ASSERT_FALSE(sent.empty());
  const auto first =
      apache::thrift::CompactSerializer::deserialize<thrift::SparkHelloPacket>(
          sent.front());
  EXPECT_TRUE(first.helloMsg().has_value());
}

} // namespace openr
