/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sodium.h>
#include <thread>
#include <unordered_set>

#include <openr/common/Util.h>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/init/Init.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreClientInternal.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

using namespace std;
using namespace openr;

namespace {

// the size of the value string
const uint32_t kValueStrSize = 64;
// max packet inter-arrival time (can't be chrono)
const uint32_t kReqTimeoutMs = 4000;
// interval for periodic syncs
const std::chrono::seconds kSyncInterval(1);
// maximum timeout for single request for sync
const std::chrono::milliseconds kSyncReqTimeout(1000);
// maximum timeout waiting for all peers to respond to sync request
const std::chrono::milliseconds kSyncMaxWaitTime(1000);

const std::chrono::milliseconds kTtl{1000};

} // namespace

//
// Three-store fixture to test dumpAllWithPrefixMultiple*
//
class MultipleStoreFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // intialize kvstore instances
    initKvStores();

    // intialize thriftWrapper instances
    initThriftWrappers();

    // initialize kvstoreClient instances
    initKvStoreClientInternal();
  }

  void
  TearDown() override {
    reset();
  }

  void
  reset() {
    // ATTN: kvStoreUpdatesQueue must be closed before destructing
    //       KvStoreClientInternal as fiber future is depending on RQueue
    store1->closeQueue();
    store2->closeQueue();
    store3->closeQueue();

    thriftWrapper1_->stop();
    thriftWrapper1_.reset();
    thriftWrapper2_->stop();
    thriftWrapper2_.reset();
    thriftWrapper3_->stop();
    thriftWrapper3_.reset();

    // ATTN:
    //  - Destroy client before destroy evb. Otherwise, destructor will
    //    FOREVER waiting fiber future to be fulfilled;
    //  - Destroy client before destroy KvStoreWrapper as client has
    //    timer set to periodically polling KvStore;
    client1.reset();
    client2.reset();
    client3.reset();

    store1->stop();
    store1.reset();
    store2->stop();
    store2.reset();
    store3->stop();
    store3.reset();

    evb.stop();
    evb.waitUntilStopped();
    evbThread.join();
  }

  void
  initKvStores() {
    // wrapper to spin up a kvstore through KvStoreWrapper
    auto makeStoreWrapper = [this](std::string nodeId) {
      config = std::make_shared<Config>(getBasicOpenrConfig(nodeId));
      return std::make_shared<KvStoreWrapper>(context, config);
    };

    // spin up KvStore instances through KvStoreWrapper
    store1 = makeStoreWrapper(node1);
    store2 = makeStoreWrapper(node2);
    store3 = makeStoreWrapper(node3);

    store1->run();
    store2->run();
    store3->run();
  }

  void
  initThriftWrappers() {
    // wrapper to spin up an OpenrThriftServerWrapper for thrift connection
    auto makeThriftServerWrapper =
        [this](std::string nodeId, std::shared_ptr<KvStoreWrapper> store) {
          return std::make_shared<OpenrThriftServerWrapper>(
              nodeId,
              nullptr /* decision */,
              nullptr /* fib */,
              store->getKvStore() /* kvStore */,
              nullptr /* linkMonitor */,
              nullptr /* monitor */,
              nullptr /* configStore */,
              nullptr /* prefixManager */,
              nullptr /* spark */,
              nullptr /* config */
          );
        };

    // spin up OpenrThriftServerWrapper for thrift connectivity
    thriftWrapper1_ = makeThriftServerWrapper(node1, store1);
    thriftWrapper1_->run();

    thriftWrapper2_ = makeThriftServerWrapper(node2, store2);
    thriftWrapper2_->run();

    thriftWrapper3_ = makeThriftServerWrapper(node3, store3);
    thriftWrapper3_->run();
  }

  void
  initKvStoreClientInternal() {
    // Create and initialize kvstore-clients
    auto port1 = thriftWrapper1_->getOpenrCtrlThriftPort();
    auto port2 = thriftWrapper2_->getOpenrCtrlThriftPort();
    auto port3 = thriftWrapper3_->getOpenrCtrlThriftPort();
    client1 = std::make_shared<KvStoreClientInternal>(
        &evb, node1, store1->getKvStore());

    client2 = std::make_shared<KvStoreClientInternal>(
        &evb,
        node2,
        store2->getKvStore(),
        persistKeyTimer /* checkPersistKeyPeriod */);

    client3 = std::make_shared<KvStoreClientInternal>(
        &evb, node3, store3->getKvStore());

    sockAddrs_.emplace_back(folly::SocketAddress{localhost_, port1});
    sockAddrs_.emplace_back(folly::SocketAddress{localhost_, port2});
    sockAddrs_.emplace_back(folly::SocketAddress{localhost_, port3});
  }

  OpenrEventBase evb;
  std::thread evbThread;
  apache::thrift::CompactSerializer serializer;
  fbzmq::Context context;

  std::shared_ptr<Config> config{nullptr};
  std::shared_ptr<KvStoreWrapper> store1{nullptr}, store2{nullptr},
      store3{nullptr};
  std::shared_ptr<OpenrThriftServerWrapper> thriftWrapper1_{nullptr},
      thriftWrapper2_{nullptr}, thriftWrapper3_{nullptr};
  std::shared_ptr<KvStoreClientInternal> client1{nullptr}, client2{nullptr},
      client3{nullptr};

  const std::chrono::milliseconds persistKeyTimer{100};
  const std::string localhost_{"::1"};
  const std::string node1{"node1"}, node2{"node2"}, node3{"node3"};

  std::vector<folly::SocketAddress> sockAddrs_;
};

/*
 * Class to create topology with multiple areas
 * Topology:
 *
 *  StoreA (pod-area)  --- (pod area) StoreB (plane area) -- (plane area) StoreC
 */
class MultipleAreaFixture : public MultipleStoreFixture {
 public:
  void
  SetUp() override {
    // intialize kvstore instances
    initKvStores();

    // intialize thriftWrapper instances
    initThriftWrappers();

    // initialize kvstoreClient instances
    initKvStoreClientInternal();
  }

  void
  TearDown() override {
    reset();
  }

  void
  setUpPeers() {
    // node1(pod-area)  --- (pod area) node2 (plane area) -- (plane area) node3
    for (auto& peer : peers1) {
      EXPECT_TRUE(store1->addPeer(planeArea, peer.first, peer.second));
    }
    for (auto& peer : peers2PlaneArea) {
      EXPECT_TRUE(store2->addPeer(planeArea, peer.first, peer.second));
    }
    for (auto& peer : peers2PodArea) {
      EXPECT_TRUE(store2->addPeer(podArea, peer.first, peer.second));
    }
    for (auto& peer : peers3) {
      EXPECT_TRUE(store3->addPeer(podArea, peer.first, peer.second));
    }
  }

  void
  initKvStores() {
    // wrapper to spin up a kvstore through KvStoreWrapper
    auto makeStoreWrapper =
        [this](std::string nodeId, std::unordered_set<std::string> areas) {
          std::vector<openr::thrift::AreaConfig> areaConfig;
          for (const auto& id : areas) {
            thrift::AreaConfig a;
            a.set_area_id(id);
            a.set_include_interface_regexes({".*"});
            a.set_neighbor_regexes({".*"});
            areaConfig.emplace_back(std::move(a));
          }
          auto tConfig = getBasicOpenrConfig(nodeId, "domain", areaConfig);
          config = std::make_shared<Config>(tConfig);
          return std::make_shared<KvStoreWrapper>(context, config);
        };

    // spin up KvStore instances through KvStoreWrapper
    store1 =
        makeStoreWrapper(node1, std::unordered_set<std::string>{planeArea});
    store2 = makeStoreWrapper(
        node2, std::unordered_set<std::string>{planeArea, podArea});
    store3 = makeStoreWrapper(node3, std::unordered_set<std::string>{podArea});

    store1->run();
    store2->run();
    store3->run();

    // add peers
    peers1.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(node2),
        std::forward_as_tuple(store2->getPeerSpec()));
    peers2PlaneArea.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(node1),
        std::forward_as_tuple(store1->getPeerSpec()));
    peers2PodArea.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(node3),
        std::forward_as_tuple(store3->getPeerSpec()));
    peers3.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(node2),
        std::forward_as_tuple(store2->getPeerSpec()));
  }

  const AreaId podArea{"pod-area"};
  const AreaId planeArea{"plane-area"};
  std::unordered_map<std::string, thrift::PeerSpec> peers1;
  std::unordered_map<std::string, thrift::PeerSpec> peers2PlaneArea;
  std::unordered_map<std::string, thrift::PeerSpec> peers2PodArea;
  std::unordered_map<std::string, thrift::PeerSpec> peers3;
};

/**
 * Merge different keys from three stores
 */
TEST_F(MultipleStoreFixture, dumpWithPrefixMultiple_differentKeys) {
  //
  // Submit three values in three different stores
  //
  folly::Baton waitBaton;
  evb.runInEventBaseThread([&]() noexcept {
    thrift::Value value;
    {
      value.value_ref() = "test_value1";
      client1->setKey(
          kTestingAreaName,
          "test_key1",
          writeThriftObjStr(value, serializer),
          100);
    }
    {
      value.value_ref() = "test_value2";
      client2->setKey(
          kTestingAreaName,
          "test_key2",
          writeThriftObjStr(value, serializer),
          200);
    }
    {
      value.value_ref() = "test_value3";
      client3->setKey(
          kTestingAreaName,
          "test_key3",
          writeThriftObjStr(value, serializer),
          300);
    }

    // Synchronization primitive
    waitBaton.post();
  });

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  auto maybe = dumpAllWithPrefixMultipleAndParse<thrift::Value>(
      kTestingAreaName, sockAddrs_, "test_");

  ASSERT_TRUE(maybe.first.has_value());

  {
    auto dump = maybe.first.value();
    EXPECT_EQ(3, dump.size());
    EXPECT_EQ("test_value1", dump["test_key1"].value_ref());
    EXPECT_EQ("test_value2", dump["test_key2"].value_ref());
    EXPECT_EQ("test_value3", dump["test_key3"].value_ref());
  }
}

/**
 * Merge same key with diff. values based on versions
 */
TEST_F(
    MultipleStoreFixture,
    dumpAllWithPrefixMultipleAndParse_sameKeysDiffValues) {
  //
  // Submit three values in three different stores
  //
  folly::Baton waitBaton;
  evb.runInEventBaseThread([&]() noexcept {
    thrift::Value value;
    {
      value.value_ref() = "test_value1";
      client1->setKey(
          kTestingAreaName,
          "test_key",
          writeThriftObjStr(value, serializer),
          300);
    }
    {
      value.value_ref() = "test_value2";
      client2->setKey(
          kTestingAreaName,
          "test_key",
          writeThriftObjStr(value, serializer),
          200);
    }
    {
      value.value_ref() = "test_value3";
      client3->setKey(
          kTestingAreaName,
          "test_key",
          writeThriftObjStr(value, serializer),
          100);
    }

    // Synchronization primitive
    waitBaton.post();
  });

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  auto maybe = dumpAllWithPrefixMultipleAndParse<thrift::Value>(
      kTestingAreaName, sockAddrs_, "test_");

  ASSERT_TRUE(maybe.first.has_value());

  {
    auto dump = maybe.first.value();
    EXPECT_EQ(1, dump.size());
    EXPECT_EQ("test_value1", dump["test_key"].value_ref());
  }
}

/**
 * Merge same key with diff. values using originator ids
 */
TEST_F(
    MultipleStoreFixture,
    dumpAllWithPrefixMultipleAndParse_sameKeysDiffValues2) {
  //
  // Submit three values in three different stores
  //
  folly::Baton waitBaton;
  evb.runInEventBaseThread([&]() noexcept {
    thrift::Value value;
    {
      value.value_ref() = "test_value1";
      client1->setKey(
          kTestingAreaName,
          "test_key",
          writeThriftObjStr(value, serializer),
          1);
    }
    {
      value.value_ref() = "test_value2";
      client2->setKey(
          kTestingAreaName,
          "test_key",
          writeThriftObjStr(value, serializer),
          1);
    }
    {
      value.value_ref() = "test_value3";
      client3->setKey(
          kTestingAreaName,
          "test_key",
          writeThriftObjStr(value, serializer),
          1);
    }

    // Synchronization primitive
    waitBaton.post();
  });

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  auto maybe = dumpAllWithPrefixMultipleAndParse<thrift::Value>(
      kTestingAreaName, sockAddrs_, "test_");

  ASSERT_TRUE(maybe.first.has_value());

  {
    auto dump = maybe.first.value();
    EXPECT_EQ(1, dump.size());
    EXPECT_EQ("test_value3", dump["test_key"].value_ref());
  }
}

TEST(KvStoreClientInternal, EmptyValueKey) {
  fbzmq::Context context;
  folly::Baton waitBaton;

  // start store1, store2, store 3
  auto config1 = std::make_shared<Config>(getBasicOpenrConfig("node1"));
  auto store1 = std::make_unique<KvStoreWrapper>(context, config1);
  store1->run();
  auto config2 = std::make_shared<Config>(getBasicOpenrConfig("node2"));
  auto store2 = std::make_unique<KvStoreWrapper>(context, config2);
  store2->run();
  auto config3 = std::make_shared<Config>(getBasicOpenrConfig("node3"));
  auto store3 = std::make_unique<KvStoreWrapper>(context, config3);
  store3->run();

  // add peers store1 <---> store2 <---> store3
  store1->addPeer(kTestingAreaName, store2->getNodeId(), store2->getPeerSpec());
  store2->addPeer(kTestingAreaName, store1->getNodeId(), store1->getPeerSpec());
  store2->addPeer(kTestingAreaName, store3->getNodeId(), store3->getPeerSpec());
  store3->addPeer(kTestingAreaName, store2->getNodeId(), store2->getPeerSpec());

  // add key in store1, check for the key in all stores
  // Create another OpenrEventBase instance for looping clients
  OpenrEventBase evb;
  int waitDuration{0};

  // create kvstore client for store 1
  auto client1 = std::make_shared<KvStoreClientInternal>(
      &evb, store1->getNodeId(), store1->getKvStore(), 1000ms);

  // Schedule callback to set keys from client1 (this will be executed first)
  evb.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 0), [&]() noexcept {
        client1->persistKey(kTestingAreaName, "k1", "v1", kTtl);
      });

  // check keys on all stores after sometime
  evb.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 300), [&]() noexcept {
        auto maybeThriftVal = store1->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("v1", maybeThriftVal.value().value_ref());

        maybeThriftVal = store2->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("v1", maybeThriftVal.value().value_ref());

        maybeThriftVal = store3->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("v1", maybeThriftVal.value().value_ref());
        EXPECT_EQ("node1", *maybeThriftVal.value().originatorId_ref());
        EXPECT_EQ(1, *maybeThriftVal.value().version_ref());
      });

  // set empty value on store1, check for empty value on other stores, and
  // key version is higher
  evb.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 10), [&]() noexcept {
        client1->clearKey(kTestingAreaName, "k1", "", kTtl);
      });

  // check key has empty value on all stores and version is incremented
  evb.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 300), [&]() noexcept {
        auto maybeThriftVal = store1->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("", maybeThriftVal.value().value_ref());

        maybeThriftVal = store2->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("", maybeThriftVal.value().value_ref());

        maybeThriftVal = store3->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("", maybeThriftVal.value().value_ref());
        EXPECT_EQ("node1", *maybeThriftVal.value().originatorId_ref());
        EXPECT_EQ(*maybeThriftVal.value().version_ref(), 2);
      });

  // persist key with new value, and check for new value and higher key version
  evb.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 10), [&]() noexcept {
        client1->persistKey(kTestingAreaName, "k1", "v2", kTtl);
      });

  // check key's value is udpated on all stores and version is incremented
  evb.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 300), [&]() noexcept {
        auto maybeThriftVal = store1->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("v2", maybeThriftVal.value().value_ref());

        maybeThriftVal = store2->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("v2", maybeThriftVal.value().value_ref());

        maybeThriftVal = store3->getKey(kTestingAreaName, "k1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ("v2", maybeThriftVal.value().value_ref());
        EXPECT_EQ("node1", *maybeThriftVal.value().originatorId_ref());
        EXPECT_EQ(*maybeThriftVal.value().version_ref(), 3);
      });

  // set empty value on store1, and check for key expiry
  evb.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 10), [&]() noexcept {
        client1->clearKey(kTestingAreaName, "k1", "", kTtl);
      });

  // after kTtl duration key must have been deleted due to ttl expiry
  evb.scheduleTimeout(
      // add 100 msec more to avoid flakiness
      std::chrono::milliseconds(waitDuration += kTtl.count() + 300),
      [&]() noexcept {
        auto maybeThriftVal = store1->getKey(kTestingAreaName, "k1");
        ASSERT_FALSE(maybeThriftVal.has_value());

        maybeThriftVal = store2->getKey(kTestingAreaName, "k1");
        ASSERT_FALSE(maybeThriftVal.has_value());

        maybeThriftVal = store3->getKey(kTestingAreaName, "k1");
        ASSERT_FALSE(maybeThriftVal.has_value());

        // Synchronization primitive
        waitBaton.post();
      });

  // Start the event loop and wait until it is finished execution.
  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  // Stop store
  LOG(INFO) << "Stopping stores";
  store1->stop();
  store2->stop();
  store3->stop();
  client1.reset();

  evb.stop();
  evb.waitUntilStopped();
  evbThread.join();
}

TEST(KvStoreClientInternal, PersistKeyTest) {
  fbzmq::Context context;
  folly::Baton waitBaton;
  const std::string nodeId{"test_store"};

  // Initialize and start KvStore with one fake peer
  auto config = std::make_shared<Config>(getBasicOpenrConfig(nodeId));
  auto store = std::make_shared<KvStoreWrapper>(context, config);
  store->run();

  // Create another OpenrEventBase instance for looping clients
  OpenrEventBase evb;

  // Create and initialize kvstore-client, with persist key timer
  auto client1 = std::make_shared<KvStoreClientInternal>(
      &evb, nodeId, store->getKvStore(), 1000ms);

  // Schedule callback to set keys from client1 (this will be executed first)
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    client1->persistKey(kTestingAreaName, "test_key3", "test_value3");
  });

  // Schedule callback to get persist key from client1
  evb.scheduleTimeout(std::chrono::milliseconds(2), [&]() noexcept {
    // 1st get key
    auto maybeVal1 = client1->getKey(kTestingAreaName, "test_key3");

    ASSERT_TRUE(maybeVal1.has_value());
    EXPECT_EQ(1, *maybeVal1->version_ref());
    EXPECT_EQ("test_value3", maybeVal1->value_ref());
  });

  // simulate kvstore restart by erasing the test_key3
  // set a TTL of 1ms in the store so that it gets deleted before refresh
  // event
  evb.scheduleTimeout(std::chrono::milliseconds(3), [&]() noexcept {
    auto keyExpVal = createThriftValue(
        1,
        nodeId,
        std::string("test_value3"),
        1, /* ttl in msec */
        500 /* ttl version */,
        0 /* hash */);
    store->setKey(kTestingAreaName, "test_key3", keyExpVal, std::nullopt);
  });

  // check after few ms if key is deleted,
  evb.scheduleTimeout(std::chrono::milliseconds(60), [&]() noexcept {
    auto maybeVal3 = client1->getKey(kTestingAreaName, "test_key3");
    ASSERT_FALSE(maybeVal3.has_value());
  });

  // Schedule after a second, key will be erased and set back in kvstore
  // with persist key check callback
  evb.scheduleTimeout(std::chrono::milliseconds(3000), [&]() noexcept {
    auto maybeVal3 = client1->getKey(kTestingAreaName, "test_key3");
    ASSERT_TRUE(maybeVal3.has_value());
    EXPECT_EQ(1, *maybeVal3->version_ref());
    EXPECT_EQ("test_value3", maybeVal3->value_ref());

    // Synchronization primitive
    waitBaton.post();
  });

  // Start the event loop and wait until it is finished execution.
  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  // Stop store
  LOG(INFO) << "Stopping store";
  store->closeQueue();
  client1.reset();
  store->stop();
  store.reset();

  evb.stop();
  evb.waitUntilStopped();
  evbThread.join();
}

/**
 * Test ttl change with persist key while keeping value and version same
 * - Set key with ttl 1s
 *   - Verify key is set and remains after ttl + 1s (2s)
 *   - Verify "1s < ttl"
 * - Update key with ttl 3s
 *   - Verify key remains set after ttl + 1s (4s)
 *   - Verify "1s < ttl < 3s"
 * - Update key with ttl 1s
 *   - Verify key remains set after ttl + 1s (2s)
 *   - Verify "1s < ttl"
 */
TEST(KvStoreClientInternal, PersistKeyChangeTtlTest) {
  fbzmq::Context context;
  folly::Baton waitBaton;
  const std::string nodeId{"test_store"};
  const std::string testKey{"test-key"};
  const std::string testValue{"test-value"};

  // Initialize and start KvStore
  auto config = std::make_shared<Config>(getBasicOpenrConfig(nodeId));
  auto store = std::make_shared<KvStoreWrapper>(context, config);
  store->run();

  // Create another OpenrEventBase instance for looping clients
  OpenrEventBase evb;

  // Create and initialize kvstore-client, with persist key timer
  auto client1 = std::make_unique<KvStoreClientInternal>(
      &evb, nodeId, store->getKvStore(), 1000ms);

  // Schedule callback to set keys from client1 (this will be executed first)
  evb.scheduleTimeout(std::chrono::seconds(0), [&]() noexcept {
    // Set key with ttl=1s
    client1->persistKey(
        kTestingAreaName, testKey, testValue, std::chrono::seconds(1));
  });

  // Verify key exists after (ttl + 1s) = 2s
  evb.scheduleTimeout(std::chrono::seconds(2), [&]() noexcept {
    // Ensure key exists
    auto maybeVal = client1->getKey(kTestingAreaName, testKey);
    ASSERT_TRUE(maybeVal.has_value());
    EXPECT_EQ(1, *maybeVal->version_ref());
    EXPECT_EQ(testValue, maybeVal->value_ref());
    EXPECT_LT(0, *maybeVal->ttl_ref());
    EXPECT_GE(1000, *maybeVal->ttl_ref());
    EXPECT_LE(6, *maybeVal->ttlVersion_ref()); // can be flaky under stress

    // Set key with higher ttl=3s
    client1->persistKey(
        kTestingAreaName, testKey, testValue, std::chrono::seconds(3));
  });

  // Verify key exists after (ttl + 1s) = 4s (+ 2s offset)
  evb.scheduleTimeout(std::chrono::seconds(6), [&]() noexcept {
    // Ensure key exists
    auto maybeVal = client1->getKey(kTestingAreaName, testKey);
    ASSERT_TRUE(maybeVal.has_value());
    EXPECT_EQ(1, *maybeVal->version_ref());
    EXPECT_EQ(testValue, maybeVal->value_ref());
    EXPECT_LT(1000, *maybeVal->ttl_ref());
    EXPECT_GE(3000, *maybeVal->ttl_ref());
    EXPECT_LE(9, *maybeVal->ttlVersion_ref()); // can be flaky under stress

    // Set key with lower ttl=1s
    client1->persistKey(
        kTestingAreaName, testKey, testValue, std::chrono::seconds(1));
  });

  // Verify key exists after (ttl + 1s) = 2s (+ 6s offset)
  evb.scheduleTimeout(std::chrono::seconds(8), [&]() noexcept {
    // Ensure key exists
    auto maybeVal = client1->getKey(kTestingAreaName, testKey);
    ASSERT_TRUE(maybeVal.has_value());
    EXPECT_EQ(1, *maybeVal->version_ref());
    EXPECT_EQ(testValue, maybeVal->value_ref());
    EXPECT_LT(0, *maybeVal->ttl_ref());
    EXPECT_GE(1000, *maybeVal->ttl_ref());
    EXPECT_LE(12, *maybeVal->ttlVersion_ref()); // can be flaky under stress

    // Synchronization primitive
    waitBaton.post();
  });

  // Start the event loop and wait until it is finished execution.
  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  // Stop store
  LOG(INFO) << "Stopping store";
  store->closeQueue();
  client1.reset();
  store->stop();
  store.reset();

  evb.stop();
  evb.waitUntilStopped();
  evbThread.join();
}

/**
 * Start a store and attach two clients to it. Set some Keys and add/del peers.
 * Verify that changes are visible in KvStore via a separate REQ socket to
 * KvStore. Further key-2 from client-2 should win over key from client-1
 */
TEST(KvStoreClientInternal, ApiTest) {
  fbzmq::Context context;
  const std::string nodeId{"test_store"};

  // Initialize and start KvStore with one fake peer
  auto config = std::make_shared<Config>(getBasicOpenrConfig(nodeId));
  auto store = std::make_shared<KvStoreWrapper>(context, config);
  store->run();

  // Define and start evb for KvStoreClientInternal usage.
  std::unique_ptr<KvStoreClientInternal> client1{nullptr}, client2{nullptr};
  OpenrEventBase openrEvb;
  std::thread openrEvbThread([&]() {
    LOG(INFO) << "Starting openrEvb...";
    openrEvb.run();
    LOG(INFO) << "openrEvb terminated...";
  });
  openrEvb.waitUntilRunning();

  // create kvstore client to interact with KvStore
  openrEvb.getEvb()->runInEventBaseThreadAndWait([&]() {
    client1 = std::make_unique<KvStoreClientInternal>(
        &openrEvb, nodeId, store->getKvStore());
    client2 = std::make_unique<KvStoreClientInternal>(
        &openrEvb, nodeId, store->getKvStore());
  });

  OpenrEventBase evb;

  // Schedule callback to set keys from client1 (this will be executed first)
  openrEvb.getEvb()->runInEventBaseThreadAndWait([&]() {
    client1->persistKey(kTestingAreaName, "test_key1", "test_value1");
    client1->setKey(kTestingAreaName, "test_key2", "test_value2");
  });

  // Schedule callback to add/del peer via client-1 (will be executed next)
  evb.scheduleTimeout(std::chrono::milliseconds(1), [&]() noexcept {
    EXPECT_TRUE(store->addPeer(
        kTestingAreaName, "peer1", createPeerSpec("inproc://fake_cmd_url_1")));
    EXPECT_TRUE(store->addPeer(
        kTestingAreaName, "peer2", createPeerSpec("inproc://fake_cmd_url_2")));
    EXPECT_TRUE(store->delPeer(kTestingAreaName, "peer1"));
  });

  // Schedule callback to persist key2 from client2 (this will be executed next)
  evb.scheduleTimeout(std::chrono::milliseconds(2), [&]() noexcept {
    openrEvb.getEvb()->runInEventBaseThreadAndWait([&]() {
      // 1st get key
      auto maybeVal1 = client2->getKey(kTestingAreaName, "test_key2");
      ASSERT_TRUE(maybeVal1.has_value());
      EXPECT_EQ(1, *maybeVal1->version_ref());
      EXPECT_EQ("test_value2", maybeVal1->value_ref());

      // persistKey with new value
      client2->persistKey(kTestingAreaName, "test_key2", "test_value2-client2");

      // 2nd getkey
      auto maybeVal2 = client2->getKey(kTestingAreaName, "test_key2");
      ASSERT_TRUE(maybeVal2.has_value());
      EXPECT_EQ(2, *maybeVal2->version_ref());
      EXPECT_EQ("test_value2-client2", maybeVal2->value_ref());

      // get key with non-existing key
      auto maybeVal3 = client2->getKey(kTestingAreaName, "test_key3");
      EXPECT_FALSE(maybeVal3);
    });
  });

  evb.scheduleTimeout(std::chrono::milliseconds(3), [&]() noexcept {
    VLOG(1) << "Running timeout for `setKey` test";
    const std::string testKey{"set_test_key"};
    auto testValue = createThriftValue(
        3,
        std::string("originator-id"),
        std::string("set_test_value"),
        Constants::kTtlInfinity, // ttl
        0, // ttl version
        generateHash(
            3,
            "originator-id",
            thrift::Value().value_ref() = "set_test_value"));

    // Sync call to insert key-value into the KvStore
    openrEvb.getEvb()->runInEventBaseThreadAndWait(
        [&]() { client1->setKey(kTestingAreaName, testKey, testValue); });

    // Sync call to get key-value from KvStore
    auto maybeValue = store->getKey(kTestingAreaName, testKey);
    ASSERT_TRUE(maybeValue);
    EXPECT_EQ(testValue, *maybeValue);
  });

  // dump keys
  evb.scheduleTimeout(std::chrono::milliseconds(4), [&]() noexcept {
    openrEvb.getEvb()->runInEventBaseThreadAndWait([&]() {
      const auto maybeKeyVals = client1->dumpAllWithPrefix(kTestingAreaName);
      ASSERT_TRUE(maybeKeyVals.has_value());
      ASSERT_EQ(3, maybeKeyVals->size());
      EXPECT_EQ("test_value1", maybeKeyVals->at("test_key1").value_ref());
      EXPECT_EQ(
          "test_value2-client2", maybeKeyVals->at("test_key2").value_ref());
      EXPECT_EQ("set_test_value", maybeKeyVals->at("set_test_key").value_ref());

      const auto maybeKeyVals2 = client2->dumpAllWithPrefix(kTestingAreaName);
      ASSERT_TRUE(maybeKeyVals2.has_value());
      EXPECT_EQ(*maybeKeyVals, *maybeKeyVals2);

      // dump keys with a given prefix
      const auto maybePrefixedKeyVals =
          client1->dumpAllWithPrefix(kTestingAreaName, "test");
      ASSERT_TRUE(maybePrefixedKeyVals.has_value());
      ASSERT_EQ(2, maybePrefixedKeyVals->size());
      EXPECT_EQ(
          "test_value1", maybePrefixedKeyVals->at("test_key1").value_ref());
      EXPECT_EQ(
          "test_value2-client2",
          maybePrefixedKeyVals->at("test_key2").value_ref());
    });
  });

  // Inject keys w/ TTL
  evb.scheduleTimeout(std::chrono::milliseconds(5), [&]() noexcept {
    openrEvb.getEvb()->runInEventBaseThreadAndWait([&]() {
      auto testValue1 = createThriftValue(
          1,
          nodeId,
          std::string("test_ttl_value1"),
          kTtl.count(), // ttl
          500, // ttl version
          0);
      client1->setKey(kTestingAreaName, "test_ttl_key1", testValue1);

      client1->persistKey(
          kTestingAreaName, "test_ttl_key1", "test_ttl_value1", kTtl);

      client2->setKey(
          kTestingAreaName, "test_ttl_key2", "test_ttl_value2", 1, kTtl);
      auto testValue2 = createThriftValue(
          1,
          nodeId,
          std::string("test_ttl_value2"),
          kTtl.count(), // ttl
          1500, // ttl version
          0);
      client2->setKey(kTestingAreaName, "test_ttl_key2", testValue2);
    });
  });

  // Keys shall not expire even after TTL bcoz client is updating their TTL
  evb.scheduleTimeout(std::chrono::milliseconds(6) + kTtl * 3, [&]() noexcept {
    openrEvb.getEvb()->runInEventBaseThreadAndWait([&]() {
      LOG(INFO) << "received response.";
      auto maybeVal1 = client2->getKey(kTestingAreaName, "test_ttl_key1");
      ASSERT_TRUE(maybeVal1.has_value());
      EXPECT_EQ("test_ttl_value1", maybeVal1->value_ref());
      EXPECT_LT(500, *maybeVal1->ttlVersion_ref());

      auto maybeVal2 = client1->getKey(kTestingAreaName, "test_ttl_key2");
      ASSERT_TRUE(maybeVal2.has_value());
      EXPECT_LT(1500, *maybeVal2->ttlVersion_ref());
      EXPECT_EQ(1, *maybeVal2->version_ref());
      EXPECT_EQ("test_ttl_value2", maybeVal2->value_ref());
    });

    // nuke client to mimick scenario user process dies and no ttl
    // update
    store->closeQueue();
    client1.reset();
    client2.reset();
  });

  evb.scheduleTimeout(std::chrono::milliseconds(7) + kTtl * 6, [&]() noexcept {
    // Verify peers INFO from KvStore
    const auto peersResponse = store->getPeers(kTestingAreaName);
    EXPECT_EQ(1, peersResponse.size());
    EXPECT_EQ(0, peersResponse.count("peer1"));
    EXPECT_EQ(1, peersResponse.count("peer2"));

    // Verify key-value info
    const auto keyValResponse = store->dumpAll(kTestingAreaName);
    LOG(INFO) << "received response.";
    for (const auto& kv : keyValResponse) {
      VLOG(4) << "key: " << kv.first
              << ", val: " << kv.second.value_ref().value();
    }
    ASSERT_EQ(3, keyValResponse.size());

    auto const& value1 = keyValResponse.at("test_key1");
    EXPECT_EQ("test_value1", value1.value_ref());
    EXPECT_EQ(1, *value1.version_ref());

    auto const& value2 = keyValResponse.at("test_key2");
    EXPECT_EQ("test_value2-client2", value2.value_ref());
    EXPECT_LE(2, *value2.version_ref()); // client-2 must win over client-1

    EXPECT_EQ(1, keyValResponse.count("set_test_key"));

    // stop the event loop
    evb.stop();
  });

  evb.run();

  // Stop store
  LOG(INFO) << "Stopping store";
  store->stop();

  // Stop openrEvb
  openrEvb.stop();
  openrEvb.waitUntilStopped();
  openrEvbThread.join();
}

TEST(KvStoreClientInternal, SubscribeApiTest) {
  fbzmq::Context context;
  folly::Baton waitBaton;
  const std::string nodeId{"test_store"};

  // Initialize and start KvStore with empty peer
  auto config = std::make_shared<Config>(getBasicOpenrConfig(nodeId));
  auto store = std::make_shared<KvStoreWrapper>(context, config);
  store->run();

  // Create another OpenrEventBase instance for looping clients
  OpenrEventBase evb;

  // Create and initialize kvstore-clients
  auto client1 = std::make_shared<KvStoreClientInternal>(
      &evb, nodeId, store->getKvStore());
  auto client2 = std::make_shared<KvStoreClientInternal>(
      &evb, nodeId, store->getKvStore());

  int key1CbCnt = 0;
  int key2CbCnt = 0;
  // Schedule callback to set keys from client1 (this will be executed first)
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    client1->subscribeKey(
        kTestingAreaName,
        "test_key1",
        [&](std::string const& k, std::optional<thrift::Value> v) {
          // should be called when client1 call persistKey for test_key1
          EXPECT_EQ("test_key1", k);
          EXPECT_EQ(1, *v.value().version_ref());
          EXPECT_EQ("test_value1", v.value().value_ref());
          key1CbCnt++;
        },
        false);
    client1->subscribeKey(
        kTestingAreaName,
        "test_key2",
        [&](std::string const& k, std::optional<thrift::Value> v) {
          // should be called when client2 call persistKey for test_key2
          EXPECT_EQ("test_key2", k);
          EXPECT_LT(0, *v.value().version_ref());
          EXPECT_GE(2, *v.value().version_ref());
          switch (*v.value().version_ref()) {
          case 1:
            EXPECT_EQ("test_value2", v.value().value_ref());
            break;
          case 2:
            EXPECT_EQ("test_value2-client2", v.value().value_ref());
            break;
          }
          key2CbCnt++;
        },
        false);
    client1->persistKey(kTestingAreaName, "test_key1", "test_value1");
    client1->setKey(kTestingAreaName, "test_key2", "test_value2");
  });

  int key2CbCntClient2{0};

  // Schedule callback to persist key2 from client2 (this will be executed next)
  evb.scheduleTimeout(std::chrono::milliseconds(10), [&]() noexcept {
    client2->persistKey(kTestingAreaName, "test_key2", "test_value2-client2");
    client2->subscribeKey(
        kTestingAreaName,
        "test_key2",
        [&](std::string const&, std::optional<thrift::Value>) {
          // this should never be called when client2 call persistKey
          // for test_key2 with same value
          key2CbCntClient2++;
        },
        false);
    // call persistkey with same value. should not get a callback here.
    client2->persistKey(kTestingAreaName, "test_key2", "test_value2-client2");
  });

  /* test for key callback with the option of getting key Value */
  int keyExpKeySubCbCnt{0}; /* reply count for key regd. with fetchValue=true */
  evb.scheduleTimeout(std::chrono::milliseconds(11), [&]() noexcept {
    client2->setKey(
        kTestingAreaName, "test_key_subs_cb", "test_key_subs_cb_val", 11);

    std::optional<thrift::Value> keyValue;
    keyValue = client2->subscribeKey(
        kTestingAreaName,
        "test_key_subs_cb",
        [&](std::string const&, std::optional<thrift::Value>) {},
        true);
    if (keyValue.has_value()) {
      EXPECT_EQ("test_key_subs_cb_val", keyValue.value().value_ref());
      keyExpKeySubCbCnt++;
    }
  });
  /* test for expired keys update */
  int keyExpKeyCbCnt{0}; /* expired key call back count specific to a key */
  int keyExpCbCnt{0}; /* expired key call back count */
  evb.scheduleTimeout(std::chrono::milliseconds(20), [&]() noexcept {
    thrift::Value keyExpVal{
        apache::thrift::FRAGILE, 1, nodeId, "test_key_exp_val", 1, 500, 0};

    client2->setKvCallback([&](
        const std::string& key,
        std::optional<thrift::Value> thriftVal) noexcept {
      if (!thriftVal.has_value()) {
        EXPECT_EQ("test_key_exp", key);
        keyExpCbCnt++;
      }
    });

    client2->subscribeKey(
        kTestingAreaName,
        "test_key_exp",
        [&](std::string const& k, std::optional<thrift::Value> v) {
          if (!v.has_value()) {
            EXPECT_EQ("test_key_exp", k);
            keyExpKeyCbCnt++;
            // Synchronization primitive
            waitBaton.post();
          }
        },
        false);

    store->setKey(kTestingAreaName, "test_key_exp", keyExpVal, std::nullopt);
  });

  // Start the event loop
  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  // Verify out expectations
  EXPECT_EQ(1, key1CbCnt);
  EXPECT_GE(2, key2CbCnt); // This can happen when KvStore processes request
  EXPECT_LE(1, key2CbCnt); // from two clients in out of order. However values
                           // are going to be same.
  EXPECT_EQ(0, key2CbCntClient2);
  EXPECT_EQ(1, keyExpCbCnt);
  EXPECT_EQ(1, keyExpKeyCbCnt);
  EXPECT_EQ(1, keyExpKeySubCbCnt);

  // Stop server
  LOG(INFO) << "Stopping store";
  store->stop();

  // reset client before queue closing
  client1.reset();
  client2.reset();

  evb.stop();
  evb.waitUntilStopped();
  evbThread.join();
}

TEST(KvStoreClientInternal, SubscribeKeyFilterApiTest) {
  fbzmq::Context context;
  folly::Baton waitBaton;
  const std::string nodeId{"test_store"};

  // Initialize and start KvStore with empty peer
  auto config = std::make_shared<Config>(getBasicOpenrConfig(nodeId));
  auto store = std::make_shared<KvStoreWrapper>(context, config);
  store->run();

  // Create another OpenrEventBase instance for looping clients
  OpenrEventBase evb;

  // Create and initialize kvstore-clients
  auto client1 = std::make_unique<KvStoreClientInternal>(
      &evb, nodeId, store->getKvStore());

  std::vector<std::string> keyPrefixList;
  keyPrefixList.emplace_back("test_");
  std::set<std::string> originatorIds{};
  KvStoreFilters kvFilters = KvStoreFilters(keyPrefixList, originatorIds);

  int key1CbCnt = 0;
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    // subscribe for key update for keys using kvstore filter
    // using store->setKey should trigger the callback, key1CbCnt++
    client1->subscribeKeyFilter(
        std::move(kvFilters),
        [&](std::string const& k, std::optional<thrift::Value> v) {
          // this should be called when client call persistKey for
          // test_key1
          EXPECT_THAT(k, testing::StartsWith("test_"));
          EXPECT_EQ(1, *v.value().version_ref());
          EXPECT_EQ("test_key_val", v.value().value_ref());
          key1CbCnt++;
        });

    thrift::Value testValue1 = createThriftValue(
        1,
        nodeId,
        std::string("test_key_val"),
        10000, /* ttl in msec */
        500 /* ttl version */,
        0 /* hash */);
    store->setKey(kTestingAreaName, "test_key1", testValue1, std::nullopt);
  });

  // subscribe for key update for keys using kvstore filter
  // using kvstoreClient->setKey(), this shouldn't trigger update as the
  // key will be in persistent DB. (key1CbCnt - shoudln't change)
  evb.scheduleTimeout(std::chrono::milliseconds(25), [&]() noexcept {
    client1->persistKey(kTestingAreaName, "test_key1", "test_value2");
  });

  // add another key with same prefix, different key string, key1CbCnt++
  evb.scheduleTimeout(std::chrono::milliseconds(50), [&]() noexcept {
    thrift::Value testValue1 = createThriftValue(
        1,
        nodeId,
        std::string("test_key_val"),
        10000, /* ttl in msec */
        500 /* ttl version */,
        0 /* hash */);
    store->setKey(kTestingAreaName, "test_key2", testValue1, std::nullopt);
  });

  // unsubscribe kvstore key filter and test for callback
  evb.scheduleTimeout(std::chrono::milliseconds(100), [&]() noexcept {
    client1->unsubscribeKeyFilter();
  });

  // add another key with same prefix, after unsubscribing,
  // key callback count will not increase
  evb.scheduleTimeout(std::chrono::milliseconds(150), [&]() noexcept {
    thrift::Value testValue1 = createThriftValue(
        1,
        nodeId,
        std::string("test_key_val"),
        10000, /* ttl in msec */
        500 /* ttl version */,
        0 /* hash */);
    store->setKey(kTestingAreaName, "test_key3", testValue1, std::nullopt);

    // Synchronization primitive
    waitBaton.post();
  });

  // Start the event loop
  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  // count must be 2
  EXPECT_EQ(2, key1CbCnt);

  // Stop server
  LOG(INFO) << "Stopping store";
  store->stop();
  client1.reset();

  evb.stop();
  evb.waitUntilStopped();
  evbThread.join();
}

/*
 * area related tests for KvStoreClientInternal. Things to test:
 * - Flooding is contained within area - basic verification
 * - setKey, getKey, clearKey, unsetKey
 * - key TTL refresh, key expiry
 *
 * Topology:
 *
 *  node1(pod-area)  --- (pod area) node2 (plane area) -- (plane area) node3
 */

TEST_F(MultipleAreaFixture, MultipleAreasPeers) {
  auto scheduleAt = std::chrono::milliseconds{0}.count();
  folly::Baton waitBaton;

  evb.scheduleTimeout(std::chrono::milliseconds(scheduleAt), [&]() noexcept {
    // test addPeers in invalid area, following result must be false
    for (auto& peer : peers1) {
      EXPECT_FALSE(store1->addPeer(kTestingAreaName, peer.first, peer.second));
    }
    for (auto& peer : peers2PlaneArea) {
      EXPECT_FALSE(store2->addPeer(kTestingAreaName, peer.first, peer.second));
    }
    for (auto& peer : peers3) {
      EXPECT_FALSE(store3->addPeer(kTestingAreaName, peer.first, peer.second));
    }
    // add peers in valid area,
    // node1(pod-area)  --- (pod area) node2 (plane area) -- (plane area) node3
    setUpPeers();
  });

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 50), [&]() noexcept {
        // test addPeers
        auto maybePeers = store1->getPeers(planeArea);
        EXPECT_EQ(maybePeers, peers1);

        auto maybePeers2 = store2->getPeers(planeArea);
        EXPECT_EQ(maybePeers2, peers2PlaneArea);

        auto maybePeers3 = store2->getPeers(podArea);
        EXPECT_EQ(maybePeers3, peers2PodArea);

        auto maybePeers4 = store3->getPeers(podArea);
        EXPECT_EQ(maybePeers4, peers3);
      });

  // test for key set, get and key flood within area
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 50), [&]() noexcept {
        thrift::Value valuePlane1;
        valuePlane1.version_ref() = 1;
        valuePlane1.value_ref() = "test_value1";
        // key set within invalid area, must return false
        EXPECT_FALSE(client1
                         ->setKey(
                             kTestingAreaName,
                             "plane_key1",
                             writeThriftObjStr(valuePlane1, serializer),
                             100,
                             Constants::kTtlInfInterval)
                         .has_value());

        EXPECT_TRUE(client1
                        ->setKey(
                            planeArea,
                            "plane_key1",
                            writeThriftObjStr(valuePlane1, serializer),
                            100,
                            Constants::kTtlInfInterval)
                        .has_value());

        // set key in pod are on node3
        thrift::Value valuePod1;
        valuePod1.version_ref() = 1;
        valuePod1.value_ref() = "test_value1";
        EXPECT_TRUE(client3
                        ->setKey(
                            podArea,
                            "pod_key1",
                            writeThriftObjStr(valuePlane1, serializer),
                            100,
                            Constants::kTtlInfInterval)
                        .has_value());
      });

  // get keys from pod and play area and ensure keys are not leaked across
  // areas
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 50), [&]() noexcept {
        // get key from default area, must be false
        auto maybeThriftVal1 = store1->getKey(kTestingAreaName, "pod_key1");
        ASSERT_FALSE(maybeThriftVal1.has_value());

        // get pod key from plane area, must be false
        auto maybeThriftVal2 = store1->getKey(planeArea, "pod_key1");
        ASSERT_FALSE(maybeThriftVal2.has_value());

        // get plane key from pod area, must be false
        auto maybeThriftVal3 = store3->getKey(podArea, "plane_key1");
        ASSERT_FALSE(maybeThriftVal3.has_value());

        // get pod key from pod area from store2, verifies flooding
        auto maybeThriftVal4 = store2->getKey(podArea, "pod_key1");
        ASSERT_TRUE(maybeThriftVal4.has_value());

        // get plane key from plane area from store2, verifies flooding
        auto maybeThriftVal5 = store2->getKey(planeArea, "plane_key1");
        ASSERT_TRUE(maybeThriftVal5.has_value());

        // Synchronization primitive
        waitBaton.post();
      });

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();
}

TEST_F(MultipleAreaFixture, MultipleAreaKeyExpiry) {
  const std::chrono::milliseconds ttl{Constants::kTtlThreshold.count() + 100};
  auto scheduleAt = std::chrono::milliseconds{0}.count();
  folly::Baton waitBaton;

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt), [&]() noexcept { setUpPeers(); });

  // add key in plane and pod area into node1 and node3 respectively
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 10), [&]() noexcept {
        EXPECT_TRUE(client1
                        ->setKey(
                            planeArea,
                            "test_ttl_key_plane",
                            "test_ttl_value_plane",
                            1,
                            ttl)
                        .has_value());
        client3->persistKey(
            podArea, "test_ttl_key_pod", "test_ttl_value_pod", ttl);
      });

  // check if key is flooding as expected by checking in node2
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 50), [&]() noexcept {
        // plane key must be present in node2 (plane area) and not in node3
        EXPECT_TRUE(
            client2->getKey(planeArea, "test_ttl_key_plane").has_value());
        EXPECT_FALSE(
            client3->getKey(podArea, "test_ttl_key_plane").has_value());

        // pod key - should present in node2 (Pod area) and absent in node1
        EXPECT_TRUE(client2->getKey(podArea, "test_ttl_key_pod").has_value());
        EXPECT_FALSE(
            client1->getKey(planeArea, "test_ttl_key_pod").has_value());
      });

  // schecdule after 2 * TTL, check key refresh is working fine
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += ttl.count() * 2), [&]() noexcept {
        // plane key must be present
        EXPECT_TRUE(
            client2->getKey(planeArea, "test_ttl_key_plane").has_value());

        // pod key must be present
        EXPECT_TRUE(client2->getKey(podArea, "test_ttl_key_pod").has_value());
        EXPECT_TRUE(client3->getKey(podArea, "test_ttl_key_pod").has_value());
      });

  // verify dumpAllWithThriftClientFromMultiple
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 10), [&]() noexcept {
        auto maybe = dumpAllWithThriftClientFromMultiple(
            planeArea,
            sockAddrs_,
            "test_",
            Constants::kServiceConnTimeout,
            Constants::kServiceProcTimeout,
            nullptr, /* disable SSLSocket Connection for UT */
            192, /* IP_TOS */
            folly::AsyncSocket::anyAddress());
        // there will be plane area key "test_ttl_key_plane"
        ASSERT_TRUE(maybe.first.has_value());
        EXPECT_EQ(maybe.first.value().size(), 1);

        // only one key in pod Area too, "test_ttl_pod_area"
        maybe = dumpAllWithThriftClientFromMultiple(
            podArea,
            sockAddrs_,
            "test_",
            Constants::kServiceConnTimeout,
            Constants::kServiceProcTimeout,
            nullptr, /* disable SSLSocket Connection for UT */
            192, /* IP_TOS */
            folly::AsyncSocket::anyAddress());
        // there will be plane area key "test_ttl_key_plane"
        ASSERT_TRUE(maybe.first.has_value());
        EXPECT_EQ(maybe.first.value().size(), 1);
      });

  // unset key, this stops key ttl refresh
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 10), [&]() noexcept {
        // plane key must be present
        client1->unsetKey(planeArea, "test_ttl_key_plane");
        client3->unsetKey(podArea, "test_ttl_key_pod");
      });

  // schecdule after 2 * TTL - keys should not be present as they've expired
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += ttl.count() * 2), [&]() noexcept {
        // keys should be expired now
        EXPECT_FALSE(
            client2->getKey(planeArea, "test_ttl_key_plane").has_value());

        // pod key must be present
        EXPECT_FALSE(client2->getKey(podArea, "test_ttl_key_pod").has_value());
        EXPECT_FALSE(client3->getKey(podArea, "test_ttl_key_pod").has_value());

        // Synchronization primitive
        waitBaton.post();
      });

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();
}

/*
 * this test checks if the checkPersistKeyInStore() works when multiple
 * areas are instantiated in the KvStore, with one area having emtpy
 * persistKeyDB.
 *
 * 1. add key in node2 by calling persistKey()
 * 2. use the kvstore API to delete the key in node2 be setting a short TTL
 * 3. verify key is deleted from node2 kvstore
 * 4. wait until checkPersistKeyInStore() kicks in to repopulate the key
 * 5. verify kvstore in node2 has the key
 */
TEST_F(MultipleAreaFixture, PersistKeyArea) {
  const std::chrono::milliseconds ttl{Constants::kTtlThreshold.count() + 100};
  auto scheduleAt = std::chrono::milliseconds{0}.count();
  folly::Baton waitBaton;

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt), [&]() noexcept { setUpPeers(); });

  // add key in plane area of node2, no keys are present in pod area
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 10), [&]() noexcept {
        client2->persistKey(
            planeArea, "test_ttl_key_plane", "test_ttl_value_plane", ttl);
      });

  // verify at node1 that key is flooded in plane area
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 50), [&]() noexcept {
        EXPECT_TRUE(
            client1->getKey(planeArea, "test_ttl_key_plane").has_value());
      });

  // expire the key in node2 kvstore by setting a low ttl value
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 10), [&]() noexcept {
        thrift::Value keyExpVal = createThriftValue(
            1,
            node2,
            std::string("test_ttl_value_plane"),
            1, /* ttl in msec */
            500 /* ttl version */,
            0 /* hash */);

        store2->setKey(
            planeArea, "test_ttl_key_plane", keyExpVal, std::nullopt);
      });

  // key is expired in node2
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 1), [&]() noexcept {
        EXPECT_FALSE(
            client2->getKey(planeArea, "test_ttl_key_plane").has_value());
      });

  // checkPersistKey should kick in and repopulate the key node1 kvstore,
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += persistKeyTimer.count() + 500),
      [&]() noexcept {
        EXPECT_TRUE(
            client2->getKey(planeArea, "test_ttl_key_plane").has_value());
        // Synchronization primitive
        waitBaton.post();
      });

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return 1;
  }

  // Run the tests
  return RUN_ALL_TESTS();
}
