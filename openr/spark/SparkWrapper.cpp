/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

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
            initializationEventQueue_.getReader(),
            addrEventQueue_.getReader(),
            neighborUpdatesQueue_,
            std::move(ioProvider),
            config,
            version,
            std::nullopt) // no Spark receive rate-limit, for testing
      : std::make_shared<Spark>(
            interfaceUpdatesQueue_.getReader(),
            initializationEventQueue_.getReader(),
            addrEventQueue_.getReader(),
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
  initializationEventQueue_.close();
  addrEventQueue_.close();
  spark_->stop();
  spark_->waitUntilStopped();
  thread_->join();
}

void
SparkWrapper::updateInterfaceDb(const InterfaceDatabase& ifDb) {
  interfaceUpdatesQueue_.push(ifDb);
}

void
SparkWrapper::sendPrefixDbSyncedSignal() {
  initializationEventQueue_.push(thrift::InitializationEvent::PREFIX_DB_SYNCED);
}

void
SparkWrapper::sendNeighborDownEvent(
    const std::string& ifName, const thrift::BinaryAddress& v6Addr) {
  addrEventQueue_.push(AddressEvent{false, v6Addr, ifName});
}

std::optional<NeighborEvents>
SparkWrapper::recvNeighborEvent(
    std::optional<std::chrono::milliseconds> timeout) {
  auto startTime = std::chrono::steady_clock::now();
  while (!neighborUpdatesReader_.size()) {
    // Break if timeout occurs
    auto now = std::chrono::steady_clock::now();
    if (timeout.has_value() && now - startTime > timeout.value()) {
      return std::nullopt;
    }
    // Yield the thread
    std::this_thread::yield();
  }

  return std::get<NeighborEvents>(neighborUpdatesReader_.get().value());
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

/**
 * Attempt to read an initialization event published by Spark.
 *
 * Args:
 *  timeout: The specified time interval till we wait for a message
 *           to be available.
 *
 * Returns:
 *   thrift::InitializationEvent if read succeeds, std::nullopt otherwise.
 */
std::optional<thrift::InitializationEvent>
SparkWrapper::recvInitializationEvent(
    std::optional<std::chrono::milliseconds> timeout) {
  auto startTime = std::chrono::steady_clock::now();
  while (!neighborUpdatesReader_.size()) {
    // Break if timeout occurs.
    auto now = std::chrono::steady_clock::now();
    if (timeout.has_value() && now - startTime > timeout.value()) {
      return std::nullopt;
    }
    // Yield the thread.
    std::this_thread::yield();
  }

  return std::get<thrift::InitializationEvent>(
      neighborUpdatesReader_.get().value());
}

/**
 * Wait to receive NEIGHBOR_DISCOVERED initialization event published by Spark.
 *
 * Args:
 *  expectEmptyNeighborEvent: Should we expect an empty neighbor event published
 *                            before seeing the initialization event.
 *  rcvdTimeout: The specified time interval till we wait for an individual
 *               message to be available.
 *  procTimeout: The specified time interval till we wait for the initialization
 *               event. (We may read an empty NeighborEvent prior to the
 *               initialization signal, depending on the passed in value of
 *               expectEmptyNeighborEvent.)
 *
 * Returns:
 *   True if the initialization event was received, false otherwise.
 */
bool
SparkWrapper::waitForInitializationEvent(
    bool expectEmptyNeighborEvent,
    std::optional<std::chrono::milliseconds> rcvdTimeout,
    std::optional<std::chrono::milliseconds> procTimeout) noexcept {
  auto startTime = std::chrono::steady_clock::now();

  while (true) {
    // Check if we've been trying beyond procTimeout.
    auto endTime = std::chrono::steady_clock::now();
    if (endTime - startTime > procTimeout.value()) {
      XLOG(ERR) << "Timeout receiving event. Time limit: "
                << procTimeout.value().count();
      break;
    }
    if (expectEmptyNeighborEvent) {
      if (auto maybeEvents = recvNeighborEvent(rcvdTimeout)) {
        auto& events = maybeEvents.value();
        if (events.size() == 0) {
          expectEmptyNeighborEvent = false;
          continue;
        }
        return false;
      }
    }

    if (auto maybeIntializationEvent = recvInitializationEvent(rcvdTimeout)) {
      if (!maybeIntializationEvent.has_value()) {
        return false;
      }
      auto initializationEvent = maybeIntializationEvent.value();
      if (initializationEvent ==
          thrift::InitializationEvent::NEIGHBOR_DISCOVERED) {
        return true;
      }
    }
  }
  return false;
}

std::pair<folly::IPAddress, folly::IPAddress>
SparkWrapper::getTransportAddrs(const NeighborEvent& event) {
  return {toIPAddress(event.neighborAddrV4), toIPAddress(event.neighborAddrV6)};
}

std::optional<thrift::SparkNeighState>
SparkWrapper::getSparkNeighState(
    std::string const& ifName, std::string const& neighborName) {
  return spark_->getSparkNeighState(ifName, neighborName).get();
}

/**
 * Get the count of all active neighbors known to Spark.
 *
 * Returns:
 *  uint64_t: Count of all active neighbors.
 */
uint64_t
SparkWrapper::getTotalNeighborCount() {
  return spark_->getTotalNeighborCount();
}

/**
 * Get the count of all active neighbors known to Spark.
 *
 * Returns:
 *  uint64_t: Count of all active neighbors.
 */
uint64_t
SparkWrapper::getActiveNeighborCount() {
  return spark_->getActiveNeighborCount();
}

void
SparkWrapper::processPacket() {
  spark_->processPacket();
}

} // namespace openr
