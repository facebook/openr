/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/io/async/AsyncTimeout.h>
#include <openr/common/Constants.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/config/Config.h>
#include <openr/monitor/SystemMetrics.h>
#include "openr/messaging/ReplicateQueue.h"

namespace openr {

class Watchdog final : public OpenrEventBase {
 public:
  explicit Watchdog(std::shared_ptr<const Config> config);

  // non-copyable
  Watchdog(Watchdog const&) = delete;
  Watchdog& operator=(Watchdog const&) = delete;

  void addEvb(OpenrEventBase* evb);

  bool memoryLimitExceeded();

  /**
   * Register a queue with watchdog to be monitored and logged
   */
  void addQueue(messaging::ReplicateQueueBase& q, const std::string& qName);

 private:
  // monitor thread status in case they get stuck
  void monitorThreadStatus();

  // monitor memory usage
  void monitorMemory();

  // update per-eventbase related counters
  void updateThreadCounters();

  // update counters for each ReplicatedQueue and its internal RWQueues
  void updateQueueCounters();

  // force to abort, aka, crash process
  void fireCrash(const std::string& msg);

  //
  // Private vars for internal state
  //
  const std::string myNodeName_;

  // Timer for checking aliveness periodically
  std::unique_ptr<folly::AsyncTimeout> watchdogTimer_{nullptr};

  // Eventbase raw pointers
  folly::F14NodeSet<OpenrEventBase*> monitorEvbs_;

  // thread healthcheck interval
  std::chrono::seconds interval_;

  // thread healthcheck threshold
  std::chrono::seconds threadTimeout_;

  // critcal memory threhsold
  uint32_t maxMemoryMB_{0};

  // boolean to indicate previous failure
  bool isDeadThreadDetected_{false};

  // amount of time memory usage sustained above memory limit
  std::optional<std::chrono::steady_clock::time_point> memExceedTime_;

  // Get the system metrics for resource usage counters
  SystemMetrics systemMetrics_{};

  // Vector of queues to monitor for drainage
  folly::F14NodeMap<
      std::string,
      std::reference_wrapper<messaging::ReplicateQueueBase>>
      monitoredQs_;
};

} // namespace openr
