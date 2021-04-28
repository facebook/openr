/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Watchdog.h"

#include <openr/common/Constants.h>
#include <openr/common/Util.h>

namespace openr {

Watchdog::Watchdog(std::shared_ptr<const Config> config)
    : myNodeName_(config->getNodeName()),
      interval_(*config->getWatchdogConfig().interval_s_ref()),
      threadTimeout_(*config->getWatchdogConfig().thread_timeout_s_ref()),
      maxMemoryMB_(*config->getWatchdogConfig().max_memory_mb_ref()),
      isDeadThreadDetected_(false) {
  // Schedule periodic timer for checking thread health
  watchdogTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    monitorThreadStatus();
    monitorMemory();
    // Schedule next timer
    watchdogTimer_->scheduleTimeout(interval_);
  });
  watchdogTimer_->scheduleTimeout(interval_);
}

void
Watchdog::addEvb(OpenrEventBase* evb, const std::string& name) {
  CHECK(evb);
  getEvb()->runInEventBaseThreadAndWait([this, evb, name]() {
    CHECK_EQ(monitorEvbs_.count(evb), 0);
    monitorEvbs_.emplace(evb, name);
  });
}

bool
Watchdog::memoryLimitExceeded() {
  bool result;
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait(
      [&result, this]() { result = memExceedTime_.has_value(); });
  return result;
}

void
Watchdog::monitorMemory() {
  auto memInUse_ = systemMetrics_.getRSSMemBytes();
  if (not memInUse_.has_value()) {
    return;
  }
  if (memInUse_.value() / 1e6 > maxMemoryMB_) {
    LOG(WARNING) << fmt::format(
        "[Mem Detector] Critical memory usage: {} bytes. Memory limit: {} MB.",
        memInUse_.value(),
        maxMemoryMB_);
    if (not memExceedTime_.has_value()) {
      memExceedTime_ = std::chrono::steady_clock::now();
      return;
    }
    // check for sustained critical memory usage
    if (std::chrono::steady_clock::now() - memExceedTime_.value() >
        Constants::kMemoryThresholdTime) {
      const std::string msg = fmt::format(
          "[Mem Detector] Memory limit exceeded the permitted limit."
          " Mem used: {}."
          " Mem Limit: {}",
          memInUse_.value(),
          maxMemoryMB_);
      fireCrash(msg);
    }
    return;
  }
  if (memExceedTime_.has_value()) {
    memExceedTime_ = std::nullopt;
  }
}

void
Watchdog::monitorThreadStatus() {
  // Use steady_clock for watchdog as system_clock can change
  auto const& now = std::chrono::steady_clock::now();
  std::vector<std::string> stuckThreads;

  for (auto const& [evb, name] : monitorEvbs_) {
    auto const& lastTs = evb->getTimestamp();
    auto timeDiff =
        std::chrono::duration_cast<std::chrono::seconds>(now - lastTs);
    VLOG(4) << "Thread " << name << ", " << (now - lastTs).count()
            << " seconds ever since last thread activity";

    if (timeDiff > threadTimeout_) {
      LOG(WARNING) << fmt::format(
          "[Dead Thread Detector] {} thread detected to be dead.", name);
      stuckThreads.emplace_back(name);
    }
  }

  if (stuckThreads.size() and isDeadThreadDetected_) {
    // fire a crash right now
    const std::string msg = fmt::format(
        "[Dead Thread Detector] Thread {} on {} is dead. Triggering crash.",
        folly::join(", ", stuckThreads),
        myNodeName_);
    fireCrash(msg);
  }

  if ((not stuckThreads.size()) and isDeadThreadDetected_) {
    LOG(INFO) << "[Dead Thread Detector] Threads seem to have recovered.";
  }

  isDeadThreadDetected_ = (stuckThreads.size() ? true : false);
}

void
Watchdog::fireCrash(const std::string& msg) {
  SYSLOG(ERROR) << msg;
  // hell ya!
  abort();
}

} // namespace openr
