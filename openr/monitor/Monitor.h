// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "openr/monitor/MonitorBase.h"

const size_t kMaxLogEvents{100};

namespace openr {

/**
 * Monitor class that simply print each log sample to syslog.
 */
class Monitor : public MonitorBase {
 public:
  Monitor(
      std::shared_ptr<const Config> config,
      const std::string& category,
      messaging::ReplicateQueue<LogSample>& eventLogUpdatesQueue,
      const size_t maxLogEvents = kMaxLogEvents)
      : MonitorBase(config, category, eventLogUpdatesQueue, maxLogEvents) {}

 private:
  //  print the log sample to syslog
  void processEventLog(LogSample const& eventLog) override;
};

} // namespace openr
