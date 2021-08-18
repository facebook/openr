/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/messaging/ReplicateQueue.h"
#include <gtest/gtest.h>

#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

#include <openr/messaging/ReplicateQueue.h>

using namespace openr::messaging;

TEST(ReplicateQueueTest, Test) {
  const size_t kNumReaders{16};
  const size_t kTotalWrites{4096};

  ReplicateQueue<int> q;
  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  // Add readers
  std::atomic<size_t> totalReads{0};
  std::atomic<size_t> replicatedReads{0};
  for (size_t i = 0; i < kNumReaders; ++i) {
    manager.addTask([reader = q.getReader(),
                     &q,
                     &totalReads,
                     &replicatedReads,
                     i,
                     kTotalWrites]() mutable {
      size_t numReads{0};
      while (true) {
        VLOG(1) << "Reader" << i << " attempting a read";
        auto maybeNum = reader.get();
        if (maybeNum.hasError()) {
          EXPECT_EQ(maybeNum.error(), QueueError::QUEUE_CLOSED);
          break;
        }
        VLOG(1) << "Reader" << i << " got " << maybeNum.value();
        ++numReads;
        ++totalReads;
        if (totalReads == kTotalWrites * kNumReaders) {
          // Before closing the replicated queue (and hence internal RWQueues),
          // get replication stats and verify that replication reads match the
          // overall reads we expect
          std::vector<RWQueueStats> stats = q.getReplicationStats();
          for (auto& stat : stats) {
            replicatedReads += stat.reads;
          }
          EXPECT_EQ(totalReads, replicatedReads);

          LOG(INFO) << "Closing queue";
          q.close();
        }
      }
      EXPECT_EQ(kTotalWrites, numReads);
      EXPECT_EQ(0, reader.size());
      LOG(INFO) << "Reader" << i << " read " << numReads << " messages.";
    });
  }

  // Add writer task
  std::atomic<size_t> totalWrites{0};
  std::atomic<size_t> replicationWrites{0};
  manager.addTask([&q, &totalWrites, &replicationWrites]() {
    for (size_t i = 0; i < kTotalWrites; ++i) {
      q.push(i);
      ++totalWrites;
    }
    LOG(INFO) << "Writer finished pushing " << kTotalWrites << " messages.";

    // Get replicated queue stats
    std::vector<RWQueueStats> stats = q.getReplicationStats();
    for (auto& stat : stats) {
      replicationWrites += stat.writes;
    }
  });

  EXPECT_EQ(kNumReaders, q.getNumReaders()); // All readers should be active
  evb.loop();
  EXPECT_EQ(0, q.getNumReaders()); // All readers should have died

  EXPECT_EQ(kTotalWrites * kNumReaders, totalReads);
  EXPECT_EQ(totalWrites, q.getNumWrites());
  EXPECT_EQ(kTotalWrites * kNumReaders, replicationWrites);
}

TEST(ReplicateQueueTest, OpenQueueTest) {
  ReplicateQueue<int> q;
  auto r1 = q.getReader("r1");
  EXPECT_EQ(1, q.getNumReaders());

  q.close();
  EXPECT_EQ(0, q.getNumReaders());

  // make sure can't add reader when ReplicateQueue is closed
  EXPECT_THROW(q.getReader(), std::runtime_error);

  // reopen queue and make sure reader can be added
  q.open();
  auto r2 = q.getReader("r2");
  EXPECT_EQ(1, q.getNumReaders());

  q.close();
}
