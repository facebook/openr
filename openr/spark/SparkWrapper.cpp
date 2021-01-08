/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SparkWrapper.h"

namespace openr {

SparkWrapper::SparkWrapper(
    std::string const& myNodeName,
    std::pair<uint32_t, uint32_t> version,
    std::shared_ptr<IoProvider> ioProvider,
    std::shared_ptr<const Config> config,
    bool isRateLimitEnabled)
    : myNodeName_(myNodeName), config_(config) {
  // apply isRateLimitEnabled.
  // Using a plain bool enable/disable for rate-limit here, to leave
  // the knowledge of the default contained in Spark (and not re-specify it
  // here).
  spark_ = isRateLimitEnabled
      ? std::make_shared<Spark>(
            std::nullopt /* ip-tos */,
            interfaceUpdatesQueue_.getReader(),
            neighborUpdatesQueue_,
            KvStoreCmdPort{10002},
            OpenrCtrlThriftPort{2018},
            std::move(ioProvider),
            config,
            version,
            std::nullopt) // no Spark receive rate-limit, for testing
      : std::make_shared<Spark>(
            std::nullopt /* ip-tos */,
            interfaceUpdatesQueue_.getReader(),
            neighborUpdatesQueue_,
            KvStoreCmdPort{10002},
            OpenrCtrlThriftPort{2018},
            std::move(ioProvider),
            config,
            version
            // Go with the default Spark rate-limit
        );
  // For testing - fuzz testing particularly - we want parsing errors to
  // be thrown upward, not suppressed.
  spark_->setThrowParserErrors(true);

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

void
SparkWrapper::updateInterfaceDb(const InterfaceDatabase& ifDb) {
  interfaceUpdatesQueue_.push(ifDb);
}

std::optional<NeighborEvent>
SparkWrapper::recvNeighborEvent(
    std::optional<std::chrono::milliseconds> timeout) {
  auto startTime = std::chrono::steady_clock::now();
  while (not neighborUpdatesReader_.size()) {
    // Break if timeout occurs
    auto now = std::chrono::steady_clock::now();
    if (timeout.has_value() && now - startTime > timeout.value()) {
      return std::nullopt;
    }
    // Yield the thread
    std::this_thread::yield();
  }

  return neighborUpdatesReader_.get().value();
}

std::optional<NeighborEvent>
SparkWrapper::waitForEvent(
    const NeighborEventType eventType,
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
    if (auto maybeEvent = recvNeighborEvent(rcvdTimeout)) {
      auto& event = maybeEvent.value();
      if (eventType == event.eventType) {
        return event;
      }
    }
  }
  return std::nullopt;
}

std::pair<folly::IPAddress, folly::IPAddress>
SparkWrapper::getTransportAddrs(const NeighborEvent& event) {
  return {toIPAddress(*event.info.transportAddressV4_ref()),
          toIPAddress(*event.info.transportAddressV6_ref())};
}

std::optional<SparkNeighState>
SparkWrapper::getSparkNeighState(
    std::string const& ifName, std::string const& neighborName) {
  return spark_->getSparkNeighState(ifName, neighborName).get();
}

void
SparkWrapper::processPacket() {
  spark_->processPacket();
}

} // namespace openr
