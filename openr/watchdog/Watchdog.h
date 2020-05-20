/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <set>
#include <string>
#include <unordered_map>

#include <fbzmq/service/monitor/SystemMetrics.h>
#include <folly/io/async/AsyncTimeout.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/config/Config.h>

namespace openr {

class Watchdog final : public OpenrEventBase {
 public:
  explicit Watchdog(std::shared_ptr<const Config> config);

  // non-copyable
  Watchdog(Watchdog const&) = delete;
  Watchdog& operator=(Watchdog const&) = delete;

  void addEvb(OpenrEventBase* evb, const std::string& name);

  bool memoryLimitExceeded();

 private:
  void updateCounters();

  // monitor memory usage
  void monitorMemory();

  void fireCrash(const std::string& msg);

  const std::string myNodeName_;

  // Timer for checking aliveness periodically
  std::unique_ptr<folly::AsyncTimeout> watchdogTimer_{nullptr};

  // mapping of thread name to eventloop pointer
  std::unordered_map<OpenrEventBase*, std::string> monitorEvbs_;

  // thread healthcheck interval
  std::chrono::seconds interval_;

  // thread healthcheck threshold
  std::chrono::seconds threadTimeout_;

  // critcal memory threhsold
  uint32_t maxMemoryMB_{0};

  // boolean to indicate previous failure
  bool previousStatus_{true};

  // amount of time memory usage sustained above memory limit
  std::optional<std::chrono::steady_clock::time_point> memExceedTime_;

  // Get the system metrics for resource usage counters
  fbzmq::SystemMetrics systemMetrics_{};
};

} // namespace openr
