/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/format.h>
#include <folly/Benchmark.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

#include <openr/common/Util.h>
#include <openr/dispatcher/DispatcherQueue.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace openr {

// benchmark for DispatcherQueue where no filtering is done
static void
BM_NoFilterDispatcherQueue(
    uint32_t iters,
    const size_t kNumReaders,
    const size_t kNumWriters,
    const size_t kCount) {
  auto suspender = folly::BenchmarkSuspender();

  //
  // Total number of reads performed
  //
  std::atomic<size_t> totalReads{0};

  //
  // Queue under testing. We will use KvStorePublication type.
  //
  DispatcherQueue q;

  //
  // Create publication object to push to DispatcherQueue.
  //
  auto publication = createThriftPublication(
      {{"key1", createThriftValue(1, "node1", "value1")}}, {}, {}, {});

  //
  // Add reader tasks. Reader would continue to read as long as queue is open
  // NOTE: We have all readers in their own event base & thread
  //
  folly::EventBase readerEvb;
  auto& readerManager = folly::fibers::getFiberManager(readerEvb);
  for (size_t i = 0; i < kNumReaders; ++i) {
    // readers will be created with no filters
    readerManager.addTask([reader = q.getReader(), i, &totalReads]() mutable {
      size_t numReads{0};
      while (true) {
        auto maybeNum = reader.get();
        if (maybeNum.hasError()) {
          break; // Queue is closed
        }
        ++numReads;
        ++totalReads;
      }
      VLOG(1) << "Reader-" << i << " consumed " << numReads << " messages";
    });
  }

  //
  // Start reader thread. This will not return until all reader tasks are
  // completed.
  //
  std::thread readerThread([&readerEvb] { readerEvb.loop(); });

  //
  // Iterate multiple times. In each iterate we
  //
  while (iters--) {
    //
    // Add writer tasks. Each writer will write `kCount` elements. So overall
    // each reader would read `kCount * kNumWriters` elements. Aka each reader
    // would read every element written by every reader.
    //
    folly::EventBase writerEvb;
    auto& writerManager = folly::fibers::getFiberManager(writerEvb);
    for (size_t i = 0; i < kNumWriters; ++i) {
      writerManager.addTask([&q, kCount, i, publication]() {
        for (size_t m = 0; m < kCount; ++m) {
          q.push(publication);
        }
        VLOG(1) << "Writer-" << i << " finished writing " << kCount
                << " messages.";
      });
    }

    //
    // Run writer-loop & wait until reader reads everything
    //
    const size_t expectedReads = kCount * kNumWriters * kNumReaders;
    totalReads = 0;
    suspender.dismiss();
    writerEvb.loop(); // Publish all writes
    while (totalReads != expectedReads) {
      std::this_thread::yield();
    }
    suspender.rehire();
  } // while

  //
  // Close queue & wait for all readers to terminate
  //
  q.close();
  readerThread.join();
}

// benchmark for DispatcherQueue where a prefix is provided to filter the keys
// in the KvStorePublication
static void
BM_FilterDispatcherQueue(
    uint32_t iters,
    const size_t kNumReaders,
    const size_t kNumWriters,
    const size_t kCount) {
  auto suspender = folly::BenchmarkSuspender();

  //
  // Total number of reads performed
  //
  std::atomic<size_t> totalReads{0};

  //
  // Queue under testing. We will use KvStorePublication type.
  //
  DispatcherQueue q;

  //
  // Create publication object to push to DispatcherQueue.
  //
  auto publication = createThriftPublication(
      {{"key1", createThriftValue(1, "node1", "value1")},
       {"key-1", createThriftValue(2, "node-1", "value-1")}},
      {},
      {},
      {});

  //
  // Add reader tasks. Reader would continue to read as long as queue is open
  // NOTE: We have all readers in their own event base & thread
  //
  folly::EventBase readerEvb;
  auto& readerManager = folly::fibers::getFiberManager(readerEvb);
  for (size_t i = 0; i < kNumReaders; ++i) {
    // readers will be created with a filter
    readerManager.addTask(
        [reader = q.getReader({"key-1"}), i, &totalReads]() mutable {
          size_t numReads{0};
          while (true) {
            auto maybeNum = reader.get();
            if (maybeNum.hasError()) {
              break; // Queue is closed
            }
            ++numReads;
            ++totalReads;
          }
          VLOG(1) << "Reader-" << i << " consumed " << numReads << " messages";
        });
  }

  //
  // Start reader thread. This will not return until all reader tasks are
  // completed.
  //
  std::thread readerThread([&readerEvb] { readerEvb.loop(); });

  //
  // Iterate multiple times. In each iterate we
  //
  while (iters--) {
    //
    // Add writer tasks. Each writer will write `kCount` elements. So overall
    // each reader would read `kCount * kNumWriters` elements. Aka each reader
    // would read every element written by every reader.
    //
    folly::EventBase writerEvb;
    auto& writerManager = folly::fibers::getFiberManager(writerEvb);
    for (size_t i = 0; i < kNumWriters; ++i) {
      writerManager.addTask([&q, kCount, i, publication]() {
        for (size_t m = 0; m < kCount; ++m) {
          q.push(publication);
        }
        VLOG(1) << "Writer-" << i << " finished writing " << kCount
                << " messages.";
      });
    }

    //
    // Run writer-loop & wait until reader reads everything
    //
    const size_t expectedReads = kCount * kNumWriters * kNumReaders;
    totalReads = 0;
    suspender.dismiss();
    writerEvb.loop(); // Publish all writes
    while (totalReads != expectedReads) {
      std::this_thread::yield();
    }
    suspender.rehire();
  } // while

  //
  // Close queue & wait for all readers to terminate
  //
  q.close();
  readerThread.join();
}

// benchmark testing for DispatcherQueue with no specified filter
BENCHMARK_NAMED_PARAM(
    BM_NoFilterDispatcherQueue, M1000000_R1_W1, 1, 1, 1000000);
BENCHMARK_NAMED_PARAM(
    BM_NoFilterDispatcherQueue, M1000000_R10_W1, 10, 1, 1000000);
BENCHMARK_NAMED_PARAM(
    BM_NoFilterDispatcherQueue, M1000000_R100_W1, 100, 1, 1000000);
BENCHMARK_NAMED_PARAM(
    BM_NoFilterDispatcherQueue, M1000000_R1_W10, 1, 10, 100000);
BENCHMARK_NAMED_PARAM(
    BM_NoFilterDispatcherQueue, M1000000_R1_W100, 1, 100, 10000);

// benchmark testing for DispatcherQueue with filter set
BENCHMARK_NAMED_PARAM(BM_FilterDispatcherQueue, M1000000_R1_W1, 1, 1, 1000000);
BENCHMARK_NAMED_PARAM(
    BM_FilterDispatcherQueue, M1000000_R10_W1, 10, 1, 1000000);
BENCHMARK_NAMED_PARAM(
    BM_FilterDispatcherQueue, M1000000_R100_W1, 100, 1, 1000000);
BENCHMARK_NAMED_PARAM(BM_FilterDispatcherQueue, M1000000_R1_W10, 1, 10, 100000);
BENCHMARK_NAMED_PARAM(
    BM_FilterDispatcherQueue, M1000000_R1_W100, 1, 100, 10000);

} // namespace openr

int
main(int argc, char* argv[]) {
  const folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
