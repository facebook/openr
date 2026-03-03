/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <folly/IPAddress.h>

namespace openr {

/*
 * SparkIoInterface - Abstract interface for SparkFaker I/O operations
 *
 * This interface abstracts packet sending and receiving, allowing SparkFaker
 * to work with either:
 *   - MockSparkIo: Uses MockIoProvider for in-process testing
 *   - RealSparkIo: Uses real UDP sockets for testing against real DUTs
 *
 * The Spark protocol logic (hello, handshake, heartbeat, state machine)
 * remains exactly the same - only the I/O layer changes.
 */
class SparkIoInterface {
 public:
  virtual ~SparkIoInterface() = default;

  /*
   * Callback type for receiving packets from DUT.
   *
   * @param srcIfName Source interface name (DUT's interface)
   * @param dstIfName Destination interface name (our fake neighbor's interface)
   * @param srcAddr Source IP address
   * @param packet Raw packet data (serialized SparkHelloPacket)
   */
  using PacketCallback = std::function<void(
      const std::string& srcIfName,
      const std::string& dstIfName,
      const folly::IPAddress& srcAddr,
      const std::string& packet)>;

  /*
   * Send a packet to the DUT.
   *
   * @param dstIfIndex Interface index on DUT to deliver packet to
   * @param srcAddr Source IP address (fake neighbor's address)
   * @param packet Raw packet data (serialized SparkHelloPacket)
   * @param latency Optional delay before delivery (only used in mock mode)
   */
  virtual void sendPacket(
      int dstIfIndex,
      const folly::IPAddress& srcAddr,
      const std::string& packet,
      std::chrono::milliseconds latency = std::chrono::milliseconds(0)) = 0;

  /*
   * Register a callback to receive packets from DUT on a specific interface.
   *
   * @param ifName Interface name to listen on
   * @param callback Function to call when packet arrives
   */
  virtual void registerCallback(
      const std::string& ifName, PacketCallback callback) = 0;

  /*
   * Start receiving packets.
   * For RealSparkIo, this starts the receive thread(s).
   * For MockSparkIo, this is typically a no-op.
   */
  virtual void startReceiving() = 0;

  /*
   * Stop receiving packets.
   */
  virtual void stopReceiving() = 0;

  /*
   * Add interface name to ifIndex mapping.
   * Used to map between interface names and indices.
   *
   * @param ifName Interface name
   * @param ifIndex Interface index
   */
  virtual void addInterface(const std::string& ifName, int ifIndex) = 0;
};

} // namespace openr
