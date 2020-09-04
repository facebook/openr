/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/Constants.h>
#include <openr/config/Config.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/spark/Spark.h>

namespace openr {

struct SparkInterfaceEntry {
  std::string ifName;
  int ifIndex;
  folly::CIDRNetwork v4Network;
  folly::CIDRNetwork v6LinkLocalNetwork;
};

/**
 * A utility class to wrap and interact with Spark. It exposes the APIs to
 * send commands to and receive publications from Spark.
 * Mainly used for testing.
 *
 * This should be managed from only one thread. Otherwise behaviour will be
 * undesirable.
 */
class SparkWrapper {
 public:
  SparkWrapper(
      std::string const& myNodeName,
      std::pair<uint32_t, uint32_t> version,
      std::shared_ptr<IoProvider> ioProvider,
      std::shared_ptr<const Config> config,
      bool isRateLimitEnabled = true);

  ~SparkWrapper();

  // start spark
  void run();

  // stop spark
  void stop();

  // add interfaceDb for Spark to tracking
  // return true upon success and false otherwise
  bool updateInterfaceDb(
      const std::vector<SparkInterfaceEntry>& interfaceEntries);

  // receive spark neighbor event
  folly::Expected<thrift::SparkNeighborEvent, fbzmq::Error> recvNeighborEvent(
      std::optional<std::chrono::milliseconds> timeout = std::nullopt);

  std::optional<thrift::SparkNeighborEvent> waitForEvent(
      const thrift::SparkNeighborEventType eventType,
      std::optional<std::chrono::milliseconds> rcvdTimeout = std::nullopt,
      std::optional<std::chrono::milliseconds> procTimeout =
          Constants::kPlatformRoutesProcTimeout) noexcept;

  // utility call to check neighbor state
  std::optional<SparkNeighState> getSparkNeighState(
      std::string const& ifName, std::string const& neighborName);

  static std::pair<folly::IPAddress, folly::IPAddress> getTransportAddrs(
      const thrift::SparkNeighborEvent& event);

  // utility function to construct thrift::AreaConfig.SparkConfigs
  const openr::thrift::SparkConfig
  getSparkConfig() {
    return config_->getSparkConfig();
  }

  /* Forwarded to the underlying Spark processPacket()
   * For test purposes, e.g. to manually invoke packet handling
   * inline on the same thread - bypassing the event-base - as is needed
   * for fuzzing.
   */
  void processPacket();

 private:
  std::string myNodeName_{""};
  std::shared_ptr<const Config> config_{nullptr};

  // Queue to send neighbor event to LinkMonitor
  messaging::ReplicateQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue_;
  messaging::RQueue<thrift::SparkNeighborEvent> neighborUpdatesReader_{
      neighborUpdatesQueue_.getReader()};

  // Queue to receive interface update from LinkMonitor
  messaging::ReplicateQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue_;

  // Spark owned by this wrapper.
  std::shared_ptr<Spark> spark_{nullptr};

  // Thread in which Spark will be running.
  std::unique_ptr<std::thread> thread_{nullptr};
};

} // namespace openr
