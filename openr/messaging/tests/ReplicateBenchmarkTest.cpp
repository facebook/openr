/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include <openr/tests/benchmark/OpenrBenchmarkBase.h>

#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

#include <openr/messaging/ReplicateQueue.h>

using openr::messaging::ReplicateQueue;

namespace openr {

class QueueBenchmarkTestFixture : public OpenrBenchmarkBase {
 public:
  explicit QueueBenchmarkTestFixture() {
    // nothing to do
  }
  ~QueueBenchmarkTestFixture() {
    // nothing to do
  }

  std::vector<thrift::Publication>
  getPublications(const size_t kTotalWrites) {
    std::vector<thrift::Publication> allPublications;

    for (size_t i = 0; i < kTotalWrites; ++i) {
      const auto adj12 = createAdjacency(
          "2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002 + i);
      const auto adj21 = createAdjacency(
          "1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001 + i);
      const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
      const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");

      auto publication = createThriftPublication(
          {{"adj:1", createAdjValue("1", 1, {adj12}, false, 1)},
           {"adj:2", createAdjValue("2", 1, {adj21}, false, 2)},
           createPrefixKeyValue("1", 1, addr1),
           createPrefixKeyValue("2", 1, addr2)},
          {},
          {},
          {},
          std::string(""));
      allPublications.push_back(publication);
    }

    return allPublications;
  }
};

static void
BM_ReplicateQueueTest(
    uint32_t iters,
    const size_t kNumReaders,
    const size_t kNumWriters,
    const size_t kTotalWrites) {
  auto suspender = folly::BenchmarkSuspender();

  auto queueBenchmarkTestFixture =
      std::make_unique<QueueBenchmarkTestFixture>();

  ReplicateQueue<thrift::Publication> q;
  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);
  std::atomic<size_t> totalReads{0};

  auto pubs = queueBenchmarkTestFixture->getPublications(kTotalWrites);

  for (size_t k = 0; k < iters; k++) {
    for (size_t i = 0; i < kNumReaders; ++i) {
      manager.addTask([reader = q.getReader(),
                       &q,
                       &totalReads,
                       kTotalWrites = kTotalWrites,
                       kNumReaders = kNumReaders]() mutable {
        size_t numReads{0};
        while (true) {
          auto maybeNum = reader.get();
          if (maybeNum.hasError()) {
            break;
          }
          ++numReads;
          ++totalReads;
          if (totalReads == kTotalWrites * kNumReaders) {
            LOG(INFO) << "Closing queue";
            q.close();
          }
        }
      });
    }

    // Add writer task
    for (size_t i = 0; i < kNumWriters; ++i) {
      manager.addTask([&q, kTotalWrites, &pubs, i, kNumWriters]() {
        for (size_t j = i * kTotalWrites / kNumWriters;
             j < (i + 1) * kTotalWrites / kNumWriters;
             j++) {
          q.push(std::move(pubs.at(j)));
        }
        LOG(INFO) << "Writer" << i << " finished pushing "
                  << kTotalWrites / kNumWriters << " messages.";
      });
    }
  }

  suspender.dismiss(); // Start measuring benchmark time
  evb.loop();
  suspender.rehire(); // Stop measuring time again
}

// The first integer parameter is number of readers
// The second integer parameter is the number of writers
// The third interger parameter is the number of messages
BENCHMARK_NAMED_PARAM(BM_ReplicateQueueTest, m100000_r1_w1, 1, 1, 100000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueueTest, m100000_r10_w1, 10, 1, 100000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueueTest, m100000_r50_w1, 50, 1, 100000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueueTest, m100000_r100_w1, 100, 1, 100000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueueTest, m100000_r10_w10, 10, 10, 100000);
BENCHMARK_NAMED_PARAM(BM_ReplicateQueueTest, m100000_r50_w10, 50, 10, 100000);
} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
