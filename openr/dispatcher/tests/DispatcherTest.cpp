/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/dispatcher/Dispatcher.h>

#include <folly/fibers/FiberManagerMap.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/dispatcher/DispatcherQueue.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/messaging/Queue.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

class DispatcherTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    dispatcher_ = std::make_shared<Dispatcher>(
        kvStoreUpdatesQueue_.getReader(), kvStorePublicationsQueue_);

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
    kvStorePublicationsQueue_.close();
    dispatcher_->stop();
    LOG(INFO) << "Stopping the Dispatcher thread";
    dispatcherThread_->join();
    LOG(INFO) << "Dispatcher thread got stopped";
  }

  // Serializes/deserializes thrift objects
  apache::thrift::CompactSerializer serializer_{};

  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue_;
  DispatcherQueue kvStorePublicationsQueue_;
  // Dispatcher owned by this wrapper
  std::shared_ptr<Dispatcher> dispatcher_{nullptr};
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

TEST_F(DispatcherTestFixture, DispatcherApiTest) {
  std::vector<std::string> filter1{"adj:"};
  std::vector<std::string> filter2{
      "adj:", "prefix:", "key7:", "key10:", "key25"};
  std::vector<std::string> filter3{};

  auto reader1 = dispatcher_->getReader(filter1);
  auto reader2 = dispatcher_->getReader(filter2);
  auto reader3 = dispatcher_->getReader(filter3);

  auto resp = dispatcher_->getDispatcherFilters().get();

  // check returned ptr is non-null
  EXPECT_NE(resp, nullptr);

  auto filters = *resp;

  EXPECT_NE(
      std::find(filters.cbegin(), filters.cend(), filter1), filters.cend());
  EXPECT_NE(
      std::find(filters.cbegin(), filters.cend(), filter2), filters.cend());
  EXPECT_NE(
      std::find(filters.cbegin(), filters.cend(), filter3), filters.cend());
}

class DispatcherKnobTestFixture : public DispatcherTestFixture {
 public:
  void
  SetUp() override {
    config_ = std::make_shared<Config>(createConfig());
    createKvStore();
    createDispatcher(kvStoreWrapper_->getReader());
  }

  void
  TearDown() override {
    kvStoreWrapper_->closeQueue();
    // ensure that Dispatcher can shutdown
    DispatcherTestFixture::TearDown();
  }

  virtual thrift::OpenrConfig
  createConfig() {
    // create basic openr config
    auto tConfig = getBasicOpenrConfig();
    return tConfig;
  }

  void
  createKvStore() {
    kvStoreWrapper_ =
        std::make_unique<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            config_->getAreaIds(), /* areaId collection */
            config_->toThriftKvStoreConfig() /* thrift::KvStoreConfig */);
    kvStoreWrapper_->run();
  }

  void
  createDispatcher(messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue) {
    dispatcher_ = std::make_shared<Dispatcher>(
        kvStoreUpdatesQueue, kvStorePublicationsQueue_);

    dispatcherThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Dispatcher thread starting";
      dispatcher_->run();
      LOG(INFO) << "Dispatcher thread finished";
    });

    dispatcher_->waitUntilRunning();
  }

  std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>
      kvStoreWrapper_;

  std::shared_ptr<Config> config_;
};

/*
 * Test will check that Dispatcher knob can successfully be switched on and will
 * ensure that Dispatcher is running. A update to KvStore will be made and the
 * test will verify that subscribers of Dispatcher can receive the update.
 */
TEST_F(DispatcherKnobTestFixture, DataPathTest) {
  EXPECT_TRUE(dispatcher_->isRunning());

  auto subscriber1 = dispatcher_->getReader({"adj:"});
  auto subscriber2 = dispatcher_->getReader({});

  const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
  const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");

  const auto adj12 =
      createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
  const auto adj21 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);

  std::vector<std::pair<std::string, thrift::Value>> keyVals = {
      createPrefixKeyValue("1", 1, addr1),
      createPrefixKeyValue("2", 1, addr2),
      {"adj:1", createAdjValue(serializer_, "1", 1, {adj12}, false, 1)},
      {"adj:2", createAdjValue(serializer_, "2", 1, {adj21}, false, 2)}

  };

  // check to ensure keys were set properly
  EXPECT_TRUE(kvStoreWrapper_->setKeys(kTestingAreaName, keyVals));

  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  // Expected publication received by subscriber 1
  auto expectedPublication1 = createThriftPublication(
      {{"adj:1", createAdjValue(serializer_, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer_, "2", 1, {adj21}, false, 2)}},
      {},
      {},
      {});

  // Expected publication received by subscriber 2
  auto expectedPublication2 = createThriftPublication(
      {{"adj:1", createAdjValue(serializer_, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer_, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});

  manager.addTask([&subscriber1, expectedPublication1]() mutable {
    auto maybePub = subscriber1.get();
    EXPECT_TRUE(maybePub.hasValue());

    folly::variant_match(
        std::move(maybePub).value(),
        [&expectedPublication1](thrift::Publication&& pub) {
          EXPECT_TRUE(equalPublication(
              std::move(pub), std::move(expectedPublication1)));
        },
        [](thrift::InitializationEvent&&) {});
  });

  manager.addTask([&subscriber2, expectedPublication2]() mutable {
    auto maybePub = subscriber2.get();
    EXPECT_TRUE(maybePub.hasValue());

    folly::variant_match(
        std::move(maybePub).value(),
        [&expectedPublication2](thrift::Publication&& pub) {
          EXPECT_TRUE(equalPublication(
              std::move(pub), std::move(expectedPublication2)));
        },
        [](thrift::InitializationEvent&&) {});
  });

  evb.loop();
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  const folly::Init init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
