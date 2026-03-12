/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>

#include <openr/common/Constants.h>
#include <openr/tests/scale/SparkIoInterface.h>

namespace openr {

/*
 * RealSparkIo - Uses real UDP sockets for testing against real DUTs
 *
 * This implementation sends/receives Spark packets over real network
 * interfaces. Used for scale testing against actual FBOSS/EOS switches
 * connected via VLAN trunk.
 *
 * Usage:
 *   auto io = std::make_shared<RealSparkIo>();
 *   io->addInterface("eth0.1", 100);  // VLAN interface
 *   io->registerCallback("eth0.1", myCallback);
 *   io->startReceiving();
 *
 *   // Send packets via sendPacket()
 *   io->sendPacket(100, srcAddr, packet);
 *
 *   io->stopReceiving();
 */
class RealSparkIo : public SparkIoInterface {
 public:
  RealSparkIo();
  ~RealSparkIo() override;

  void sendPacket(
      int dstIfIndex,
      const folly::IPAddress& srcAddr,
      const std::string& packet,
      std::chrono::milliseconds latency =
          std::chrono::milliseconds(0)) override;

  void registerCallback(
      const std::string& ifName, PacketCallback callback) override;

  void startReceiving() override;

  void stopReceiving() override;

  void addInterface(const std::string& ifName, int ifIndex) override;

  /*
   * Set multicast address and port for Spark discovery.
   * Defaults to Spark's standard multicast address.
   */
  void setMulticastAddress(const folly::IPAddressV6& addr, uint16_t port);

 private:
  /*
   * Create and bind a UDP socket for an interface.
   * Joins the Spark multicast group.
   */
  int createSocket(const std::string& ifName, int ifIndex);

  /*
   * Receive thread function for an interface.
   * Dispatches to all callbacks registered for ifNames sharing this ifIndex.
   */
  void receiveLoop(int ifIndex, int sockFd);

  /*
   * Mutex for thread safety
   */
  std::mutex mutex_;

  /*
   * Interface name <-> index mappings
   */
  std::map<std::string, int> ifNameToIndex_;
  std::map<int, std::string> ifIndexToName_;

  /*
   * Reverse mapping: ifIndex -> all registered ifNames (for multi-dispatch)
   */
  std::map<int, std::set<std::string>> ifIndexToNames_;

  /*
   * Sockets per interface
   */
  std::map<int, int> ifIndexToSockFd_; /* ifIndex -> socket fd */

  /*
   * Callbacks per interface
   */
  std::map<std::string, PacketCallback> callbacks_;

  /*
   * Receive threads per physical interface (keyed by ifIndex)
   */
  std::map<int, std::unique_ptr<std::thread>> receiveThreads_;

  /*
   * Running flag
   */
  std::atomic<bool> running_{false};

  /*
   * Multicast address for Spark (ff02::1:6666)
   */
  folly::IPAddressV6 mcastAddr_{folly::IPAddressV6("ff02::1")};
  uint16_t mcastPort_{6666};
};

} // namespace openr
