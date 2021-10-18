/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/monitor/Monitor.h"

#include <folly/logging/xlog.h>

namespace openr {

void
Monitor::processEventLog(LogSample const& eventLog) {
  // publish log message
  XLOG(INFO) << "Get a " << category_ << " event log: " << eventLog.toJson();
  // NOTE: Could add your own implementation to push logs to your database.
}

void
Monitor::dumpHeapProfile() {
  XLOG(INFO)
      << "Please add your own implementation to dump the heap memory profile.";
}

} // namespace openr
