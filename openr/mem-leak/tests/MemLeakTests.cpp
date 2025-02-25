/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <sys/resource.h>
#include <chrono>
#include <thread>
#include "folly/logging/xlog.h"
#include "openr/mem-leak/MemLeak.h"

/**
 * Test case to verify that the memory leak thread can be started and
 * that it causes anticipated increase in process's memory usage.
 */
TEST(MemLeakTest, StartMemoryLeakThread) {
  // Start the memory leak thread
  startMemoryLeakThread();

  // Wait for 30 seconds to allow the thread to run for a few iterations
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(30));

  // Check if the process memory usage has increased significantly
  // Note: This is a simple check and may not be accurate in all cases
  // Get the process memory usage
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  auto memUsage = ru.ru_maxrss * 1024; // Convert from kilobytes to bytes
  EXPECT_GT(memUsage, 300 * 1024 * 1024); // Expect at least 300 MB of memory
  // usage
  XLOG(INFO) << "Process memory usage: " << memUsage << " bytes";
}

int
main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
