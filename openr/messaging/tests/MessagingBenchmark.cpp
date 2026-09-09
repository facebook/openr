/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>
#include <vector>

#include <folly/Benchmark.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>

#include <openr/messaging/Queue.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

namespace {

struct StateUpdate {
  std::string key;
  size_t value;
  bool barrier;
};

messaging::StateSuppressionKey
getStateSuppressionKey(const StateUpdate& update) {
  return messaging::StateSuppressionKey{
      update.key,
      update.barrier ? messaging::StateSuppressionAction::KEY_BARRIER
                     : messaging::StateSuppressionAction::REPLACE_PENDING};
}

} // namespace

static void
BM_RWQueue(
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
  // Queue under testing. We use primitive type. This is good enough for us to
  // measure the performance overhead of messaging queue.
  //
  messaging::RWQueue<size_t> q;

  //
  // Add reader tasks. Reader would continue to read as long as queue is open
  // NOTE: We have all readers in their own event base & thread
  //
  folly::EventBase readerEvb;
  auto& readerManager = folly::fibers::getFiberManager(readerEvb);
  for (size_t i = 0; i < kNumReaders; ++i) {
    readerManager.addTask([&q, i, &totalReads]() mutable {
      size_t numReads{0};
      while (true) {
        auto maybeNum = q.get();
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
      writerManager.addTask([&q, kCount, i]() {
        for (size_t m = 0; m < kCount; ++m) {
          q.push(m);
        }
        VLOG(1) << "Writer-" << i << " finished writing " << kCount
                << " messages.";
      });
    }

    //
    // Run writer-loop & wait until reader reads everything
    //
    const size_t expectedReads = kCount * kNumWriters;
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

static void
BM_ReplicateQueue(
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
  // Queue under testing. We use primitive type. This is good enough for us to
  // measure the performance overhead of messaging queue.
  //
  messaging::ReplicateQueue<size_t> q;

  //
  // Add reader tasks. Reader would continue to read as long as queue is open
  // NOTE: We have all readers in their own event base & thread
  //
  folly::EventBase readerEvb;
  auto& readerManager = folly::fibers::getFiberManager(readerEvb);
  for (size_t i = 0; i < kNumReaders; ++i) {
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
      writerManager.addTask([&q, kCount, i]() {
        for (size_t m = 0; m < kCount; ++m) {
          q.push(m);
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

static void
BM_StatePush(
    folly::UserCounters& counters,
    uint32_t iters,
    const bool enableQueueCoalescing,
    const size_t activationThreshold,
    const size_t numKeys,
    const size_t barrierInterval,
    const size_t count) {
  folly::BenchmarkSuspender suspender;

  std::vector<std::string> keys;
  keys.reserve(numKeys);
  for (size_t i = 0; i < numKeys; ++i) {
    keys.emplace_back("area:key-" + std::to_string(i));
  }
  std::vector<StateUpdate> updates;
  updates.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    updates.push_back(
        StateUpdate{
            keys[i % numKeys],
            i,
            barrierInterval != 0 && (i + 1) % barrierInterval == 0});
  }

  size_t pendingMessages{0};
  while (iters--) {
    messaging::ReplicateQueue<StateUpdate> queue;
    auto reader = enableQueueCoalescing
        ? queue.getReader(
              "benchmark",
              messaging::StateSuppressionPolicy<StateUpdate>{
                  getStateSuppressionKey, activationThreshold})
        : queue.getReader("benchmark");

    suspender.dismiss();
    for (const auto& update : updates) {
      queue.push(update);
    }
    suspender.rehire();

    pendingMessages = reader.size();
    folly::doNotOptimizeAway(pendingMessages);
  }
  counters["pending_messages"] = pendingMessages;
  counters["suppressed_messages"] = count - pendingMessages;
}

/**
 * The first parameter is number of readers
 * The second parameter is the number of writers
 * The third parameter is the number of messages each writer sends
 *
 * In our benchmark we intends to keep Number of Messages Written same. So when
 * we increase writers, we reduce number of messages per writer (third param)
 */

BENCHMARK_NAMED_PARAM(BM_RWQueue, M1000000_R1_W1, 1, 1, 1000000);
BENCHMARK_NAMED_PARAM(BM_RWQueue, M1000000_R10_W1, 10, 1, 1000000);
BENCHMARK_NAMED_PARAM(BM_RWQueue, M1000000_R100_W1, 100, 1, 1000000);
BENCHMARK_NAMED_PARAM(BM_RWQueue, M1000000_R1000_W1, 1000, 1, 1000000);
BENCHMARK_NAMED_PARAM(BM_RWQueue, M1000000_R1_W10, 1, 10, 100000);
BENCHMARK_NAMED_PARAM(BM_RWQueue, M1000000_R1_W100, 1, 100, 10000);
BENCHMARK_NAMED_PARAM(BM_RWQueue, M1000000_R1_W1000, 1, 1000, 1000);

BENCHMARK_NAMED_PARAM(BM_ReplicateQueue, M1000000_R1_W1, 1, 1, 1000000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueue, M1000000_R10_W1, 10, 1, 1000000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueue, M1000000_R100_W1, 100, 1, 1000000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueue, M1000000_R1_W10, 1, 10, 100000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueue, M1000000_R1_W100, 1, 100, 10000);

BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, Off_Hot100_10K, false, 0, 100, 0, 10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, On_Hot100_10K, true, 0, 100, 0, 10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, Adaptive_Hot100_10K, true, 64, 100, 0, 10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, Off_Keys50_Writes50, false, 0, 50, 0, 50);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, On_Keys50_Writes50, true, 0, 50, 0, 50);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, Adaptive_Keys50_Writes50, true, 64, 50, 0, 50);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, Off_Unique_10K, false, 0, 10000, 0, 10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, On_Unique_10K, true, 0, 10000, 0, 10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, Adaptive_Unique_10K, true, 64, 10000, 0, 10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, Off_Barrier97_10K, false, 0, 100, 97, 10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, On_Barrier97_10K, true, 0, 100, 97, 10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_StatePush, Adaptive_Barrier97_10K, true, 64, 100, 97, 10000);

} // namespace openr

int
main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
