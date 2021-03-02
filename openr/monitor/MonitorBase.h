/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Function.h>

#include <fb303/ServiceData.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/config/Config.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>
#include <openr/monitor/SystemMetrics.h>

namespace fb303 = facebook::fb303;

namespace openr {

/**
 * This class is a base class for Open/R monitoring. It
 * implements common functions:
 * 1. Start a fiber to read the log queue and export logs to database based on
 *    subclass's processEventLog() implementation.
 * 2. Store and return the most recent logs;
 * 3. Export process counters: process.memory.rss, process.uptime,
 *    and process.cpu.pct
 */
class MonitorBase : public OpenrEventBase {
 public:
  MonitorBase(
      std::shared_ptr<const Config> config,
      const std::string& category,
      messaging::RQueue<LogSample> logSampleQueue);

  // Get recent event logs
  std::list<std::string> getRecentEventLogs();

  // Destructor
  virtual ~MonitorBase() = default;

 protected:
  // Category for log message
  const std::string category_;

 private:
  // Pure virtual function for processing and publishing a log
  virtual void processEventLog(LogSample const& eventLog) = 0;

  // Set process counters
  void updateProcessCounters();

  // Common information added to each log: "domain", "node-name", etc
  LogSample commonLogToMerge_;

  // Number of last log events to queue
  const uint32_t maxLogEvents_{0};

  // List of recent log
  std::list<std::string> recentLog_{};

  // Timer to periodically set process cpu/uptime/memory counter
  std::unique_ptr<folly::AsyncTimeout> setProcessCounterTimer_;

  // Start timestamp for calculate process.uptime.seconds
  const std::chrono::steady_clock::time_point startTime_;

  // Get the system metrics for resource usage counters
  SystemMetrics systemMetrics_{};
};

} // namespace openr
