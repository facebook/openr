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

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/resource-monitor/ResourceMonitor.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/Types.h>

namespace openr {

class Watchdog final : public fbzmq::ZmqEventLoop {
 public:
  Watchdog(
      std::string const& myNodeName,
      std::chrono::seconds healthCheckInterval,
      std::chrono::seconds healthCheckThreshold,
      uint32_t critialMemoryMB);

  ~Watchdog() override = default;

  // non-copyable
  Watchdog(Watchdog const&) = delete;
  Watchdog& operator=(Watchdog const&) = delete;

  void addEvl(ZmqEventLoop* evl, const std::string& name);

  void delEvl(ZmqEventLoop* evl);

  bool memoryLimitExceeded() const;

 private:

  void updateCounters();

  // monitor memory usage
  void monitorMemory();

  void fireCrash(const std::string &msg);

  const std::string myNodeName_;

  // Timer for checking aliveness periodically
  std::unique_ptr<fbzmq::ZmqTimeout> watchdogTimer_{nullptr};

  // mapping of thread name to eventloop pointer
  std::unordered_map<ZmqEventLoop*, std::string> allEvls_;

  // thread healthcheck interval
  const std::chrono::seconds healthCheckInterval_;

  // thread healthcheck threshold
  const std::chrono::seconds healthCheckThreshold_;

  // boolean to indicate previous failure
  bool previousStatus_{true};

  // critcal memory threhsold
  uint32_t criticalMemoryMB_{0};

  // amount of time memory usage sustained above memory limit
  folly::Optional<std::chrono::steady_clock::time_point> memExceedTime_ =
      folly::none;

  // resource monitor
  fbzmq::ResourceMonitor resourceMonitor_{};
};

} // namespace openr
