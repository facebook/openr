// Copyright 2004-present Facebook. All Rights Reserved.

#include "openr/monitor/Monitor.h"

namespace openr {

void
Monitor::processEventLog(LogSample const& eventLog) {
  // publish log message
  LOG(INFO) << "Get a " << category_ << " event log: " << eventLog.toJson();
  // NOTE: Could add your own implementation to push logs to your database.
}

} // namespace openr
