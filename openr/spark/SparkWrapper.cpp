/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SparkWrapper.h"

using namespace fbzmq;

namespace openr {

SparkWrapper::SparkWrapper(
    std::string const& myDomainName,
    std::string const& myNodeName,
    std::pair<uint32_t, uint32_t> version,
    std::shared_ptr<IoProvider> ioProvider,
    std::shared_ptr<thrift::OpenrConfig> config,
    SparkTimeConfig timeConfig)
    : myNodeName_(myNodeName) {
  spark_ = std::make_shared<Spark>(
      myDomainName,
      myNodeName,
      static_cast<uint16_t>(6666),
      timeConfig.myHelloTime, // spark2_hello_time
      timeConfig.myHelloFastInitTime, // spark2_hello_fast_init_time
      timeConfig.myHandshakeTime, // spark2_handshake_time
      timeConfig.myHeartbeatTime, // spark2_heartbeat_time
      timeConfig.myHandshakeHoldTime, // spark2_handshale_hold_time
      timeConfig.myHeartbeatHoldTime, // spark2_heartbeat_hold_time
      timeConfig.myGracefulRestartHoldTime, // spark2_gr_hold_time
      std::nullopt /* ip-tos */,
      true /* enableV4 */,
      interfaceUpdatesQueue_.getReader(),
      neighborUpdatesQueue_,
      KvStoreCmdPort{10002},
      OpenrCtrlThriftPort{2018},
      version,
      std::move(ioProvider),
      true /* enableFloodOptimization */,
      std::move(config));

  // start spark
  run();
}

SparkWrapper::~SparkWrapper() {
  stop();
}

void
SparkWrapper::run() {
  thread_ = std::make_unique<std::thread>([this]() {
    VLOG(1) << "Spark running.";
    spark_->run();
    VLOG(1) << "Spark stopped.";
  });
  spark_->waitUntilRunning();
}

void
SparkWrapper::stop() {
  interfaceUpdatesQueue_.close();
  spark_->stop();
  spark_->waitUntilStopped();
  thread_->join();
}

bool
SparkWrapper::updateInterfaceDb(
    const std::vector<SparkInterfaceEntry>& interfaceEntries) {
  thrift::InterfaceDatabase ifDb(
      apache::thrift::FRAGILE, myNodeName_, {}, thrift::PerfEvents());
  ifDb.perfEvents_ref().reset();

  for (const auto& interface : interfaceEntries) {
    ifDb.interfaces.emplace(
        interface.ifName,
        createThriftInterfaceInfo(
            true,
            interface.ifIndex,
            {toIpPrefix(interface.v4Network),
             toIpPrefix(interface.v6LinkLocalNetwork)}));
  }

  interfaceUpdatesQueue_.push(std::move(ifDb));
  return true;
}

folly::Expected<thrift::SparkNeighborEvent, Error>
SparkWrapper::recvNeighborEvent(
    std::optional<std::chrono::milliseconds> timeout) {
  auto startTime = std::chrono::steady_clock::now();
  while (not neighborUpdatesReader_.size()) {
    // Break if timeout occurs
    auto now = std::chrono::steady_clock::now();
    if (timeout.has_value() && now - startTime > timeout.value()) {
      return folly::makeUnexpected(Error(-1, std::string("timed out")));
    }
    // Yield the thread
    std::this_thread::yield();
  }

  return neighborUpdatesReader_.get().value();
}

std::optional<thrift::SparkNeighborEvent>
SparkWrapper::waitForEvent(
    const thrift::SparkNeighborEventType eventType,
    std::optional<std::chrono::milliseconds> rcvdTimeout,
    std::optional<std::chrono::milliseconds> procTimeout) noexcept {
  auto startTime = std::chrono::steady_clock::now();

  while (true) {
    // check if it is beyond procTimeout
    auto endTime = std::chrono::steady_clock::now();
    if (endTime - startTime > procTimeout.value()) {
      LOG(ERROR) << "Timeout receiving event. Time limit: "
                 << procTimeout.value().count();
      break;
    }
    auto maybeEvent = recvNeighborEvent(rcvdTimeout);
    if (maybeEvent.hasError()) {
      LOG(ERROR) << "recvNeighborEvent failed: " << maybeEvent.error();
      continue;
    }
    auto& event = maybeEvent.value();
    if (eventType == event.eventType) {
      return event;
    }
  }
  return std::nullopt;
}

std::pair<folly::IPAddress, folly::IPAddress>
SparkWrapper::getTransportAddrs(const thrift::SparkNeighborEvent& event) {
  return {toIPAddress(event.neighbor.transportAddressV4),
          toIPAddress(event.neighbor.transportAddressV6)};
}

std::optional<SparkNeighState>
SparkWrapper::getSparkNeighState(
    std::string const& ifName, std::string const& neighborName) {
  return spark_->getSparkNeighState(ifName, neighborName);
}

thrift::AreaConfig
SparkWrapper::createAreaConfig(
    const std::string& areaId,
    const std::vector<std::string>& neighborRegexes,
    const std::vector<std::string>& interfaceRegexes) {
  thrift::AreaConfig areaConfig;
  areaConfig.area_id = areaId;
  areaConfig.neighbor_regexes = neighborRegexes;
  areaConfig.interface_regexes = interfaceRegexes;
  return areaConfig;
}

} // namespace openr
