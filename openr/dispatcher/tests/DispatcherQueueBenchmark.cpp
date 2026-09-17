/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>
#include <vector>

#include <fmt/format.h>
#include <folly/Benchmark.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

#include <openr/common/Util.h>
#include <openr/dispatcher/DispatcherQueue.h>
#include <openr/dispatcher/PublicationCoalescer.h>
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

/*
 * Saturated-backlog benchmark for push-time coalescing: the reader is never
 * drained, so this measures the cost of a push and, more importantly, the
 * resulting pending depth -- which is what bounds openr memory.
 *
 * Exports pending_messages and suppressed_messages so the memory win is
 * visible alongside the CPU cost.
 */
static void
BM_CoalescePublications(
    folly::UserCounters& counters,
    uint32_t iters,
    const bool enableCoalescing,
    const size_t numKeys,
    const size_t numAreas,
    const size_t count,
    const bool injectInitEvent) {
  folly::BenchmarkSuspender suspender;

  /*
   * Prebuild publications so their CONSTRUCTION is excluded from timing.
   * The per-push variant copy below is still inside the timed region --
   * DispatcherQueue::push takes an rvalue and copies again per reader --
   * so the absolute numbers include a publication copy. Both the On and
   * Off arms pay it identically, so the comparison remains valid.
   */
  std::vector<std::string> areas;
  areas.reserve(numAreas);
  for (size_t i = 0; i < numAreas; ++i) {
    areas.emplace_back(fmt::format("area-{}", i));
  }
  std::vector<thrift::Publication> publications;
  publications.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    publications.emplace_back(createThriftPublication(
        {{fmt::format("adj:key-{}", i % numKeys),
          createThriftValue(1, "node1", fmt::format("value-{}", i))}},
        {} /* expiredKeys */,
        std::nullopt /* nodeIds */,
        std::nullopt /* keysToUpdate */,
        areas[i % numAreas]));
  }

  size_t pendingMessages{0};
  while (iters--) {
    DispatcherQueue q;
    std::optional<messaging::StateSuppressionPolicy<KvStorePublication>>
        policy = std::nullopt;
    if (enableCoalescing) {
      policy = getKvStorePublicationSuppressionPolicy();
    }
    auto reader = q.getReader({} /* prefixes */, "benchmark", policy);

    suspender.dismiss();
    for (size_t i = 0; i < publications.size(); ++i) {
      /*
       * One event, halfway through, so there is a substantial run of
       * publications on either side of it.
       */
      if (injectInitEvent && i == publications.size() / 2) {
        q.push(KvStorePublication(thrift::InitializationEvent::KVSTORE_SYNCED));
      }
      q.push(KvStorePublication(publications[i]));
    }
    suspender.rehire();

    pendingMessages = reader.size();
    folly::doNotOptimizeAway(pendingMessages);
  }
  const size_t totalPushed = count + (injectInitEvent ? 1 : 0);
  counters["pending_messages"] = pendingMessages;
  counters["suppressed_messages"] = totalPushed - pendingMessages;
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

/*
 * Coalescing, single area: the backlog should collapse to one element whose
 * size is the unique changed-key set, regardless of how many pushes arrive.
 */
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_CoalescePublications,
    Off_Hot100_1Area_10K,
    false /* enableCoalescing */,
    100,
    1,
    10000,
    false /* injectInitEvent */);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_CoalescePublications,
    On_Hot100_1Area_10K,
    true /* enableCoalescing */,
    100,
    1,
    10000,
    false /* injectInitEvent */);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_CoalescePublications,
    Off_Unique10K_1Area_10K,
    false /* enableCoalescing */,
    10000,
    1,
    10000,
    false /* injectInitEvent */);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_CoalescePublications,
    On_Unique10K_1Area_10K,
    true /* enableCoalescing */,
    10000,
    1,
    10000,
    false /* injectInitEvent */);

/*
 * Coalescing across interleaved areas.
 */
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_CoalescePublications,
    Off_Hot100_4Areas_10K,
    false /* enableCoalescing */,
    100,
    4,
    10000,
    false /* injectInitEvent */);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_CoalescePublications,
    On_Hot100_4Areas_10K,
    true /* enableCoalescing */,
    100,
    4,
    10000,
    false /* injectInitEvent */);

/*
 * One InitializationEvent halfway through the stream. An event is never merged
 * away, so it always survives as its own element -- but whether the
 * publications on either side of it can still merge is what separates the two
 * bounding mechanisms, and shows up here as the pending depth.
 */
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_CoalescePublications,
    On_Hot100_1Area_10K_Event,
    true /* enableCoalescing */,
    100,
    1,
    10000,
    true /* injectInitEvent */);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_CoalescePublications,
    On_Hot100_4Areas_10K_Event,
    true /* enableCoalescing */,
    100,
    4,
    10000,
    true /* injectInitEvent */);

} // namespace openr

int
main(int argc, char* argv[]) {
  const folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
