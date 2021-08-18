/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/watchdog/Watchdog.h"
#include <fb303/ServiceData.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/watchdog/Watchdog.h>
#include "folly/Range.h"
#include "openr/messaging/ReplicateQueue.h"

namespace fb303 = facebook::fb303;

namespace openr {

Watchdog::Watchdog(std::shared_ptr<const Config> config)
    : myNodeName_(config->getNodeName()),
      interval_(*config->getWatchdogConfig().interval_s_ref()),
      threadTimeout_(*config->getWatchdogConfig().thread_timeout_s_ref()),
      maxMemoryMB_(*config->getWatchdogConfig().max_memory_mb_ref()),
      isDeadThreadDetected_(false) {
  // Schedule periodic timer for checking thread health
  watchdogTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    // check dead thread
    monitorThreadStatus();

    // check overall memory usage
    monitorMemory();

    // collect evb specific counters
    updateThreadCounters();

    // collect queue counters
    updateQueueCounters();

    // Schedule next timer
    watchdogTimer_->scheduleTimeout(interval_);
  });
  watchdogTimer_->scheduleTimeout(interval_);
}

void
Watchdog::addEvb(OpenrEventBase* evb) {
  CHECK(evb);
  getEvb()->runInEventBaseThreadAndWait([this, evb]() {
    CHECK_EQ(monitorEvbs_.count(evb), 0);
    monitorEvbs_.emplace(evb);
  });
}

void
Watchdog::addQueue(messaging::ReplicateQueueBase& q, const std::string& qName) {
  if (monitoredQs_.find(qName) == monitoredQs_.end()) {
    monitoredQs_.emplace(qName, std::ref(q));
  } else {
    LOG(INFO) << "Queue " << qName << " is already registered.";
  }
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

  for (auto const& evb : monitorEvbs_) {
    auto const& name = evb->getEvbName();
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
Watchdog::updateThreadCounters() {
  for (auto& evb : monitorEvbs_) {
    // Asynchronously fetch thread mem usage data inside each individual evb
    evb->runInEventBaseThread([name = evb->getEvbName()]() {
      auto allocBytes = memory::getThreadBytesImpl(true);
      auto deallocBytes = memory::getThreadBytesImpl(false);
      auto diff =
          (allocBytes < deallocBytes ? 0 : (allocBytes - deallocBytes) / 1024);

      fb303::fbData->setCounter(
          fmt::format("watchdog.thread_mem_usage_kb.{}", name), diff);
    });

    // Record eventbase's notification queue size to support memory check
    fb303::fbData->setCounter(
        fmt::format("watchdog.evb_queue_size.{}", evb->getEvbName()),
        evb->getEvb()->getNotificationQueueSize());
  }
}

void
Watchdog::fireCrash(const std::string& msg) {
  SYSLOG(ERROR) << msg;
  // hell ya!
  abort();
}

void
Watchdog::updateQueueCounters() {
  // TODO: T98478475 - Currently we only log the counters but don't take any
  // action if the queue keeps growing. In future, via T98478475, we should
  // invoke fireCrash() if the queue keeps growing.
  LOG(INFO) << "[QueueMonitor] Updating queue counters";
  for (auto const& [qName, q] : monitoredQs_) {
    fb303::fbData->setCounter(
        fmt::format("messaging.replicate_queue.{}.readers", qName),
        q.get().getNumReaders());
    fb303::fbData->setCounter(
        fmt::format("messaging.replicate_queue.{}.messages_sent", qName),
        q.get().getNumWrites());

    // Get the stats for each replicated queue
    // TODO: 1. Handle the delete scenario where a reader disconnects
    // TODO: 2. Assign keys to each reader so we can record the stats without
    //          risk of mixup
    std::vector<messaging::RWQueueStats> stats = q.get().getReplicationStats();
    for (auto& stat : stats) {
      fb303::fbData->setCounter(
          fmt::format("messaging.rw_queue.{}-{}.size", qName, stat.queueId),
          stat.size);

      fb303::fbData->setCounter(
          fmt::format("messaging.rw_queue.{}-{}.read", qName, stat.queueId),
          stat.reads);

      fb303::fbData->setCounter(
          fmt::format("messaging.rw_queue.{}-{}.sent", qName, stat.queueId),
          stat.writes);
    }
  }
}

} // namespace openr
