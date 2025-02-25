/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/mem-leak/MemLeak.h>
#include <chrono>
#include <thread>
#include "folly/logging/xlog.h"

/**
 * @brief Create a thread that leaks memory.
 *
 * Start a new thread that allocates 100 MB of memory every 10
 * seconds, causing a memory leak. The allocated memory is not freed,
 * resulting in a continuous increase in memory usage.
 */
void
startMemoryLeakThread() {
  /* sleep override */
  std::thread([]() {
    XLOG(INFO) << "Starting memory leak thread...";
    uint64_t loopCount = 0;
    while (true) {
      loopCount++;
      XLOG(INFO) << "Memory leak thread loop count: " << loopCount;
      // Allocate 100 MB of memory using new
      char* leak = new char[100 * 1024 * 1024];
      // Use the allocated memory to ensure it is committed
      std::fill(leak, leak + (100 * 1024 * 1024), 0);
      XLOG(INFO) << "Address of leak variable: " << static_cast<void*>(leak);
      // Sleep for 10 seconds
      /* sleep override */
      std::this_thread::sleep_for(std::chrono::seconds(10));
      // Note: Memory is not freed, causing a leak
    }
    /* sleep override */
  }).detach(); // Detach the thread to run independently
}
