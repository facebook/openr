/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/spark/SparkWrapper.h>

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
            interfaceUpdatesQueue_.getReader(),
            neighborUpdatesQueue_,
            std::move(ioProvider),
            config,
            version,
            std::nullopt) // no Spark receive rate-limit, for testing
      : std::make_shared<Spark>(
            interfaceUpdatesQueue_.getReader(),
            neighborUpdatesQueue_,
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
    XLOG(DBG1) << "Spark running.";
    spark_->run();
    XLOG(DBG1) << "Spark stopped.";
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

void
SparkWrapper::sendAdjDbSyncedSignal() {
  interfaceUpdatesQueue_.push(thrift::InitializationEvent::ADJACENCY_DB_SYNCED);
}

std::optional<NeighborEvents>
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

std::optional<NeighborEvents>
SparkWrapper::waitForEvents(
    const NeighborEventType eventType,
    std::optional<std::chrono::milliseconds> rcvdTimeout,
    std::optional<std::chrono::milliseconds> procTimeout) noexcept {
  auto startTime = std::chrono::steady_clock::now();

  while (true) {
    // check if it is beyond procTimeout
    auto endTime = std::chrono::steady_clock::now();
    if (endTime - startTime > procTimeout.value()) {
      XLOG(ERR) << "Timeout receiving event. Time limit: "
                << procTimeout.value().count();
      break;
    }
    if (auto maybeEvents = recvNeighborEvent(rcvdTimeout)) {
      auto& events = maybeEvents.value();
      NeighborEvents retEvents;
      for (auto& event : events) {
        if (eventType == event.eventType) {
          retEvents.emplace_back(std::move(event));
        }
      }
      if (retEvents.size() > 0) {
        return retEvents;
      }
    }
  }
  return std::nullopt;
}

std::pair<folly::IPAddress, folly::IPAddress>
SparkWrapper::getTransportAddrs(const NeighborEvent& event) {
  return {
      toIPAddress(*event.info.transportAddressV4_ref()),
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
