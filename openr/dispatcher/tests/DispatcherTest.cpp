/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/dispatcher/Dispatcher.h>

#include <openr/dispatcher/DispatcherQueue.h>
#include <vector>

#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/messaging/Queue.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

class DispatcherTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    dispatcher_ = std::make_shared<dispatcher::Dispatcher>(
        kvStoreUpdatesQueue_.getReader());

    dispatcherThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Dispatcher thread starting";
      dispatcher_->run();
      LOG(INFO) << "Dispatcher thread finishing";
    });

    dispatcher_->waitUntilRunning();
  }

  void
  TearDown() override {
    // ensure that Dispatcher can shutdown
    kvStoreUpdatesQueue_.close();
    dispatcher_->stop();
    LOG(INFO) << "Stopping the Dispatcher thread";
    dispatcherThread_->join();
    LOG(INFO) << "Dispatcher thread got stopped";
  }

  // Serializes/deserializes thrift objects
  apache::thrift::CompactSerializer serializer_{};
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue_;
  // Dispatcher owned by this wrapper
  std::shared_ptr<dispatcher::Dispatcher> dispatcher_{nullptr};
  // Thread in which Dispatcher will be running
  std::unique_ptr<std::thread> dispatcherThread_{nullptr};
};

/*
 * Test will check that Dispatcher can successfully subscribe to KvStore
 * and Dispatcher can add subscribers. It will then try to write a
 * thrift::Publication to its readers based off the provided filter and ensure
 * that they can read the publication if it passes the filter. The tests expects
 * that each reader gets the same publication and that Dispatcher made 7
 * replicated writes and had 4 replicated reads.
 */
TEST_F(DispatcherTestFixture, ReadWriteTest) {
  // check that dispatcher is a reader of kvStoreUpdatesQueue_
  EXPECT_EQ(kvStoreUpdatesQueue_.getNumReaders(), 1);

  // add readers to dispatcher
  auto dispatcherReader1 = dispatcher_->getReader();
  auto dispatcherReader2 = dispatcher_->getReader({"prefix:"});
  auto dispatcherReader3 = dispatcher_->getReader({"adj:"});
  auto dispatcherReader4 = dispatcher_->getReader({"key:"});

  EXPECT_EQ(dispatcher_->getNumReaders(), 4);

  const auto adj12 =
      createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
  const auto adj21 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);
  const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
  const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");

  // push thrift Publication into queue
  auto publication1 = createThriftPublication(
      {{"adj:1", createAdjValue(serializer_, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer_, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2),
       {"key1", createThriftValue(1, "node1", std::string("value1"))}},
      {},
      {},
      {});

  // publication 1 with all keys except prefix keys filtered
  auto prefixPublication = createThriftPublication(
      {createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});

  // publication 1 with all keys except adj keys filtered
  auto adjPublication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer_, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer_, "2", 1, {adj21}, false, 2)}},
      {},
      {},
      {});

  kvStoreUpdatesQueue_.push(publication1);

  // push thrift InitializationEvent into the queue
  auto publication2 = thrift::InitializationEvent::KVSTORE_SYNCED;
  kvStoreUpdatesQueue_.push(publication2);

  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  // set up reader tasks
  manager.addTask([&dispatcherReader1, publication1, publication2]() mutable {
    auto maybePub = dispatcherReader1.get();
    EXPECT_TRUE(maybePub.hasValue());

    folly::variant_match(
        std::move(maybePub).value(),
        [publication1](thrift::Publication&& pub) {
          EXPECT_TRUE(pub == publication1);
        },
        [publication2](thrift::InitializationEvent&& event) {
          EXPECT_TRUE(event == publication2);
        });
  });

  manager.addTask(
      [&dispatcherReader2, prefixPublication, publication2]() mutable {
        auto maybePub = dispatcherReader2.get();
        EXPECT_TRUE(maybePub.hasValue());

        folly::variant_match(
            std::move(maybePub).value(),
            [prefixPublication](thrift::Publication&& pub) {
              EXPECT_TRUE(pub == prefixPublication);
            },
            [publication2](thrift::InitializationEvent&& event) {
              EXPECT_TRUE(event == publication2);
            });
      });

  manager.addTask([&dispatcherReader3, adjPublication, publication2]() mutable {
    auto maybePub = dispatcherReader3.get();
    EXPECT_TRUE(maybePub.hasValue());

    folly::variant_match(
        std::move(maybePub).value(),
        [adjPublication](thrift::Publication&& pub) {
          EXPECT_TRUE(pub == adjPublication);
        },
        [publication2](thrift::InitializationEvent&& event) {
          EXPECT_TRUE(event == publication2);
        });
  });

  // should never receive a thrift::Publication due to its filter
  manager.addTask([&dispatcherReader4, publication2]() mutable {
    auto maybePub = dispatcherReader4.get();
    EXPECT_TRUE(maybePub.hasValue());

    folly::variant_match(
        std::move(maybePub).value(),
        [](thrift::Publication&&) {},
        [publication2](thrift::InitializationEvent&& event) {
          EXPECT_TRUE(event == publication2);
        });
  });

  // check that publication1/publication2 was pushed to the readers
  evb.loop();

  // check write stats for Dispatcher
  EXPECT_EQ(dispatcher_->getNumWrites(), 2);

  // check Replication Stats after reading/writing
  std::vector<messaging::RWQueueStats> stats =
      dispatcher_->getReplicationStats();

  std::atomic<size_t> replicatedReads{0};
  std::atomic<size_t> replicatedWrites{0};

  for (auto& stat : stats) {
    replicatedReads += stat.reads;
    replicatedWrites += stat.writes;
  }

  EXPECT_EQ(replicatedReads, 4);
  // No thrift::Publication matched reader 4's filter leading to one less write
  EXPECT_EQ(replicatedWrites, 7);
}

/*
 * Test will check that Dispatcher does not replicated empty
 * thrift::publications. It will push an empty publication to the Dispatcher,
 * but the reader should not receive anything. The test expects that the reader
 * queue has a size of zero after the attempted push to Dispatcher.
 */
TEST_F(DispatcherTestFixture, EmptyPublicationTest) {
  auto dispatcherReader = dispatcher_->getReader({"adj:"});

  const auto adj42 =
      createAdjacency("2", "4/2", "2/4", "fe80::2", "192.168.0.2", 10, 100002);
  auto publication = createThriftPublication(
      {{"adj:1", {}}, {"adj:2", {}}, {"adj:4", {}}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {} /* keysToUpdate */);

  // push publication with empty values
  kvStoreUpdatesQueue_.push(publication);

  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  // nothing should be pushed to the reader
  manager.addTask(
      [&dispatcherReader]() mutable { EXPECT_EQ(dispatcherReader.size(), 0); });

  evb.loop();
}
