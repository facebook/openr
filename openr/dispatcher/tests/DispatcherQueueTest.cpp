/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#define DispatcherQueue_TEST_FRIENDS \
  FRIEND_TEST(DispatcherQueueTest, OpenQueueTest);

#include <openr/dispatcher/DispatcherQueue.h>

#include <folly/Overload.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/tests/utils/Utils.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {

/*
 * Test will check that DispatcherQueue can successfully read and write. This
 * will only pass thrift::InitializationEvent objects which don't get filtered.
 * It will then check that the correct amount of replicatedWrites and
 * replicatedReads are made.
 */
TEST(DispatcherQueueTest, EventReadWriteTest) {
  const size_t kNumReaders{4};
  const size_t kTotalWrites{64};

  DispatcherQueue q;
  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  // Add readers
  std::atomic<size_t> totalReads{0};
  std::atomic<size_t> replicatedReads{0};
  for (size_t i = 0; i < kNumReaders; ++i) {
    manager.addTask([reader = q.getReader({"prefix:"}),
                     &q,
                     &totalReads,
                     &replicatedReads,
                     i,
                     kTotalWrites = std::as_const(kTotalWrites)]() mutable {
      size_t numReads{0};
      while (true) {
        VLOG(1) << "Reader" << i << " attempting a read";
        auto maybeNum = reader.get();
        if (maybeNum.hasError()) {
          EXPECT_EQ(maybeNum.error(), messaging::QueueError::QUEUE_CLOSED);
          break;
        }
        ++numReads;
        ++totalReads;
        if (totalReads == kTotalWrites * kNumReaders) {
          // Before closing the replicated queue (and hence internal RWQueues),
          // get replication stats and verify that replication reads match
          // the overall reads we expect
          std::vector<messaging::RWQueueStats> stats = q.getReplicationStats();
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
      auto intializationEvent = thrift::InitializationEvent::AGENT_CONFIGURED;

      q.push(intializationEvent);
      ++totalWrites;
    }
    LOG(INFO) << "Writer finished pushing " << kTotalWrites << " messages.";

    // Get replicated queue stats
    std::vector<messaging::RWQueueStats> stats = q.getReplicationStats();
    for (auto& stat : stats) {
      replicationWrites += stat.writes;
    }
  });

  EXPECT_EQ(kNumReaders, q.getNumReaders()); // All readers should be active
  evb.loop();
  EXPECT_EQ(0, q.getNumReaders()); // All readers should have been closed

  EXPECT_EQ(kTotalWrites * kNumReaders, totalReads);
  EXPECT_EQ(totalWrites, q.getNumWrites());
  EXPECT_EQ(kTotalWrites * kNumReaders, replicationWrites);
}

/*
 * Test will check that DispatcherQueue can successfully read and write. This
 * will only pass thrift::Publication objects which do not get filtered. It
 * will then check that the correct amount of replicatedWrites and
 * replicatedReads are made.
 */
TEST(DispatcherQueueTest, PublicationReadWriteTest) {
  const size_t kNumReaders{8};
  const size_t kTotalWrites{512};

  DispatcherQueue q;
  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);
  // Serializes/deserializes thrift objects
  apache::thrift::CompactSerializer serializer_{};

  // Add readers
  std::atomic<size_t> totalReads{0};
  std::atomic<size_t> replicatedReads{0};
  for (size_t i = 0; i < kNumReaders; ++i) {
    manager.addTask([reader = q.getReader(),
                     &q,
                     &totalReads,
                     &replicatedReads,
                     i,
                     kTotalWrites = std::as_const(kTotalWrites)]() mutable {
      size_t numReads{0};
      while (true) {
        VLOG(1) << "Reader" << i << " attempting a read";
        auto maybeNum = reader.get();
        if (maybeNum.hasError()) {
          EXPECT_EQ(maybeNum.error(), messaging::QueueError::QUEUE_CLOSED);
          break;
        }
        ++numReads;
        ++totalReads;
        if (totalReads == kTotalWrites * kNumReaders) {
          // Before closing the replicated queue (and hence internal RWQueues),
          // get replication stats and verify that replication reads match
          // the overall reads we expect
          std::vector<messaging::RWQueueStats> stats = q.getReplicationStats();
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
  manager.addTask([&q, &totalWrites, &replicationWrites, &serializer_]() {
    for (size_t i = 0; i < kTotalWrites; ++i) {
      const auto adj12 = createAdjacency(
          "2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
      const auto adj21 = createAdjacency(
          "1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);
      const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
      const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");

      // push thrift Publication into queue
      auto publication = createThriftPublication(
          {{"adj:1", createAdjValue(serializer_, "1", 1, {adj12}, false, 1)},
           {"adj:2", createAdjValue(serializer_, "2", 1, {adj21}, false, 2)},
           createPrefixKeyValue("1", 1, addr1),
           createPrefixKeyValue("2", 1, addr2),
           {"key1", createThriftValue(1, "node1", std::string("value1"))}},
          {},
          {},
          {});

      q.push(publication);
      ++totalWrites;
    }
    LOG(INFO) << "Writer finished pushing " << kTotalWrites << " messages.";

    // Get replicated queue stats
    std::vector<messaging::RWQueueStats> stats = q.getReplicationStats();
    for (auto& stat : stats) {
      replicationWrites += stat.writes;
    }
  });

  EXPECT_EQ(kNumReaders, q.getNumReaders()); // All readers should be active
  evb.loop();
  EXPECT_EQ(0, q.getNumReaders()); // All readers should have been closed

  EXPECT_EQ(kTotalWrites * kNumReaders, totalReads);
  EXPECT_EQ(totalWrites, q.getNumWrites());
  EXPECT_EQ(kTotalWrites * kNumReaders, replicationWrites);
}

/*
 * Test will check that DispatcherQueue can successfully read and write. This
 * will only pass thrift::Publication objects which do get filtered. It
 * will then check that the correct amount of replicatedWrites and
 * replicatedReads are made.
 */
TEST(DispatcherQueueTest, FilterPublicationReadWriteTest) {
  DispatcherQueue q;
  folly::EventBase evb;

  auto& manager = folly::fibers::getFiberManager(evb);

  auto reader1 = q.getReader({"adj:3", "adj:2", "adj:4", "adj:10"});
  auto reader2 = q.getReader({"key1"});
  auto reader3 = q.getReader({"key2"});
  auto reader4 = q.getReader({"adj:5"});

  // Serializes/deserializes thrift objects
  apache::thrift::CompactSerializer serializer_{};

  const auto adj32 =
      createAdjacency("2", "3/2", "2/3", "fe80::2", "192.168.0.2", 10, 100002);
  const auto adj21 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);
  const auto adj23 =
      createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 10, 100003);
  const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");

  // push thrift Publication into queue
  auto publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer_, "3", 1, {adj32}, false, 3)},
       {"adj:2", {}},
       {"adj:4", createAdjValue(serializer_, "4", 1, {}, false, 4)},
       createPrefixKeyValue("3", 1, addr3),
       {"key1", createThriftValue(1, "node1", std::string("value1"))}},
      {"adj:10", "key21"}, // expiredKeys
      {},
      {});

  q.push(publication);

  manager.addTask([&reader1, &serializer_, &adj32]() mutable {
    auto maybePub = reader1.get();

    // no empty keyVals should be in the publication
    // no keys that don't match the prefix should be in publication
    // keyVals and expiredKeys should be non-empty
    auto expectedPublication = createThriftPublication(
        {{"adj:3", createAdjValue(serializer_, "3", 1, {adj32}, false, 3)},
         {"adj:4", createAdjValue(serializer_, "4", 1, {}, false, 4)}},
        {"adj:10"},
        {},
        {});

    folly::variant_match(
        std::move(maybePub).value(),
        [expectedPublication](thrift::Publication&& pub) {
          EXPECT_TRUE(pub == expectedPublication);
        },
        [](thrift::InitializationEvent) {});
  });
  manager.addTask([&reader2]() mutable {
    auto maybePub = reader2.get();
    // keyVals should be non-empty
    auto expectedPublication = createThriftPublication(
        {{"key1", createThriftValue(1, "node1", std::string("value1"))}},
        {},
        {},
        {});

    folly::variant_match(
        std::move(maybePub).value(),
        [expectedPublication](thrift::Publication&& pub) {
          EXPECT_TRUE(pub == expectedPublication);
        },
        [](thrift::InitializationEvent) {});
  });

  manager.addTask([&reader3]() mutable {
    auto maybePub = reader3.get();
    // expiredKeys should be non-empty
    auto expectedPublication = createThriftPublication({}, {"key21"}, {}, {});

    folly::variant_match(
        std::move(maybePub).value(),
        [expectedPublication](thrift::Publication&& pub) {
          EXPECT_TRUE(pub == expectedPublication);
        },
        [](thrift::InitializationEvent) {});
  });

  // nothing should be pushed to this reader since no non-empty publications
  // match the filter
  manager.addTask([&reader4]() mutable { EXPECT_EQ(reader4.size(), 0); });

  evb.loop();
}

/*
 * Test will check that DispatcherQueue can only have readers when the queue is
 * open. It checks that if you getReader is called on a closed queue,
 * it throws an error.
 */
TEST(DispatcherQueueTest, OpenQueueTest) {
  DispatcherQueue q;
  auto r1 = q.getReader();
  EXPECT_EQ(1, q.getNumReaders());

  q.close();
  EXPECT_EQ(0, q.getNumReaders());

  // attempt to push and check it fails
  auto publication = createThriftPublication({}, {}, {}, {}, {});
  EXPECT_FALSE(q.push(publication));

  // make sure can't add reader when DispatcherQueue is closed
  EXPECT_THROW(q.getReader(), std::runtime_error);

  // reopen queue and make sure reader can be added
  q.closed_ = false;
  {
    auto r2 = q.getReader();
    EXPECT_EQ(1, q.getNumReaders());
  }
  q.close();
}

/*
 * Test will check the DispatcherQueue API to check verify all of the correct
 * filters are returned for each of the internal RW queue. Test will also remove
 * RW queues from internal DispatcherQueue if reader has become stale.
 */
TEST(DispatcherQueueTest, DispatcherQueueFilterApiTest) {
  DispatcherQueue q;

  std::vector<std::string> filters1{"prefix:"};
  std::vector<std::string> filters2{"key1:", "key2:"};
  std::vector<std::string> filters3{"adj:", "prefix:"};
  std::vector<std::string> filters4{};
  std::vector<std::string> filters5{"key1:", "key2:", "adj:", "prefix:"};

  {
    auto r1 = q.getReader(filters1);
    auto r2 = q.getReader(filters2);
    auto r3 = q.getReader(filters3);
    auto r4 = q.getReader(filters4);
    auto r5 = q.getReader(filters5);

    auto filters = q.getFilters();

    EXPECT_EQ(filters->size(), 5);

    EXPECT_EQ(filters->at(0), filters1);
    EXPECT_EQ(filters->at(1), filters2);
    EXPECT_EQ(filters->at(2), filters3);
    EXPECT_EQ(filters->at(3), filters4);
    EXPECT_EQ(filters->at(4), filters5);
  }

  {
    // attempt to get filters when readers are all out of scope
    // getFilters functions should remove all of the readers
    auto filters = q.getFilters();
    EXPECT_EQ(filters->size(), 0);
  }
}

} // namespace openr
