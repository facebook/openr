/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Watchdog.h"

#include <openr/common/Util.h>
#include <syslog.h>

using apache::thrift::FRAGILE;

namespace openr {

Watchdog::Watchdog(
    std::string const& myNodeName,
    std::chrono::seconds healthCheckInterval,
    std::chrono::seconds healthCheckThreshold)
    : myNodeName_(myNodeName),
      healthCheckInterval_(healthCheckInterval),
      healthCheckThreshold_(healthCheckThreshold),
      previousStatus_(true) {

  // Schedule periodic timer for checking thread health
  watchdogTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    updateCounters();
  });
  watchdogTimer_->scheduleTimeout(
    healthCheckInterval_, true /* isPeriodic */);
}

void
Watchdog::addEvl(
  ZmqEventLoop* evl, const std::string& name) {
    runImmediatelyOrInEventLoop([&, evl, name]() {
      CHECK_EQ(allEvls_.count(evl), 0);
      allEvls_[evl] = name;
    });
}

void
Watchdog::delEvl(ZmqEventLoop* evl) {
  runImmediatelyOrInEventLoop([&, evl]() {
    CHECK_NE(allEvls_.count(evl), 0);
    allEvls_.erase(evl);
  });
}

void
Watchdog::updateCounters() {
  VLOG(1) << "Checking thread aliveness counters...";

  auto const& now = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch());
  std::vector<std::string> stuckThreads;
  for (auto const& it : allEvls_) {
    auto const& name = it.second;
    auto const& lastTs = it.first->getTimestamp();
    VLOG(4) << "Thread " << name << ", " << (now - lastTs).count()
            << " seconds ever since last thread activity";

    if (now - lastTs > healthCheckThreshold_) {
      // fire a crash right now
      LOG(WARNING) << "Watchdog: " << name << " thread detected to be dead";
      stuckThreads.emplace_back(name);
    }
  }

  if (stuckThreads.size() and previousStatus_) {
    LOG(WARNING) << "Watchdog: Waiting for one more round before crashing";
  }

  if (stuckThreads.size() and !previousStatus_) {
    LOG(WARNING) << "Watchdog: Found stuck threads in consecutive runs";
    fireCrash(folly::join(", ", stuckThreads));
  }

  if (!stuckThreads.size() and !previousStatus_) {
    LOG(INFO) << "Watchdog: Threads seems to have recovered";
  }

  previousStatus_ = stuckThreads.size() == 0;
}

void
Watchdog::fireCrash(const std::string& threads) {
  LOG(WARNING)
    << "***** Unhealthy Openr Thread Detected, Force Crashing... *****";
  syslog(
      LOG_ALERT,
      "%s",
      folly::sformat(
          "OpenR DeadThreadDetector: Thread {} on {} is detected dead. "
          "Triggering crash.", threads, myNodeName_).c_str());
  // hell ya!
  abort();
}

} // namespace openr
