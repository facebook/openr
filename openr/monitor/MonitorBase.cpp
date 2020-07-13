// Copyright 2004-present Facebook. All Rights Reserved.

#include "openr/monitor/MonitorBase.h"

namespace openr {
MonitorBase::MonitorBase(
    std::shared_ptr<const Config> config,
    const std::string& category,
    messaging::ReplicateQueue<LogSample>& eventLogUpdatesQueue)
    : category_{category},
      eventLogUpdatesQueue_{eventLogUpdatesQueue},
      maxLogEvents_{
          folly::to<uint32_t>(config->getMonitorConfig().max_event_log)} {
  // Initialize stats counter
  fb303::fbData->addStatExportType("monitor.log.publish.failure", fb303::COUNT);

  // Fiber task to read the LogSample from queue and publish
  addFiberTask([
    q = std::move(eventLogUpdatesQueue_.getReader()),
    config,
    this
  ]() mutable noexcept {
    LOG(INFO) << "Starting log sample updates processing fiber";
    while (true) {
      // perform read log from the queue
      auto maybeLog = q.get();
      VLOG(2) << "Received log sample update";
      if (maybeLog.hasError()) {
        LOG(INFO) << "Terminating log sample updates processing fiber";
        break;
      }

      // validate, process and publish the event logs
      try {
        auto inputLog = maybeLog.value();
        // add common attributes
        inputLog.addString("node_name", config->getNodeName());
        inputLog.addString("domain", config->getConfig().domain);

        // throws std::invalid_argument if not exist
        inputLog.getString("event");

        // add to recent log list
        if (recentLog_.size() >= maxLogEvents_) {
          recentLog_.pop_front();
        }
        recentLog_.push_back(inputLog);

        // and publish the log
        processEventLog(inputLog);
      } catch (const std::exception& e) {
        fb303::fbData->addStatValue(
            "monitor.log.publish.failure", 1, fb303::COUNT);
        LOG(ERROR) << "Failed to publish the log. Error: "
                   << folly::exceptionStr(e);
        continue;
      }
    }
  });
}

std::list<LogSample>
MonitorBase::getRecentEventLogs() {
  return recentLog_;
}

} // namespace openr
