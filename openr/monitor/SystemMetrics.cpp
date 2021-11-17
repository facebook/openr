/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/monitor/SystemMetrics.h"

#include <folly/logging/xlog.h>

namespace openr {

namespace {

/* Return memory the process currently used from /proc/[pid]/status.
 / The /proc is a pseudo-filesystem providing an API to kernel data
 / structures.
*/
std::optional<size_t>
getMemBytes(const std::string& memoryType) {
  std::optional<size_t> rss;
  // match the line like: "VmRSS:      9028 kB"
  // match the line like: "VmSize:      10036 kB"
  // std::string regexString("VmRSS:\\s+(\\d+)\\s+(\\w+)");
  auto regexString = fmt::format("{}:\\s+(\\d+)\\s+(\\w+)", memoryType);
  re2::RE2 regex{regexString};
  std::string rssMatched;
  std::string line;
  try {
    // "/proc/self/" allows a process to look at itself without knowing the PID.
    std::ifstream input("/proc/self/status");
    if (input.is_open()) {
      while (std::getline(input, line)) {
        bool result = re2::RE2::Extract(line, regex, "\\1", &rssMatched);
        if (result) {
          rss = std::stoull(rssMatched) * 1024;
          break;
        }
      }
    }
  } catch (const std::exception& ex) {
    XLOG(ERR)
        << "Fail to read the \"/proc/self/status\" of current process to get the memory usage: "
        << ex.what();
  }
  return rss;
}
} // namespace

// Return RSS memory the process currently used
std::optional<size_t>
SystemMetrics::getRSSMemBytes() {
  return getMemBytes("VmRSS");
}

// Return virtual memory the process currently used
std::optional<size_t>
SystemMetrics::getVirtualMemBytes() {
  return getMemBytes("VmSize");
}

/* Return CPU% the process used
 / This need to be called twice to get the time difference
 / and calculate the CPU%.
 /
 / It will return folly::none when:
 /    1. first time query
 /    2. get invalid time:
 /        - previous timestamp > current timestamp
 /        - preivous total used time > current total used time
*/
std::optional<double>
SystemMetrics::getCPUpercentage() {
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);

  ProcCpuTime nowCpuTime(usage);
  std::optional<double> cpuPct;

  // calculate the CPU% = (process time diff) / (time elapsed) * 100
  if (prevCpuTime.timestamp != 0 && // has cached before
      nowCpuTime.timestamp > prevCpuTime.timestamp &&
      nowCpuTime.totalTime > prevCpuTime.totalTime) {
    uint64_t timestampDiff = nowCpuTime.timestamp - prevCpuTime.timestamp;
    uint64_t procTimeDiff = nowCpuTime.totalTime - prevCpuTime.totalTime;
    cpuPct = ((double)procTimeDiff / (double)timestampDiff) * 100;
  }

  // update the cache for next CPU% update
  prevCpuTime = nowCpuTime;

  return cpuPct;
}

// get current timestamp
uint64_t
SystemMetrics::getCurrentNanoTime() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace openr
