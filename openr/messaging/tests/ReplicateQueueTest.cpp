// Copyright 2004-present Facebook. All Rights Reserved.

#include <gtest/gtest.h>

#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

#include <openr/messaging/ReplicateQueue.h>

using namespace ::testing;
using namespace openr::messaging;

TEST(ReplicateQueueTest, Test) {
  const size_t kNumReaders{16};
  const size_t kTotalWrites{4096};

  ReplicateQueue<int> q;
  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  // Add readers
  std::atomic<size_t> totalReads{0};
  for (int i = 0; i < kNumReaders; ++i) {
    manager.addTask(
        [reader = q.getReader(), &q, &totalReads, i, kTotalWrites]() mutable {
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
  manager.addTask([&q]() {
    for (int i = 0; i < kTotalWrites; ++i) {
      q.push(i);
    }
    LOG(INFO) << "Writer finished pushing " << kTotalWrites << " messages.";
  });

  EXPECT_EQ(kNumReaders, q.getNumReaders()); // All readers should be active
  evb.loop();
  EXPECT_EQ(0, q.getNumReaders()); // All readers should have died

  EXPECT_EQ(kTotalWrites * kNumReaders, totalReads);
}
