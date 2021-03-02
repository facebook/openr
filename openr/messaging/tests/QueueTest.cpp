/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/executors/ManualExecutor.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

#include <openr/messaging/Queue.h>

using namespace openr::messaging;

TEST(RWQueueTest, SizeAndReaders) {
  RWQueue<int> q;

  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());

  q.push(1);
  q.push(2);

  EXPECT_EQ(2, q.size());
  EXPECT_EQ(0, q.numPendingReads());

  EXPECT_EQ(1, q.get().value());
  EXPECT_EQ(2, q.get().value());

  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());

  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);
  manager.addTask([&q]() mutable { EXPECT_EQ(1, q.get().value()); });
  manager.addTask([&q]() mutable { EXPECT_EQ(2, q.get().value()); });

  evb.loopOnce();
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(2, q.numPendingReads());

  q.push(1);
  q.push(2);
  evb.loopOnce();
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());
}

TEST(RWQueueTest, OrderedPushGet) {
  RWQueue<std::string> q;

  q.push(std::string("one"));
  q.push(std::string("two"));
  q.push(std::string("three"));

  EXPECT_EQ(3, q.size());
  EXPECT_EQ("one", q.get().value());
  EXPECT_EQ(2, q.size());
  EXPECT_EQ("two", q.get().value());
  EXPECT_EQ(1, q.size());
  EXPECT_EQ("three", q.get().value());
  EXPECT_EQ(0, q.size());
}

TEST(RWQueueTest, ClosedPendingReads) {
  RWQueue<int> q;

  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);
  manager.addTask([&q]() mutable {
    EXPECT_FALSE(q.isClosed());
    auto x = q.get(); // Perform read
    EXPECT_TRUE(q.isClosed());
    EXPECT_TRUE(x.hasError());
    EXPECT_EQ(x.error(), QueueError::QUEUE_CLOSED);
  });

  evb.loopOnce(); // Fiber should get stuck at the read
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(1, q.numPendingReads());

  q.close();
  evb.loopOnce();
  EXPECT_TRUE(q.isClosed());
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());
  EXPECT_EQ(q.get().error(), QueueError::QUEUE_CLOSED);
}

TEST(RWQueueTest, ClosedPendingData) {
  RWQueue<int> q;

  q.push(1);
  q.push(2);
  EXPECT_EQ(2, q.size());
  EXPECT_EQ(0, q.numPendingReads());

  q.close();
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());
}

TEST(RWQueueTest, ClosedReads) {
  RWQueue<double> q;
  q.close();
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());

  EXPECT_EQ(q.get().error(), QueueError::QUEUE_CLOSED);

  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads()); // Request doesn't gets queued in
}

TEST(RWQueueTest, MultipleReadersWriters) {
  const size_t kNumReaders{16};
  const size_t kNumWriters{16};
  const size_t kCountPerWriter{128};
  RWQueue<size_t> q;
  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  // Add reader task
  std::atomic<size_t> totalReads{0};
  for (size_t i = 0; i < kNumReaders; ++i) {
    manager.addTask([&q, &totalReads, i]() {
      size_t numReads{0};
      while (true) {
        VLOG(1) << "Reader" << i << " attempting a read";
        auto maybeNum = q.get();
        if (maybeNum.hasError()) {
          EXPECT_EQ(QueueError::QUEUE_CLOSED, maybeNum.error());
          LOG(INFO) << "Reader" << i << " received queue closed error.";
          break;
        }
        VLOG(1) << "Reader" << i << " got " << maybeNum.value();
        ++numReads;
        ++totalReads;
        if (totalReads == kNumWriters * kCountPerWriter) {
          LOG(INFO) << "Closing queue";
          q.close();
        }
      }
      LOG(INFO) << "Reader" << i << " read " << numReads << " messages.";
    });
  }

  // Add writer task
  for (size_t i = 0; i < kNumWriters; ++i) {
    manager.addTask([&q, i]() {
      for (size_t j = 0; j < kCountPerWriter; ++j) {
        const size_t num = i * kCountPerWriter + j;
        VLOG(1) << "Writer" << i << " sending " << num;
        q.push(num);
      }
      LOG(INFO) << "Writer" << i << " finished pushing " << kCountPerWriter
                << " messages.";
    });
  }

  evb.loop();
}

TEST(RWQueueTest, MultiThreadTest) {
  const size_t kNumReaders{16};
  const size_t kNumWriters{16};
  const size_t kCountPerWriter{8192};
  ASSERT_EQ(kNumReaders, kNumWriters);
  RWQueue<size_t> q;
  std::vector<std::unique_ptr<folly::EventBase>> evbs;

  // Add reader task
  std::atomic<size_t> totalReads{0};
  for (size_t i = 0; i < kNumReaders; ++i) {
    evbs.emplace_back(std::make_unique<folly::EventBase>());
    folly::fibers::getFiberManager(*evbs.back())
        .addTask([&q, &totalReads, i]() {
          size_t numReads{0};
          while (true) {
            VLOG(1) << "Reader" << i << " attempting a read";
            auto maybeNum = q.get();
            if (maybeNum.hasError()) {
              EXPECT_EQ(QueueError::QUEUE_CLOSED, maybeNum.error());
              LOG(INFO) << "Reader" << i << " received queue closed error.";
              break;
            }
            VLOG(1) << "Reader" << i << " got " << maybeNum.value();
            ++numReads;
            ++totalReads;
            if (totalReads == kNumWriters * kCountPerWriter) {
              LOG(INFO) << "Closing queue";
              q.close();
            }
          }
          EXPECT_LE(1, numReads);
          LOG(INFO) << "Reader" << i << " read " << numReads << " messages.";
        });
  }

  // Add writer task
  for (size_t i = 0; i < kNumWriters; ++i) {
    evbs.emplace_back(std::make_unique<folly::EventBase>());
    folly::fibers::getFiberManager(*evbs.back()).addTask([&q, i]() {
      for (size_t j = 0; j < kCountPerWriter; ++j) {
        const size_t num = i * kCountPerWriter + j;
        VLOG(1) << "Writer" << i << " sending " << num;
        q.push(num);
      }
      LOG(INFO) << "Writer" << i << " finished pushing " << kCountPerWriter
                << " messages.";
    });
  }

  std::vector<std::thread> evbThreads;
  for (auto& evb : evbs) {
    evbThreads.emplace_back([evbPtr = evb.get()]() { evbPtr->loop(); });
  }
  for (auto& evbThread : evbThreads) {
    evbThread.join();
  }

  EXPECT_EQ(kNumWriters * kCountPerWriter, totalReads);
}

#if FOLLY_HAS_COROUTINES
TEST(RWQueueTest, CoroTest) {
  const size_t kNumReaders{16};
  const size_t kNumWriters{16};
  const size_t kCountPerWriter{8192};
  ASSERT_EQ(kNumReaders, kNumWriters);
  std::atomic<size_t> totalReads{0};

  auto readerCoro = [&totalReads](
                        size_t readerId,
                        RWQueue<int>& q,
                        size_t count) -> folly::coro::Task<void> {
    int numReads = 0;
    while (true) {
      try {
        auto item = co_await q.getCoro();
        if (item.hasError()) {
          LOG(INFO) << "Reader" << readerId << " terminating.";
          break;
        }
        ++numReads;
        ++totalReads;
        VLOG(1) << "Reader" << readerId << " received " << item.value();
        if (totalReads == kNumWriters * kCountPerWriter) {
          LOG(INFO) << "Closing queue";
          q.close();
        }
      } catch (std::exception const& e) {
        LOG(FATAL) << folly::exceptionStr(e);
      }
    }
    LOG(INFO) << "Reader" << readerId << " done. Received " << numReads
              << " items";
    EXPECT_LE(1, numReads); // Should read atleast one item
    co_return;
  };

  auto writerCoro = [](size_t writerId,
                       RWQueue<int>& q,
                       size_t count) -> folly::coro::Task<void> {
    for (size_t i = 0; i < count; ++i) {
      q.push(i);
      // VLOG(1) << "Writer " << writerId << " sending " << i;
    }
    LOG(INFO) << "Writer" << writerId << " done. Sent " << count << " items";
    co_return;
  };

  RWQueue<int> q;
  folly::ManualExecutor executor;
  for (size_t i = 0; i < kNumReaders; ++i) {
    readerCoro(i, q, kCountPerWriter).scheduleOn(&executor).start();
  }
  for (size_t i = 0; i < kNumWriters; ++i) {
    writerCoro(i, q, kCountPerWriter).scheduleOn(&executor).start();
  }

  executor.drain();
  EXPECT_EQ(kNumWriters * kCountPerWriter, totalReads);
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());
}
#endif

TEST(RQueueTest, ReadTest) {
  auto rwq = std::make_shared<RWQueue<int>>();
  RQueue<int> rq(rwq);

  rwq->push(1);
  rwq->push(2);

  EXPECT_EQ(1, rq.get().value());
  EXPECT_EQ(2, rq.get().value());

#if FOLLY_HAS_COROUTINES
  auto coroRead = [](RQueue<int>& rq, int expected) -> folly::coro::Task<void> {
    LOG(INFO) << "Performing coro read";
    auto item = co_await rq.getCoro();
    EXPECT_EQ(expected, item.value());
    LOG(INFO) << "Coro read successful";
  };

  folly::ManualExecutor executor;
  coroRead(rq, 5).scheduleOn(&executor).start();
  executor.drive();
  EXPECT_EQ(1, rwq->numPendingReads());
  EXPECT_EQ(0, rwq->size());

  LOG(INFO) << "Unblocking coro read";
  rwq->push(5);
  executor.drive();
  EXPECT_EQ(0, rwq->numPendingReads());
  EXPECT_EQ(0, rwq->size());
#endif
}
