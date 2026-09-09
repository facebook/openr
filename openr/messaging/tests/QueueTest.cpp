/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

namespace {

struct StateUpdate {
  std::string key;
  int value;
  bool barrier{false};

  bool
  operator==(const StateUpdate& other) const {
    return key == other.key && value == other.value && barrier == other.barrier;
  }
};

StateSuppressionKey
getStateSuppressionKey(const StateUpdate& update) {
  return StateSuppressionKey{
      update.key,
      update.barrier ? StateSuppressionAction::KEY_BARRIER
                     : StateSuppressionAction::REPLACE_PENDING};
}

StateSuppressionPolicy<StateUpdate>
getStateSuppressionPolicy(const size_t activationThreshold = 0) {
  return StateSuppressionPolicy<StateUpdate>{
      getStateSuppressionKey, activationThreshold};
}

} // namespace

TEST(RWQueueTest, SizeAndReaders) {
  RWQueue<int> q;

  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());

  q.push(1);
  q.push(2);

  EXPECT_EQ(2, q.size());
  EXPECT_EQ(0, q.numPendingReads());
  EXPECT_EQ(2, q.numWrites());
  EXPECT_EQ(0, q.numReads());

  EXPECT_EQ(1, q.get().value());
  EXPECT_EQ(2, q.get().value());

  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());
  EXPECT_EQ(2, q.numWrites());
  EXPECT_EQ(2, q.numReads());

  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);
  manager.addTask([&q]() mutable { EXPECT_EQ(1, q.get().value()); });
  manager.addTask([&q]() mutable { EXPECT_EQ(2, q.get().value()); });

  evb.loopOnce();
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(2, q.numPendingReads());
  EXPECT_EQ(2, q.numWrites());
  EXPECT_EQ(2, q.numReads());

  q.push(1);
  q.push(2);
  evb.loopOnce();
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());
  EXPECT_EQ(4, q.numWrites());
  EXPECT_EQ(4, q.numReads());
}

TEST(RWQueueTest, DataTypes) {
  // class with const field
  class A {
   public:
    const std::string a;
    explicit A(std::string&& a) : a(std::move(a)) {}
  };

  {
    RWQueue<A> q;
    q.push(A("a"));
    EXPECT_EQ(1, q.size());
    EXPECT_EQ("a", q.get().value().a);
  }

  // class with shared_ptr<const A>
  class B {
   public:
    std::shared_ptr<const A> aPtr;
    explicit B(std::shared_ptr<const A> a) : aPtr(a) {}
  };

  {
    RWQueue<B> q;
    auto aPtr = std::make_shared<A>("a");
    q.push(B(aPtr));
    EXPECT_EQ(1, q.size());
    EXPECT_EQ("a", q.get().value().aPtr->a);
  }
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
  std::atomic<size_t> totalWrites{0};
  for (size_t i = 0; i < kNumWriters; ++i) {
    manager.addTask([&q, &totalWrites, i]() {
      for (size_t j = 0; j < kCountPerWriter; ++j) {
        const size_t num = i * kCountPerWriter + j;
        VLOG(1) << "Writer" << i << " sending " << num;
        q.push(num);
        ++totalWrites;
      }
      LOG(INFO) << "Writer" << i << " finished pushing " << kCountPerWriter
                << " messages.";
    });
  }

  evb.loop();
  EXPECT_EQ(totalWrites, q.numWrites());
  EXPECT_EQ(totalReads, q.numReads());
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

  std::atomic<size_t> totalWrites{0};
  auto writerCoro = [&totalWrites](
                        size_t writerId,
                        RWQueue<int>& q,
                        size_t count) -> folly::coro::Task<void> {
    for (size_t i = 0; i < count; ++i) {
      q.push(static_cast<int>(i));
      ++totalWrites;
      // VLOG(1) << "Writer " << writerId << " sending " << i;
    }
    LOG(INFO) << "Writer" << writerId << " done. Sent " << count << " items";
    co_return;
  };

  RWQueue<int> q;
  folly::ManualExecutor executor;
  for (size_t i = 0; i < kNumReaders; ++i) {
    co_withExecutor(&executor, readerCoro(i, q, kCountPerWriter)).start();
  }
  for (size_t i = 0; i < kNumWriters; ++i) {
    co_withExecutor(&executor, writerCoro(i, q, kCountPerWriter)).start();
  }

  executor.drain();
  EXPECT_EQ(kNumWriters * kCountPerWriter, totalReads);
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(0, q.numPendingReads());
  EXPECT_EQ(totalWrites, q.numWrites());
  EXPECT_EQ(totalReads, q.numReads());
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
  co_withExecutor(&executor, coroRead(rq, 5)).start();
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

TEST(RWQueueTest, PushTimeCoalescing) {
  // Coalescer sums the incoming value into the pending tail element.
  RWQueue<int> q("coalescing", [](int& existing, int& incoming) {
    existing += incoming;
    return true;
  });

  q.push(1); // empty queue -> appended (coalescer only runs when non-empty)
  q.push(2); // -> merged into tail
  q.push(3); // -> merged into tail
  EXPECT_EQ(1, q.size()); // all collapsed into a single pending element
  EXPECT_EQ(6, q.get().value());
  EXPECT_EQ(0, q.size());
}

TEST(RWQueueTest, CoalescingReturnFalseAppends) {
  // Coalesce only positive increments; a non-positive value starts a new
  // element (coalescer returns false -> append).
  RWQueue<int> q("coalescing", [](int& existing, int& incoming) {
    if (incoming > 0) {
      existing += incoming;
      return true;
    }
    return false;
  });

  q.push(1);
  q.push(2); // merged -> 3
  q.push(-5); // not coalesced -> new tail element
  q.push(4); // merged into tail -> -1
  EXPECT_EQ(2, q.size());
  EXPECT_EQ(3, q.get().value());
  EXPECT_EQ(-1, q.get().value());
}

TEST(RWQueueTest, NoCoalescerIsPlainAppend) {
  RWQueue<int> q; // no coalescer -> normal append behavior
  q.push(1);
  q.push(2);
  EXPECT_EQ(2, q.size());
  EXPECT_EQ(1, q.get().value());
  EXPECT_EQ(2, q.get().value());
}

TEST(RWQueueTest, KeyedStateSuppression) {
  RWQueue<StateUpdate> q("state-suppression", getStateSuppressionPolicy());

  q.push(StateUpdate{"a", 1});
  q.push(StateUpdate{"b", 1});
  q.push(StateUpdate{"a", 2});
  q.push(StateUpdate{"c", 1});
  q.push(StateUpdate{"b", 2});

  EXPECT_EQ(3, q.size());
  auto stats = q.getStats();
  EXPECT_EQ(5, stats.writes);
  EXPECT_EQ(0, stats.reads);
  EXPECT_EQ(3, stats.size);
  EXPECT_EQ((StateUpdate{"a", 2}), q.get().value());
  auto statsAfterRead = q.getStats();
  EXPECT_EQ(1, statsAfterRead.reads);
  EXPECT_EQ(2, statsAfterRead.size);
  EXPECT_EQ((StateUpdate{"c", 1}), q.get().value());
  EXPECT_EQ((StateUpdate{"b", 2}), q.get().value());
}

TEST(RWQueueTest, KeyedStateSuppressionBarrier) {
  RWQueue<StateUpdate> q("state-suppression", getStateSuppressionPolicy());

  q.push(StateUpdate{"a", 1});
  q.push(StateUpdate{"a", 2, true});
  q.push(StateUpdate{"a", 3});
  q.push(StateUpdate{"a", 4});

  EXPECT_EQ(3, q.size());
  EXPECT_EQ((StateUpdate{"a", 1}), q.get().value());
  EXPECT_EQ((StateUpdate{"a", 2, true}), q.get().value());
  EXPECT_EQ((StateUpdate{"a", 4}), q.get().value());
}

TEST(RWQueueTest, KeyedStateSuppressionCloseWithPendingState) {
  RWQueue<StateUpdate> q("state-suppression", getStateSuppressionPolicy());

  q.push(StateUpdate{"a", 1});
  q.push(StateUpdate{"b", 1});
  q.push(StateUpdate{"a", 2});
  ASSERT_EQ(2, q.size());

  q.close();
  EXPECT_EQ(0, q.size());
  EXPECT_EQ(QueueError::QUEUE_CLOSED, q.get().error());
}

TEST(RWQueueTest, KeyedStateSuppressionWithBlockedReader) {
  RWQueue<StateUpdate> q("state-suppression", getStateSuppressionPolicy());
  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  manager.addTask(
      [&q]() { EXPECT_EQ((StateUpdate{"a", 1}), q.get().value()); });
  evb.loopOnce();
  ASSERT_EQ(1, q.numPendingReads());

  q.push(StateUpdate{"a", 1});
  evb.loopOnce();
  EXPECT_EQ(0, q.numPendingReads());
  EXPECT_EQ(0, q.size());

  q.push(StateUpdate{"a", 2});
  q.push(StateUpdate{"a", 3});
  EXPECT_EQ(1, q.size());
  EXPECT_EQ((StateUpdate{"a", 3}), q.get().value());
}

TEST(RWQueueTest, KeyedStateSuppressionConcurrentProducersAndConsumer) {
  constexpr size_t kNumProducers{8};
  constexpr int kUpdatesPerProducer{8192};
  constexpr int kBarrierInterval{257};
  RWQueue<StateUpdate> q("state-suppression", getStateSuppressionPolicy());

  std::vector<int> lastSeen(kNumProducers, -1);
  bool orderPreserved{true};
  std::thread consumer([&]() {
    size_t terminalBarriers{0};
    while (terminalBarriers < kNumProducers) {
      auto update = q.get().value();
      const auto producer = static_cast<size_t>(update.key.front() - 'a');
      if (update.barrier && update.value == kUpdatesPerProducer) {
        orderPreserved &= lastSeen.at(producer) == kUpdatesPerProducer - 1;
        ++terminalBarriers;
        continue;
      }
      orderPreserved &= update.value > lastSeen.at(producer);
      lastSeen.at(producer) = update.value;
    }
  });

  std::vector<std::thread> producers;
  producers.reserve(kNumProducers);
  for (size_t producer = 0; producer < kNumProducers; ++producer) {
    producers.emplace_back([&, producer]() {
      const std::string key(1, static_cast<char>('a' + producer));
      for (int value = 0; value < kUpdatesPerProducer; ++value) {
        q.push(StateUpdate{key, value, (value + 1) % kBarrierInterval == 0});
      }
      q.push(StateUpdate{key, kUpdatesPerProducer, true});
    });
  }

  for (auto& producer : producers) {
    producer.join();
  }
  consumer.join();

  EXPECT_TRUE(orderPreserved);
  EXPECT_EQ(std::vector<int>(kNumProducers, kUpdatesPerProducer - 1), lastSeen);
  EXPECT_EQ(kNumProducers * (kUpdatesPerProducer + 1), q.numWrites());
  EXPECT_EQ(0, q.size());
}

TEST(RWQueueTest, KeyedStateSuppressionActivatesAboveThreshold) {
  RWQueue<StateUpdate> q("state-suppression", getStateSuppressionPolicy(3));

  q.push(StateUpdate{"a", 1});
  q.push(StateUpdate{"b", 1});
  q.push(StateUpdate{"a", 2});

  EXPECT_EQ(3, q.size());

  q.push(StateUpdate{"c", 1});

  EXPECT_EQ(3, q.size());
  EXPECT_EQ((StateUpdate{"b", 1}), q.get().value());
  EXPECT_EQ((StateUpdate{"a", 2}), q.get().value());
  EXPECT_EQ((StateUpdate{"c", 1}), q.get().value());
}

TEST(RWQueueTest, KeyedStateSuppressionPreservesBarriersAtActivation) {
  RWQueue<StateUpdate> q("state-suppression", getStateSuppressionPolicy(4));

  q.push(StateUpdate{"a", 1});
  q.push(StateUpdate{"a", 2, true});
  q.push(StateUpdate{"a", 3});
  q.push(StateUpdate{"a", 4});
  q.push(StateUpdate{"b", 1});

  EXPECT_EQ(4, q.size());
  EXPECT_EQ((StateUpdate{"a", 1}), q.get().value());
  EXPECT_EQ((StateUpdate{"a", 2, true}), q.get().value());
  EXPECT_EQ((StateUpdate{"a", 4}), q.get().value());
  EXPECT_EQ((StateUpdate{"b", 1}), q.get().value());
}

TEST(RWQueueTest, KeyedStateSuppressionReturnsToFifoAfterDrain) {
  RWQueue<StateUpdate> q("state-suppression", getStateSuppressionPolicy(2));

  q.push(StateUpdate{"a", 1});
  q.push(StateUpdate{"a", 2});
  q.push(StateUpdate{"b", 1});
  EXPECT_EQ((StateUpdate{"a", 2}), q.get().value());
  EXPECT_EQ((StateUpdate{"b", 1}), q.get().value());

  q.push(StateUpdate{"c", 1});
  q.push(StateUpdate{"c", 2});

  EXPECT_EQ(2, q.size());
  EXPECT_EQ((StateUpdate{"c", 1}), q.get().value());
  EXPECT_EQ((StateUpdate{"c", 2}), q.get().value());
}
