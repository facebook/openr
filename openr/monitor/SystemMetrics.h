/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <glog/logging.h>
#include <re2/re2.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <chrono>
#include <fstream>

namespace openr {

/**
 * This class provides the API to get the system usage for monitoring,
 * including the CPU, memory usage, etc.
 */
class SystemMetrics {
 public:
  // get RSS memory the process used, aka, memory is allocated to the process in
  // RAM.
  std::optional<size_t> getRSSMemBytes();

  // get virtual memory the process used, aka, all memory that the process can
  // access, including memory in RAM and swapped out, memory that is allocated
  // but not used, and memory that is from shared libraries
  std::optional<size_t> getVirtualMemBytes();

  // get CPU% the process used
  std::optional<double> getCPUpercentage();

 private:
  /**
  / To record CPU used time of current process (in nanoseconds)
  */
  using ProcCpuTime = struct ProcCpuTime {
    uint64_t userTime = 0; /* CPU time used in user mode */
    uint64_t sysTime = 0; /*  CPU time used in system mode*/
    uint64_t totalTime = 0; /* total CPU time used */
    uint64_t timestamp = 0; /* timestamp for current record */
    ProcCpuTime() {} // for initializing the prevCpuTime
    explicit ProcCpuTime(struct rusage& usage)
        : userTime(
              usage.ru_utime.tv_sec * 1.0e9 + usage.ru_utime.tv_usec * 1.0e3),
          sysTime(
              usage.ru_stime.tv_sec * 1.0e9 + usage.ru_stime.tv_usec * 1.0e3),
          totalTime(userTime + sysTime),
          timestamp(getCurrentNanoTime()) {}
  };

  // cache for CPU used time of previous query
  ProcCpuTime prevCpuTime;

  // get current timestamp (in nanoseconds)
  uint64_t static getCurrentNanoTime();
};

} // namespace openr
