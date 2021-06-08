/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "openr/monitor/MonitorBase.h"

namespace openr {

/**
 * Monitor class that simply print each log sample to syslog.
 */
class Monitor : public MonitorBase {
 public:
  Monitor(
      std::shared_ptr<const Config> config,
      const std::string& category,
      messaging::RQueue<LogSample> logSampleQueue)
      : MonitorBase(config, category, logSampleQueue) {}

 private:
  //  print the log sample to syslog
  void processEventLog(LogSample const& eventLog) override;

  // Get the heap profile
  void dumpHeapProfile() override;
};

} // namespace openr
