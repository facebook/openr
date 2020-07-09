// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <folly/Function.h>

#include <fb303/ServiceData.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/config/Config.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>

namespace fb303 = facebook::fb303;

namespace openr {

/**
 * This class is a base class for Open/R monitoring. It
 * implements common functions:
 * 1. Start a fiber to read the log queue and export logs to database based on
 *    subclass's processEventLog() implementation.
 * 2. Store and return the most recent logs;
 * TODO: 3. Export special counters: process.memory.rss, process.uptime,
 *          and process.cpu.pct
 */
class MonitorBase : public OpenrEventBase {
 public:
  MonitorBase(
      std::shared_ptr<const Config> config,
      const std::string& category,
      messaging::ReplicateQueue<LogSample>& eventLogUpdatesQueue,
      const size_t maxLogEvents);

  // Add an event log to queue
  void addEventLog(LogSample const& eventLog);

  // Get recent event logs
  std::list<LogSample> getRecentEventLogs();

  // Destructor
  virtual ~MonitorBase() = default;

 protected:
  // Category for log message
  const std::string category_;

 private:
  // Pure virtual function for processing and publishing a log
  virtual void processEventLog(LogSample const& eventLog) = 0;

  // Common information added to each log: "domain", "node-name", etc
  LogSample commonLogToMerge_;

  // Queue to collect EventLog
  messaging::ReplicateQueue<LogSample>& eventLogUpdatesQueue_;

  // Number of last log events to queue
  const size_t maxLogEvents_{0};

  // List of recent log
  std::list<LogSample> recentLog_{};
};

} // namespace openr
