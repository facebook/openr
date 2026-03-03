/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/scale/SparkIoInterface.h>

namespace openr {

/*
 * MockSparkIo - Wraps MockIoProvider for in-process testing
 *
 * This adapter allows SparkFaker to use MockIoProvider through the
 * SparkIoInterface abstraction. Used for testing OpenR components
 * in the same process without real network I/O.
 */
class MockSparkIo : public SparkIoInterface {
 public:
  explicit MockSparkIo(std::shared_ptr<MockIoProvider> mockIo)
      : mockIo_(std::move(mockIo)) {}

  void
  sendPacket(
      int dstIfIndex,
      const folly::IPAddress& srcAddr,
      const std::string& packet,
      std::chrono::milliseconds latency =
          std::chrono::milliseconds(0)) override {
    mockIo_->injectPacket(dstIfIndex, srcAddr, packet, latency);
  }

  void
  registerCallback(
      const std::string& ifName, PacketCallback callback) override {
    mockIo_->registerPacketCallback(ifName, callback);
  }

  void
  startReceiving() override {
    /*
     * MockIoProvider is started separately by the test harness.
     * Nothing to do here.
     */
  }

  void
  stopReceiving() override {
    /*
     * MockIoProvider is stopped separately by the test harness.
     * Nothing to do here.
     */
  }

  void
  addInterface(const std::string& ifName, int ifIndex) override {
    mockIo_->addIfNameIfIndex({{ifName, ifIndex}});
  }

  /*
   * Get the underlying MockIoProvider (for tests that need direct access)
   */
  std::shared_ptr<MockIoProvider>
  getMockIoProvider() const {
    return mockIo_;
  }

 private:
  std::shared_ptr<MockIoProvider> mockIo_;
};

} // namespace openr
