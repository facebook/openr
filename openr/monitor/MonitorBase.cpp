/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/monitor/MonitorBase.h"
#include <folly/logging/xlog.h>
#include <openr/common/Constants.h>

namespace openr {

MonitorBase::MonitorBase(
    std::shared_ptr<const Config> config,
    const std::string& category,
    messaging::RQueue<LogSample> logSampleQueue)
    : category_{category},
      maxLogEvents_{
          folly::to<uint32_t>(*config->getMonitorConfig().max_event_log())},
      startTime_{std::chrono::steady_clock::now()} {
  // Initialize stats counter
  fb303::fbData->addStatExportType("monitor.log.publish.failure", fb303::COUNT);

  // Periodically set process cpu/uptime/memory counter
  setProcessCounterTimer_ =
      folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
        updateProcessCounters();
        setProcessCounterTimer_->scheduleTimeout(
            Constants::kCounterSubmitInterval);
      });
  // Schedule an immediate timeout
  setProcessCounterTimer_->scheduleTimeout(0);

  // Periodically dump the heap memory profile
  if (config->isMemoryProfilingEnabled()) {
    dumpHeapProfileTimer_ =
        folly::AsyncTimeout::make(*getEvb(), [this, config]() noexcept {
          dumpHeapProfile();
          dumpHeapProfileTimer_->scheduleTimeout(
              config->getMemoryProfilingInterval());
        });
    // Schedule an immediate timeout
    dumpHeapProfileTimer_->scheduleTimeout(0);
  }

  // Fiber task to read the LogSample from queue and publish
  addFiberTask(
      [q = std::move(logSampleQueue), config, this]() mutable noexcept {
        XLOG(INFO) << "Starting log sample updates processing fiber "
                   << "with isLogSubmissionEnable() flag: "
                   << config->isLogSubmissionEnabled();
        while (true) {
          // perform read log from the queue
          auto maybeLog = q.get();
          XLOG(DBG2) << "Received log sample update";
          if (maybeLog.hasError()) {
            XLOG(INFO) << "Terminating log sample updates processing fiber";
            break;
          }

          // validate, process and publish the event logs
          try {
            auto inputLog = maybeLog.value();
            // add common attributes
            inputLog.addString("node_name", config->getNodeName());

            // throws std::invalid_argument if not exist
            inputLog.getString("event");

            // add to recent log list
            if (recentLog_.size() >= maxLogEvents_) {
              recentLog_.pop_front();
            }
            recentLog_.emplace_back(inputLog.toJson());

            // publish the log if enable log submission
            if (config->isLogSubmissionEnabled()) {
              processEventLog(inputLog);
            }
          } catch (const std::exception& e) {
            fb303::fbData->addStatValue(
                "monitor.log.publish.failure", 1, fb303::COUNT);
            XLOG(ERR) << "Failed to publish the log. Error: "
                      << folly::exceptionStr(e);
          }
        }
      });
}

std::list<std::string>
MonitorBase::getRecentEventLogs() {
  return recentLog_;
}

void
MonitorBase::updateProcessCounters() {
  // set process.uptime.seconds counter
  const auto now = std::chrono::steady_clock::now();
  fb303::fbData->setCounter(
      "process.uptime.seconds",
      std::chrono::duration_cast<std::chrono::seconds>(now - startTime_)
          .count());

  // set process.memory.rss counter
  const auto rssMem = systemMetrics_.getRSSMemBytes();
  if (rssMem.has_value()) {
    fb303::fbData->setCounter("process.memory.rss", rssMem.value());
  }

  // set process.cpu.pct counter
  const auto cpuPct = systemMetrics_.getCPUpercentage();
  if (cpuPct.has_value()) {
    fb303::fbData->setCounter("process.cpu.pct", cpuPct.value());
    cpuPeakPct_ = std::max(cpuPeakPct_, cpuPct.value());
    fb303::fbData->setCounter("process.cpu.peak_pct", cpuPeakPct_);
  }
}

} // namespace openr
