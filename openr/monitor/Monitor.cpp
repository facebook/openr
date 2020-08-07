/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/monitor/Monitor.h"

namespace openr {

void
Monitor::processEventLog(LogSample const& eventLog) {
  // publish log message
  LOG(INFO) << "Get a " << category_ << " event log: " << eventLog.toJson();
  // NOTE: Could add your own implementation to push logs to your database.
}

} // namespace openr
