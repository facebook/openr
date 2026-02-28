/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <folly/IPAddress.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/tests/mocks/MockIoProvider.h>

namespace openr {

/*
 * SparkFaker - Fakes Spark neighbors for DUT testing
 *
 * This class generates Spark hello/handshake/heartbeat packets to make
 * a DUT believe it has real neighbors, without running actual OpenR instances.
 *
 * Each fake neighbor is a state machine that follows Spark protocol:
 * IDLE -> WARM -> NEGOTIATE -> ESTABLISHED
 *
 * Usage:
 *   auto faker = std::make_shared<SparkFaker>(mockIoProvider);
 *   faker->addNeighbor("spine-0", "spine-0-to-dut", 100, "fe80::1");
 *   faker->start();
 *
 *   // DUT will now see "spine-0" as a real Spark neighbor
 */
class SparkFaker {
 public:
  /*
   * FakeNeighbor represents a single simulated Spark neighbor
   */
  struct FakeNeighbor {
    std::string nodeName; /* Name of the fake node (e.g., "spine-0") */
    std::string ifName; /* Interface name on fake node */
    int ifIndex{0}; /* Interface index for MockIoProvider */
    folly::IPAddressV6 v6Addr; /* IPv6 address of fake node */
    folly::IPAddressV4 v4Addr; /* IPv4 address of fake node */

    /* Spark state machine */
    thrift::SparkNeighState state{thrift::SparkNeighState::IDLE};
    uint64_t seqNum{1};

    /* DUT's state (learned from DUT's packets) */
    std::string dutNodeName;
    std::optional<uint64_t> dutSeqNum;
    std::optional<int64_t> dutTimestamp;

    /* Interface on DUT that connects to this neighbor */
    std::string dutIfName;
    int dutIfIndex{0};

    /* Timing */
    std::chrono::steady_clock::time_point lastHelloTime;
    std::chrono::steady_clock::time_point lastHeartbeatTime;

    /* Failure simulation - when true, neighbor stops sending packets */
    bool failed{false};
  };

  /*
   * Construct SparkFaker with MockIoProvider for packet injection
   */
  explicit SparkFaker(std::shared_ptr<MockIoProvider> mockIo);

  ~SparkFaker();

  /*
   * Add a fake neighbor
   *
   * @param nodeName Name of the fake node (e.g., "spine-0")
   * @param ifName Interface name on fake node
   * @param ifIndex Interface index for MockIoProvider
   * @param v6Addr IPv6 address of fake node (link-local)
   * @param dutIfName Interface name on DUT that connects to this neighbor
   * @param dutIfIndex Interface index on DUT
   */
  void addNeighbor(
      const std::string& nodeName,
      const std::string& ifName,
      int ifIndex,
      const std::string& v6Addr,
      const std::string& dutIfName,
      int dutIfIndex);

  /*
   * Start sending Spark packets
   * Begins hello loop and state machine processing
   */
  void start();

  /*
   * Stop sending packets
   */
  void stop();

  /*
   * Get count of fake neighbors
   */
  size_t
  getNeighborCount() const {
    return neighbors_.size();
  }

  /*
   * Simulate neighbor failure
   * Stops sending packets from specified neighbor (DUT will timeout)
   *
   * @param nodeName Name of the neighbor to fail
   * @return true if neighbor was found and failed
   */
  bool failNeighbor(const std::string& nodeName);

  /*
   * Recover a failed neighbor
   * Resumes sending packets from specified neighbor
   *
   * @param nodeName Name of the neighbor to recover
   * @return true if neighbor was found and recovered
   */
  bool recoverNeighbor(const std::string& nodeName);

  /*
   * Get neighbor state (for testing)
   */
  std::optional<thrift::SparkNeighState> getNeighborState(
      const std::string& nodeName) const;

  /*
   * Handle packet from DUT (for testing)
   * Normally MockIoProvider delivers packets, but this allows manual injection
   */
  void handleDutPacket(
      const std::string& ifName, const thrift::SparkHelloPacket& packet);

  /*
   * Handle raw packet from DUT via callback
   * Parses the packet and calls handleDutPacket
   */
  void handleRawDutPacket(
      const std::string& srcIfName,
      const std::string& dstIfName,
      const folly::IPAddress& srcAddr,
      const std::string& packet);

  /*
   * Register callbacks with MockIoProvider for bidirectional communication
   * Called automatically in start() after neighbors are added
   */
  void registerCallbacks();

 private:
  /*
   * Build SparkHelloMsg for a neighbor
   */
  thrift::SparkHelloMsg buildHelloMsg(FakeNeighbor& neighbor);

  /*
   * Build SparkHandshakeMsg for a neighbor
   */
  thrift::SparkHandshakeMsg buildHandshakeMsg(
      FakeNeighbor& neighbor, const std::string& dutNodeName);

  /*
   * Build SparkHeartbeatMsg for a neighbor
   */
  thrift::SparkHeartbeatMsg buildHeartbeatMsg(FakeNeighbor& neighbor);

  /*
   * Send hello packet from a fake neighbor
   */
  void sendHello(FakeNeighbor& neighbor);

  /*
   * Send handshake packet from a fake neighbor
   */
  void sendHandshake(FakeNeighbor& neighbor);

  /*
   * Send heartbeat packet from a fake neighbor
   */
  void sendHeartbeat(FakeNeighbor& neighbor);

  /*
   * Main loop that sends periodic hellos and heartbeats
   */
  void runLoop();

  /*
   * Process state machine for a single neighbor
   */
  void processNeighbor(FakeNeighbor& neighbor);

  std::shared_ptr<MockIoProvider> mockIo_;
  std::vector<FakeNeighbor> neighbors_;

  std::atomic<bool> running_{false};
  std::unique_ptr<std::thread> thread_;

  /* Timing parameters (matching Spark defaults) */
  const std::chrono::milliseconds helloInterval_{100};
  const std::chrono::milliseconds heartbeatInterval_{20};
  const std::chrono::milliseconds holdTime_{10000};

  /* Serializer for thrift packets */
  apache::thrift::CompactSerializer serializer_;
};

} // namespace openr
