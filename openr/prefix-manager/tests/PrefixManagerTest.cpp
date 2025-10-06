/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/IPAddress.h>
#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/decision/RibEntry.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

using apache::thrift::CompactSerializer;

namespace {

const std::chrono::milliseconds kRouteUpdateTimeout =
    std::chrono::milliseconds(500);

const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto addr4 = toIpPrefix("::ffff:10.4.4.4/128");
const auto addr5 = toIpPrefix("ffff:10:1:5::/64");
const auto addr6 = toIpPrefix("ffff:10:2:6::/64");
const auto addr7 = toIpPrefix("ffff:10:3:7::0/64");
const auto addr8 = toIpPrefix("ffff:10:4:8::/64");
const auto addr9 = toIpPrefix("ffff:10:4:9::/64");
const auto addr10 = toIpPrefix("ffff:10:4:10::/64");

const auto prefixEntry1 = createPrefixEntry(addr1, thrift::PrefixType::DEFAULT);
const auto prefixEntry2 =
    createPrefixEntry(addr2, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry3 = createPrefixEntry(addr3, thrift::PrefixType::DEFAULT);
const auto prefixEntry4 =
    createPrefixEntry(addr4, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry5 = createPrefixEntry(addr5, thrift::PrefixType::DEFAULT);
const auto prefixEntry6 =
    createPrefixEntry(addr6, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry7 = createPrefixEntry(addr7, thrift::PrefixType::BGP);
const auto prefixEntry8 =
    createPrefixEntry(addr8, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry9 = createPrefixEntry(addr9, thrift::PrefixType::VIP);
const auto prefixEntry1Bgp = createPrefixEntry(addr1, thrift::PrefixType::BGP);
} // namespace

class PrefixManagerTestFixture : public testing::Test {
 public:
  void
  SetUp() override {
    // Reset all global counters
    fb303::fbData->resetAllData();

    // create KvStore + PrefixManager instances for testing
    initKvStoreWithPrefixManager();

    // trigger initialization sequence before writing to KvStore
    triggerInitializationEventForPrefixManager(
        fibRouteUpdatesQueue, kvStoreWrapper->getKvStoreUpdatesQueueWriter());

    // wait for PREFIX_DB_SYNCED event being published
    waitForPrefixDbSyncedEvent();
  }

  void
  TearDown() override {
    // Close queues
    closeQueue();

    prefixManager->stop();
    prefixManagerThread->join();
    prefixManager.reset();

    // stop the kvStore
    kvStoreWrapper->stop();
    kvStoreWrapper.reset();

    // stop evbThread
    if (evb.isRunning()) {
      evb.stop();
      evb.waitUntilStopped();
      evbThread.join();
    }
  }

  void
  closeQueue() {
    kvRequestQueue.close();
    prefixUpdatesQueue.close();
    prefixMgrInitializationEventsQueue.close();
    staticRouteUpdatesQueue.close();
    fibRouteUpdatesQueue.close();
    kvStoreWrapper->closeQueue();
  }

  void
  openQueue() {
    kvRequestQueue.open();
    prefixUpdatesQueue.open();
    prefixMgrInitializationEventsQueue.open();
    staticRouteUpdatesQueue.open();
    fibRouteUpdatesQueue.open();
    kvStoreWrapper->openQueue();
  }

  void
  initKvStoreWithPrefixManager() {
    // Register the read in the beginning of the test
    earlyStaticRoutesReaderPtr =
        std::make_shared<messaging::RQueue<DecisionRouteUpdate>>(
            staticRouteUpdatesQueue.getReader());

    // create config
    config = std::make_shared<Config>(createConfig());

    // spin up a kvstore
    kvStoreWrapper =
        std::make_shared<KvStoreWrapper<thrift::OpenrCtrlCppAsyncClient>>(
            config->getAreaIds(), /* areaId collection */
            config->toThriftKvStoreConfig(), /* thrift::KvStoreConfig */
            std::nullopt, /* peerUpdatesQueue */
            kvRequestQueue.getReader() /* kvRequestQueue */);
    kvStoreWrapper->run();

    // spin up a prefix-mgr
    createPrefixManager(config);
  }

  void
  createPrefixManager(std::shared_ptr<Config> cfg) {
    // start a prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        staticRouteUpdatesQueue,
        kvRequestQueue,
        prefixMgrInitializationEventsQueue,
        kvStoreWrapper->getReader(),
        prefixUpdatesQueue.getReader(),
        fibRouteUpdatesQueue.getReader(),
        cfg);

    prefixManagerThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "PrefixManager thread starting";
      prefixManager->run();
      LOG(INFO) << "PrefixManager thread finishing";
    });
    prefixManager->waitUntilRunning();
  }

  void
  waitForPrefixDbSyncedEvent() {
    // ATTN: make sure PREFIX_DB_SYNC event received.
    initializationEventsQueueReader.get(); // perform read

    XLOG(INFO) << "Successfully published PREFIX_DB_SYNC event";
  }

  virtual thrift::OpenrConfig
  createConfig() {
    auto tConfig = getBasicOpenrConfig(nodeId_);
    return tConfig;
  }

  // Get number of `advertised` prefixes
  uint32_t
  getNumPrefixes(const std::string& keyPrefix) {
    uint32_t count = 0;
    auto keyVals = kvStoreWrapper->dumpAll(
        kTestingAreaName, KvStoreFilters({keyPrefix}, {} /* originatorIds */));
    for (const auto& [_, val] : keyVals) {
      if (!val.value()) {
        continue;
      }

      auto prefixDb =
          readThriftObjStr<thrift::PrefixDatabase>(*val.value(), serializer);
      if (*prefixDb.deletePrefix()) {
        // skip prefixes marked for delete
        continue;
      }
      count += 1;
    }
    return count;
  }

  std::shared_ptr<messaging::RQueue<DecisionRouteUpdate>>
  getEarlyStaticRoutesReaderPtr() {
    return earlyStaticRoutesReaderPtr;
  }

  std::optional<thrift::RouteDatabaseDelta>
  waitForRouteUpdate(
      messaging::RQueue<DecisionRouteUpdate>& reader,
      std::optional<thrift::PrefixType> expectedPrefixType = std::nullopt,
      std::chrono::milliseconds timeout = kRouteUpdateTimeout) {
    auto startTime = std::chrono::steady_clock::now();
    while (!reader.size()) {
      // break of timeout occurs
      auto now = std::chrono::steady_clock::now();
      if (now - startTime > timeout) {
        return std::nullopt;
      }
      // Yield the thread
      std::this_thread::yield();
    }
    auto readValue = reader.get().value();
    if (expectedPrefixType.has_value()) {
      EXPECT_EQ(readValue.prefixType, expectedPrefixType.value());
    }
    return readValue.toThrift();
  }

  // NodeId to initialize
  const std::string nodeId_{"node-1"};

  OpenrEventBase evb;
  std::thread evbThread;

  // Queue for publishing entries to PrefixManager
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<thrift::InitializationEvent>
      prefixMgrInitializationEventsQueue;
  messaging::RQueue<thrift::InitializationEvent>
      initializationEventsQueueReader{
          prefixMgrInitializationEventsQueue.getReader()};
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue;

  // Create the serializer for write/read
  CompactSerializer serializer;
  std::shared_ptr<Config> config{nullptr};
  std::unique_ptr<PrefixManager> prefixManager{nullptr};
  std::unique_ptr<std::thread> prefixManagerThread{nullptr};
  std::shared_ptr<KvStoreWrapper<thrift::OpenrCtrlCppAsyncClient>>
      kvStoreWrapper{nullptr};

 private:
  std::shared_ptr<messaging::RQueue<DecisionRouteUpdate>>
      earlyStaticRoutesReaderPtr;
};

TEST_F(PrefixManagerTestFixture, AddRemovePrefix) {
  // Expect no throw
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry1}).get());
  EXPECT_FALSE(prefixManager->advertisePrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry1}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry2}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry4}).get());
  EXPECT_FALSE(prefixManager->advertisePrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry2}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry4}).get());
  EXPECT_TRUE(
      prefixManager
          ->advertisePrefixes({prefixEntry1, prefixEntry2, prefixEntry3})
          .get());
  EXPECT_TRUE(
      prefixManager->withdrawPrefixes({prefixEntry1, prefixEntry2}).get());
  EXPECT_FALSE(
      prefixManager->withdrawPrefixes({prefixEntry1, prefixEntry2}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry4}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry1}).get());
}

TEST_F(PrefixManagerTestFixture, RemoveUpdateType) {
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry1}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry2}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry3}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry4}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry5}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry6}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry7}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry8}).get());
  EXPECT_TRUE(prefixManager->advertisePrefixes({prefixEntry9}).get());

  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry1}).get());
  EXPECT_TRUE(
      prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT).get());
  // can't withdraw twice
  EXPECT_FALSE(
      prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT).get());

  // all the DEFAULT type should be gone
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry3}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry5}).get());

  // The PREFIX_ALLOCATOR type should still be there to be withdrawed
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry2}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry4}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry6}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry8}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry9}).get());

  EXPECT_FALSE(
      prefixManager
          ->withdrawPrefixesByType(thrift::PrefixType::PREFIX_ALLOCATOR)
          .get());

  // update all allocated prefixes
  EXPECT_TRUE(
      prefixManager->advertisePrefixes({prefixEntry2, prefixEntry4}).get());

  // Test sync logic
  EXPECT_TRUE(prefixManager
                  ->syncPrefixesByType(
                      thrift::PrefixType::PREFIX_ALLOCATOR,
                      {prefixEntry6, prefixEntry8})
                  .get());
  EXPECT_FALSE(prefixManager
                   ->syncPrefixesByType(
                       thrift::PrefixType::PREFIX_ALLOCATOR,
                       {prefixEntry6, prefixEntry8})
                   .get());

  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry2}).get());
  EXPECT_FALSE(prefixManager->withdrawPrefixes({prefixEntry4}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry6}).get());
  EXPECT_TRUE(prefixManager->withdrawPrefixes({prefixEntry8}).get());
}

TEST_F(PrefixManagerTestFixture, VerifyKvStore) {
  int scheduleAt{0};
  auto prefixKey =
      PrefixKey(nodeId_, toIPNetwork(*prefixEntry1.prefix()), kTestingAreaName);
  auto keyStr = prefixKey.getPrefixKeyV2();
  auto prefixDbMarker = Constants::kPrefixDbMarker.toString() + nodeId_;

  // Schedule callbacks to run sequentially
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry1}).get();
      });

  // Throttling can come from:
  //  - `syncKvStore()` inside `PrefixManager`
  //  - `persistKey()` inside `KvStoreClientInternal`
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Wait for throttled update to announce to kvstore
        auto maybeValue = kvStoreWrapper->getKey(kTestingAreaName, keyStr);
        EXPECT_TRUE(maybeValue.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value().value(), serializer);
        EXPECT_EQ(*db.thisNodeName(), nodeId_);
        EXPECT_EQ(db.prefixEntries()->size(), 1);

        prefixManager->withdrawPrefixes({prefixEntry1}).get();
        prefixManager->advertisePrefixes({prefixEntry2}).get();
        prefixManager->advertisePrefixes({prefixEntry3}).get();
        prefixManager->advertisePrefixes({prefixEntry4}).get();
        prefixManager->advertisePrefixes({prefixEntry5}).get();
        prefixManager->advertisePrefixes({prefixEntry6}).get();
        prefixManager->advertisePrefixes({prefixEntry7}).get();
        prefixManager->advertisePrefixes({prefixEntry8}).get();
        prefixManager->advertisePrefixes({prefixEntry9}).get();
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Wait for throttled update to announce to kvstore
        auto maybeValue2 = kvStoreWrapper->getKey(kTestingAreaName, keyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        auto db2 = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue2.value().value().value(), serializer);
        EXPECT_EQ(8, getNumPrefixes(prefixDbMarker));
        // now make a change and check again
        prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT)
            .get();
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Wait for throttled update to announce to kvstore
        auto maybeValue3 = kvStoreWrapper->getKey(kTestingAreaName, keyStr);
        EXPECT_TRUE(maybeValue3.has_value());
        auto db3 = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue3.value().value().value(), serializer);
        EXPECT_EQ(6, getNumPrefixes(prefixDbMarker));

        evb.stop();
      });

  // let magic happen
  evb.run();
}

TEST_F(PrefixManagerTestFixture, CounterInitialization) {
  auto counters = fb303::fbData->getCounters();

  // Verify the counter keys exist
  ASSERT_TRUE(counters.count("prefix_manager.route_advertisements.sum"));
  ASSERT_TRUE(counters.count("prefix_manager.route_withdraws.sum"));
  ASSERT_TRUE(counters.count("prefix_manager.originated_routes.sum"));
  ASSERT_TRUE(counters.count("prefix_manager.rejected.sum"));

  // Verify the value of counter keys
  EXPECT_EQ(0, counters.at("prefix_manager.route_advertisements.sum"));
  EXPECT_EQ(0, counters.at("prefix_manager.route_withdraws.sum"));
  EXPECT_EQ(0, counters.at("prefix_manager.originated_routes.sum"));
  EXPECT_EQ(0, counters.at("prefix_manager.rejected.sum"));
}

/**
 * Test prefix advertisement in KvStore with multiple clients.
 * NOTE: Priority LOOPBACK > DEFAULT
 * 1. Inject prefix1 with client-loopback and client-default - Verify KvStore
 * 2. Withdraw prefix1 with client-loopback - Verify KvStore
 * 3. Withdraw prefix1 with client-default - Verify KvStore
 */
TEST_F(PrefixManagerTestFixture, VerifyKvStoreMultipleClients) {
  // Order of prefix-entries -> loopback > default
  const auto loopback_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::LOOPBACK, createMetrics(200, 0, 0));
  const auto default_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(100, 0, 0));

  auto keyStr =
      PrefixKey(nodeId_, toIPNetwork(addr1), kTestingAreaName).getPrefixKeyV2();

  std::optional<thrift::PrefixEntry> expectedPrefix;

  auto cb = [&](std::optional<thrift::Value> val) noexcept {
    ASSERT_TRUE(val.has_value());
    auto db = readThriftObjStr<thrift::PrefixDatabase>(
        val->value().value(), serializer);
    EXPECT_EQ(*db.thisNodeName(), nodeId_);
    if (expectedPrefix.has_value() && db.prefixEntries()->size() != 0) {
      // we should always be advertising one prefix until we withdraw all
      EXPECT_EQ(db.prefixEntries()->size(), 1);
      EXPECT_EQ(expectedPrefix, db.prefixEntries()->at(0))
          << "expeceted: "
          << apache::thrift::util::enumNameSafe(*expectedPrefix->type())
          << " actual: "
          << apache::thrift::util::enumNameSafe(
                 *db.prefixEntries()->at(0).type());
    } else {
      EXPECT_TRUE(*db.deletePrefix());
      EXPECT_TRUE(db.prefixEntries()->size() == 1);
    }
  };

  auto q = kvStoreWrapper->getKvStore()->getKvStoreUpdatesReader();
  //
  // 1. Inject prefix1 with client-loopback and client-default - Verify KvStore
  //
  expectedPrefix = loopback_prefix; // lowest client-id will win
  prefixManager->advertisePrefixes({loopback_prefix, default_prefix}).get();
  auto maybePub = q.get();
  folly::variant_match(
      std::move(maybePub).value(),
      [&](thrift::Publication&& pub) {
        for (auto const& [key, rcvdValue] : *pub.keyVals()) {
          cb(rcvdValue);
        }
      },
      [](thrift::InitializationEvent&&) {
        // Do not interested in initialization event
      });

  //
  // 2. Withdraw prefix1 with client-loopback - Verify KvStore
  //
  expectedPrefix = default_prefix;
  prefixManager->withdrawPrefixes({loopback_prefix}).get();
  maybePub = q.get();
  folly::variant_match(
      std::move(maybePub).value(),
      [&](thrift::Publication&& pub) {
        for (auto const& [key, rcvdValue] : *pub.keyVals()) {
          cb(rcvdValue);
        }
      },
      [](thrift::InitializationEvent&&) {
        // Do not interested in initialization event
      });

  //
  // 3. Withdraw prefix1 with client-default - Verify KvStore
  //
  expectedPrefix = std::nullopt;
  prefixManager->withdrawPrefixes({default_prefix}).get();
  maybePub = q.get();
  folly::variant_match(
      std::move(maybePub).value(),
      [&](thrift::Publication&& pub) {
        for (auto const& [key, rcvdValue] : *pub.keyVals()) {
          cb(rcvdValue);
        }
      },
      [](thrift::InitializationEvent&&) {
        // Do not interested in initialization event
      });
}

/**
 * test to check prefix key add, withdraw does not trigger update for all
 * the prefixes managed by the prefix manager. This test does not apply to
 * the old key format
 */
TEST_F(PrefixManagerTestFixture, PrefixKeyUpdates) {
  int scheduleAt{0};
  auto prefixKey1 = PrefixKey(
      nodeId_,
      folly::IPAddress::createNetwork(toString(*prefixEntry1.prefix())),
      kTestingAreaName);
  auto prefixKey2 = PrefixKey(
      nodeId_,
      folly::IPAddress::createNetwork(toString(*prefixEntry2.prefix())),
      kTestingAreaName);

  // Schedule callbacks to run at fixes timestamp
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry1}).get();
      });

  // Throttling can come from:
  //  - `syncKvStore()` inside `PrefixManager`
  //  - `persistKey()` inside `KvStoreClientInternal`
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKeyV2();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version(), 1);

        // add another key
        prefixManager->advertisePrefixes({prefixEntry2}).get();
      });

  // version of first key should still be 1
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 4 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKeyV2();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version(), 1);

        prefixKeyStr = prefixKey2.getPrefixKeyV2();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        EXPECT_EQ(*maybeValue2.value().version(), 1);

        // withdraw prefixEntry2
        prefixManager->withdrawPrefixes({prefixEntry2}).get();
      });

  // version of prefixEntry1 should still be 1
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKeyV2();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version(), 1);

        // verify key is withdrawn
        prefixKeyStr = prefixKey2.getPrefixKeyV2();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue2.value().value().value(), serializer);
        EXPECT_NE(db.prefixEntries()->size(), 0);
        EXPECT_TRUE(*db.deletePrefix());

        evb.stop();
      });

  // let magic happen
  evb.run();
}

/**
 * Test prefix key subscription callback from `KvstoreClientInternal`.
 * The test verifies the callback takes the action that reflects the current
 * state of prefix(existence/disappearance) in the `PrefixManager` and
 * appropriately updates `KvStore`.
 */
TEST_F(PrefixManagerTestFixture, PrefixKeySubscription) {
  int scheduleAt{0};
  int keyVersion{0};
  int staleKeyVersion{100};
  const auto prefixEntry =
      createPrefixEntry(toIpPrefix("5001::/64"), thrift::PrefixType::DEFAULT);
  auto prefixKeyStr =
      PrefixKey(nodeId_, toIPNetwork(*prefixEntry.prefix()), kTestingAreaName)
          .getPrefixKeyV2();

  // Schedule callback to set keys from client1 (this will be executed first)
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry}).get();
      });

  // Wait for throttled update to announce to kvstore
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        ASSERT_TRUE(maybeValue.has_value());
        keyVersion = *maybeValue.value().version();
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value().value(), serializer);
        EXPECT_EQ(*db.thisNodeName(), nodeId_);
        EXPECT_EQ(db.prefixEntries()->size(), 1);
        EXPECT_EQ(db.prefixEntries()[0], prefixEntry);
      });

  // increment the key version in kvstore and set empty value. `PrefixManager`
  // will detect value changed, and retain the value present in persistent DB,
  // and advertise with higher key version.
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 10), [&]() noexcept {
        auto emptyPrefixDb = createPrefixDb(nodeId_, {});
        kvStoreWrapper->setKey(
            kTestingAreaName, /* areaId */
            prefixKeyStr, /* key */
            createThriftValue(
                keyVersion + 1, /* key version */
                nodeId_, /* originatorId */
                writeThriftObjStr(emptyPrefixDb, serializer) /* value */
                ));
      });

  // Wait for throttled update to announce to kvstore
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value().value(), serializer);
        EXPECT_EQ(*maybeValue.value().version(), keyVersion + 2);
        EXPECT_EQ(*db.thisNodeName(), nodeId_);
        EXPECT_EQ(db.prefixEntries()->size(), 1);
        EXPECT_EQ(db.prefixEntries()[0], prefixEntry);
      });

  // Clear key from prefix DB map, which will delete key from persistent
  // store and update kvstore with empty prefix entry list
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept { prefixManager->withdrawPrefixes({prefixEntry}).get(); });

  // verify key is withdrawn from kvstore
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value().value(), serializer);
        EXPECT_EQ(*maybeValue.value().version(), keyVersion + 3);
        EXPECT_EQ(*db.thisNodeName(), nodeId_);
        EXPECT_NE(db.prefixEntries()->size(), 0);
        EXPECT_TRUE(*db.deletePrefix());
      });

  // Insert same key in kvstore with any higher version, and non empty value
  // Prefix manager should get the update and re-advertise with empty Prefix
  // with higher key version.
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixDb = createPrefixDb(nodeId_, {prefixEntry});
        kvStoreWrapper->setKey(
            kTestingAreaName, /* areaId */
            prefixKeyStr, /* key */
            createThriftValue(
                staleKeyVersion, /* key version */
                nodeId_, /* originatorId */
                writeThriftObjStr(prefixDb, serializer) /* value */
                ));
      });

  // prefix manager will override the key inserted above with higher key
  // version and empty prefix DB
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value().value(), serializer);
        EXPECT_EQ(*maybeValue.value().version(), staleKeyVersion + 1);
        EXPECT_EQ(*db.thisNodeName(), nodeId_);
        EXPECT_NE(db.prefixEntries()->size(), 0);
        EXPECT_TRUE(*db.deletePrefix());

        evb.stop();
      });

  // let magic happen
  evb.run();
}

class PrefixManagerSmallTtlTestFixture : public PrefixManagerTestFixture {
 public:
  virtual thrift::OpenrConfig
  createConfig() override {
    auto tConfig = PrefixManagerTestFixture::createConfig();
    tConfig.kvstore_config()->key_ttl_ms() = ttl_.count();
    return tConfig;
  }

 protected:
  std::chrono::milliseconds ttl_{2000};
};

TEST_F(PrefixManagerSmallTtlTestFixture, PrefixWithdrawExpiry) {
  int scheduleAt{0};

  auto prefixKey1 =
      PrefixKey(nodeId_, toIPNetwork(*prefixEntry1.prefix()), kTestingAreaName);
  auto prefixKey2 =
      PrefixKey(nodeId_, toIPNetwork(*prefixEntry2.prefix()), kTestingAreaName);

  // insert two prefixes
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry1}).get();
        prefixManager->advertisePrefixes({prefixEntry2}).get();
      });

  // check both prefixes are in kvstore
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKeyV2();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version(), 1);

        prefixKeyStr = prefixKey2.getPrefixKeyV2();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        EXPECT_EQ(*maybeValue2.value().version(), 1);

        // withdraw prefixEntry1
        prefixManager->withdrawPrefixes({prefixEntry1}).get();
      });

  // check `prefixEntry1` should have been expired, prefix 2 should be there
  // with same version
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt +=
          2 * Constants::kKvStoreSyncThrottleTimeout.count() + ttl_.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKeyV2();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_FALSE(maybeValue.has_value());

        prefixKeyStr = prefixKey2.getPrefixKeyV2();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        EXPECT_EQ(*maybeValue2.value().version(), 1);

        evb.stop();
      });

  // let magic happen
  evb.run();
}

TEST_F(PrefixManagerTestFixture, GetPrefixes) {
  prefixManager->advertisePrefixes({prefixEntry1});
  prefixManager->advertisePrefixes({prefixEntry2});
  prefixManager->advertisePrefixes({prefixEntry3});
  prefixManager->advertisePrefixes({prefixEntry4});
  prefixManager->advertisePrefixes({prefixEntry5});
  prefixManager->advertisePrefixes({prefixEntry6});
  prefixManager->advertisePrefixes({prefixEntry7});
  prefixManager->advertisePrefixes({prefixEntry9});

  auto resp1 = prefixManager->getPrefixes().get();
  ASSERT_TRUE(resp1);
  auto& prefixes1 = *resp1;
  EXPECT_EQ(8, prefixes1.size());
  EXPECT_NE(
      std::find(prefixes1.cbegin(), prefixes1.cend(), prefixEntry4),
      prefixes1.cend());
  EXPECT_EQ(
      std::find(prefixes1.cbegin(), prefixes1.cend(), prefixEntry8),
      prefixes1.cend());

  auto resp2 =
      prefixManager->getPrefixesByType(thrift::PrefixType::DEFAULT).get();
  ASSERT_TRUE(resp2);
  auto& prefixes2 = *resp2;
  EXPECT_EQ(3, prefixes2.size());
  EXPECT_NE(
      std::find(prefixes2.cbegin(), prefixes2.cend(), prefixEntry3),
      prefixes2.cend());
  EXPECT_EQ(
      std::find(prefixes2.cbegin(), prefixes2.cend(), prefixEntry2),
      prefixes2.cend());

  auto resp3 =
      prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT).get();
  EXPECT_TRUE(resp3);

  auto resp4 =
      prefixManager->getPrefixesByType(thrift::PrefixType::DEFAULT).get();
  EXPECT_TRUE(resp4);
  EXPECT_EQ(0, resp4->size());

  auto resp5 = prefixManager->getPrefixesByType(thrift::PrefixType::VIP).get();
  ASSERT_TRUE(resp5);
  auto& prefixes5 = *resp5;
  EXPECT_EQ(1, prefixes5.size());
  EXPECT_NE(
      std::find(prefixes5.cbegin(), prefixes5.cend(), prefixEntry9),
      prefixes5.cend());

  auto resp6 =
      prefixManager->withdrawPrefixesByType(thrift::PrefixType::VIP).get();
  EXPECT_TRUE(resp6);

  auto resp7 = prefixManager->getPrefixesByType(thrift::PrefixType::VIP).get();
  EXPECT_TRUE(resp7);
  EXPECT_EQ(0, resp7->size());
}

TEST_F(PrefixManagerTestFixture, PrefixUpdatesQueue) {
  // ADD_PREFIXES
  {
    // Send update request in queue
    PrefixEvent event(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::BGP,
        {prefixEntry1, prefixEntry7});
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore
    // ATTN: both prefixes should be updated via throttle
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(2, pub.keyVals()->size());

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(2, prefixes->size());
    EXPECT_THAT(
        *prefixes, testing::UnorderedElementsAre(prefixEntry1, prefixEntry7));
  }

  // WITHDRAW_PREFIXES_BY_TYPE
  {
    // Send update request in queue
    PrefixEvent event(
        PrefixEventType::WITHDRAW_PREFIXES_BY_TYPE, thrift::PrefixType::BGP);
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore
    // ATTN: ONLY `prefixEntry7` will be removed as its type is BGP
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals()->size());

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(1, prefixes->size());
    EXPECT_THAT(*prefixes, testing::Contains(prefixEntry1));
  }

  // SYNC_PREFIXES_BY_TYPE
  {
    // Send update request in queue
    PrefixEvent event(
        PrefixEventType::SYNC_PREFIXES_BY_TYPE,
        thrift::PrefixType::DEFAULT,
        {prefixEntry3});
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore
    // ATTN: 1st pub is withdrawn notification of existing `prefixEntry1`
    //       `KvStoreClientInternal` won't throttle the change
    auto pub1 = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub1.keyVals()->size());

    // ATTN: 2nd pub is advertisement notification of `prefixEntry3`
    auto pub2 = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub2.keyVals()->size());

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(1, prefixes->size());
    EXPECT_THAT(*prefixes, testing::Contains(prefixEntry3));
  }

  // WITHDRAW_PREFIXES
  {
    // Send update request in queue
    PrefixEvent event(
        PrefixEventType::WITHDRAW_PREFIXES,
        thrift::PrefixType::DEFAULT,
        {prefixEntry3});
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore (PrefixManager has processed the update)
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals()->size());

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(0, prefixes->size());
  }

  // Test VIP prefixes add and withdraw
  // Add prefixEntry9 with 2 nexthops, withdraw 1 nexthop, then withdraw the
  // other one
  PrefixEntry cPrefixEntry(
      std::make_shared<thrift::PrefixEntry>(prefixEntry9), {});
  std::unordered_set<thrift::NextHopThrift> nexthops;
  nexthops.insert(createNextHop(toBinaryAddress("::1")));
  nexthops.insert(createNextHop(toBinaryAddress("::2")));
  cPrefixEntry.nexthops = nexthops;

  // ADD_PREFIXES
  {
    int scheduleAt{0};
    auto staticRoutesReaderPtr = getEarlyStaticRoutesReaderPtr();

    auto prefixKey9 = PrefixKey(
                          nodeId_,
                          toIPNetwork(*prefixEntry9.prefix()),
                          Constants::kDefaultArea.toString())
                          .getPrefixKeyV2();

    evb.scheduleTimeout(
        std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
          // Send prefix update request in queue
          PrefixEvent event(
              PrefixEventType::ADD_PREFIXES, thrift::PrefixType::VIP, {}, {});
          event.prefixEntries.push_back(cPrefixEntry);
          prefixUpdatesQueue.push(std::move(event));

          // Unicast route of VIP prefixEntry9 is sent to Decision/Fib for
          // programming.
          auto update = waitForRouteUpdate(*staticRoutesReaderPtr);
          EXPECT_TRUE(update.has_value());

          const auto& updatedRoutes = *update.value().unicastRoutesToUpdate();
          const auto& deletedRoutes = *update.value().unicastRoutesToDelete();
          EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
          EXPECT_THAT(deletedRoutes, testing::SizeIs(0));
          const auto& route = updatedRoutes.back();
          EXPECT_EQ(*route.dest(), addr9);
          EXPECT_EQ(route.nextHops().value().size(), 2); // Two next hops.
        });

    evb.scheduleTimeout(
        std::chrono::milliseconds(
            scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
        [&]() {
          // prefixEntry9 is not injected into KvStore.
          EXPECT_FALSE(
              kvStoreWrapper->getKey(kTestingAreaName, prefixKey9).has_value());

          // Unicast route of prefixEntry9 is programmed.
          DecisionRouteUpdate routeUpdate;
          routeUpdate.addRouteToUpdate(RibUnicastEntry(
              toIPNetwork(addr9),
              nexthops,
              prefixEntry9,
              Constants::kDefaultArea.toString()));
          fibRouteUpdatesQueue.push(std::move(routeUpdate));

          // Wait for prefix update in KvStore
          auto pub = kvStoreWrapper->recvPublication();
          EXPECT_EQ(1, pub.keyVals()->size());
          auto maybeValue =
              kvStoreWrapper->getKey(kTestingAreaName, prefixKey9);
          EXPECT_TRUE(maybeValue.has_value());
          auto db = readThriftObjStr<thrift::PrefixDatabase>(
              maybeValue.value().value().value(), serializer);
          EXPECT_EQ(*db.thisNodeName(), nodeId_);
          EXPECT_EQ(db.prefixEntries()->size(), 1);
          // Area stack should not be set for originated VIP.
          EXPECT_TRUE(db.prefixEntries()[0].area_stack()->empty());
          EXPECT_EQ(db.prefixEntries()[0].type(), thrift::PrefixType::VIP);

          // Verify prefixEntry9 exists in PrefixManager.
          auto prefixes = prefixManager->getPrefixes().get();
          EXPECT_EQ(1, prefixes->size());
          EXPECT_THAT(*prefixes, testing::Contains(prefixEntry9));

          evb.stop();
        });

    // let magic happen
    evb.run();
  }

  // WITHDRAW_PREFIXES
  {
    // Send update request in queue
    PrefixEvent event(
        PrefixEventType::WITHDRAW_PREFIXES, thrift::PrefixType::VIP, {}, {});
    event.prefixEntries.push_back(cPrefixEntry);
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore (PrefixManager has processed the update)
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals()->size());

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(0, prefixes->size());
  }
}

/**
 * Verifies `getAdvertisedRoutesFiltered` with all filter combinations
 */
TEST_F(PrefixManagerTestFixture, GetAdvertisedRoutes) {
  //
  // Add prefixes, prefix1 -> DEFAULT, LOOPBACK
  //
  auto const prefix = toIpPrefix("10.0.0.0/8");
  {
    PrefixEvent event1(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::DEFAULT,
        {createPrefixEntry(prefix, thrift::PrefixType::DEFAULT)});
    PrefixEvent event2(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::LOOPBACK,
        {createPrefixEntry(prefix, thrift::PrefixType::LOOPBACK)});
    prefixUpdatesQueue.push(std::move(event1));
    prefixUpdatesQueue.push(std::move(event2));
  }

  //
  // Empty filter
  //
  {
    thrift::AdvertisedRouteFilter filter;
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    auto attempts = 0;
    if (routes->size() == 0 && attempts < 3) {
      routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
      attempts++;
    }

    ASSERT_EQ(1, routes->size());
    auto& routeDetail = routes->at(0);
    EXPECT_EQ(prefix, *routeDetail.prefix());
    EXPECT_EQ(thrift::PrefixType::LOOPBACK, *routeDetail.bestKey());
    EXPECT_EQ(2, routeDetail.bestKeys()->size());
    EXPECT_EQ(2, routeDetail.routes()->size());
  }

  //
  // Filter on prefix
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixes() = std::vector<thrift::IpPrefix>({prefix});
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(1, routes->size());

    auto& routeDetail = routes->at(0);
    EXPECT_EQ(prefix, *routeDetail.prefix());
    EXPECT_EQ(thrift::PrefixType::LOOPBACK, *routeDetail.bestKey());
    EXPECT_EQ(2, routeDetail.bestKeys()->size());
    EXPECT_EQ(2, routeDetail.routes()->size());
  }

  //
  // Filter on non-existing prefix
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixes() =
        std::vector<thrift::IpPrefix>({toIpPrefix("11.0.0.0/8")});
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(0, routes->size());
  }

  //
  // Filter on empty prefix list. Should return empty list
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixes() = std::vector<thrift::IpPrefix>();
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(0, routes->size());
  }

  //
  // Filter on type
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixType() = thrift::PrefixType::DEFAULT;
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(1, routes->size());

    auto& routeDetail = routes->at(0);
    EXPECT_EQ(prefix, *routeDetail.prefix());
    EXPECT_EQ(thrift::PrefixType::LOOPBACK, *routeDetail.bestKey());
    EXPECT_EQ(2, routeDetail.bestKeys()->size());
    EXPECT_EQ(1, routeDetail.routes()->size());

    auto& route = routeDetail.routes()->at(0);
    EXPECT_EQ(thrift::PrefixType::DEFAULT, route.key());
  }

  //
  // Filter on non-existing type (BGP)
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixType() = thrift::PrefixType::BGP;
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(0, routes->size());
  }

  //
  // Filter on non-existing type (VIP)
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixType() = thrift::PrefixType::VIP;
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(0, routes->size());
  }
}

/**
 * Verifies the test case with empty entries. Other cases are exercised above
 */
TEST(PrefixManager, FilterAdvertisedRoutes) {
  std::vector<thrift::AdvertisedRouteDetail> routes;
  std::unordered_map<thrift::PrefixType, PrefixEntry> entries;
  thrift::AdvertisedRouteFilter filter;
  PrefixManager::filterAndAddAdvertisedRoute(
      routes, filter.prefixType(), folly::CIDRNetwork(), entries);
  EXPECT_TRUE(routes.empty());
}

class PrefixManagerMultiAreaTestFixture : public PrefixManagerTestFixture {
 protected:
  thrift::OpenrConfig
  createConfig() override {
    // config three areas A B C without policy
    auto A = createAreaConfig("A", {"RSW.*"}, {".*"});
    auto B = createAreaConfig("B", {"FSW.*"}, {".*"});
    auto C = createAreaConfig("C", {"SSW.*"}, {".*"});

    auto tConfig = getBasicOpenrConfig(nodeId_, {A, B, C});
    return tConfig;
  }

 public:
  // return false if publication is tll update.
  bool
  readPublication(
      thrift::Publication const& pub,
      std::map<
          std::pair<std::string /* prefixStr */, std::string /* area */>,
          thrift::PrefixEntry>& got,
      std::map<
          std::pair<std::string /* prefixStr */, std::string /* area */>,
          thrift::PrefixEntry>& gotDeleted) {
    EXPECT_EQ(1, pub.keyVals()->size());
    auto kv = pub.keyVals()->begin();

    if (!kv->second.value().has_value()) {
      // skip TTL update
      CHECK_GT(*kv->second.ttlVersion(), 0);
      return false;
    }

    auto db = readThriftObjStr<thrift::PrefixDatabase>(
        kv->second.value().value(), serializer);
    EXPECT_EQ(1, db.prefixEntries()->size());
    auto prefix = db.prefixEntries()->at(0);
    auto prefixKeyWithArea = std::make_pair(kv->first, *pub.area());
    if (*db.deletePrefix()) {
      gotDeleted.emplace(prefixKeyWithArea, prefix);
    } else {
      got.emplace(prefixKeyWithArea, prefix);
    }
    return true;
  }
};

/**
 * Test cross-AREA route redistribution for Decision RIB/VIP routes with:
 *  - prefix update
 */
TEST_F(PrefixManagerMultiAreaTestFixture, DecisionRouteUpdates) {
  const auto areaStrA{"A"};
  const auto areaStrB{"B"};
  const auto areaStrC{"C"};
  const auto prefixStr =
      PrefixKey(nodeId_, toIPNetwork(addr1), areaStrA).getPrefixKeyV2();
  const auto prefixKeyAreaA = std::make_pair(prefixStr, areaStrA);
  const auto prefixKeyAreaB = std::make_pair(prefixStr, areaStrB);
  const auto prefixKeyAreaC = std::make_pair(prefixStr, areaStrC);

  auto path1_2_1 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_1"),
      1);
  path1_2_1.area() = areaStrA;
  auto path1_2_2 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_2"),
      2);
  path1_2_2.area() = areaStrB;

  //
  // 1. Inject prefix1 from area A, {B, C} should receive announcement
  //

  {
    auto prefixEntry1A = prefixEntry1;
    auto expectedPrefixEntry1A = prefixEntry1;

    // append area_stack after area redistibution
    prefixEntry1A.area_stack() = {"65000"};
    expectedPrefixEntry1A.area_stack() = {"65000", areaStrA};

    // increase metrics.distance by 1 after area redistribution
    prefixEntry1A.metrics()->distance() = 1;
    expectedPrefixEntry1A.metrics()->distance() = 2;

    // PrefixType being overridden with RIB type after area redistribution
    prefixEntry1A.type() = thrift::PrefixType::DEFAULT;
    expectedPrefixEntry1A.type() = thrift::PrefixType::RIB;

    // Set non-transitive attributes
    prefixEntry1A.forwardingAlgorithm() =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    prefixEntry1A.forwardingType() = thrift::PrefixForwardingType::SR_MPLS;
    prefixEntry1A.minNexthop() = 10;

    // Non-transitive attributes should be reset after redistribution.
    expectedPrefixEntry1A.forwardingAlgorithm() =
        thrift::PrefixForwardingAlgorithm();
    expectedPrefixEntry1A.forwardingType() = thrift::PrefixForwardingType();
    expectedPrefixEntry1A.minNexthop().reset();

    auto unicast1A = RibUnicastEntry(
        toIPNetwork(addr1), {path1_2_1}, prefixEntry1A, areaStrA, false);

    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1A);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::pair<std::string, std::string>, thrift::PrefixEntry> expected,
        got, gotDeleted;
    expected.emplace(prefixKeyAreaB, expectedPrefixEntry1A);
    expected.emplace(prefixKeyAreaC, expectedPrefixEntry1A);

    auto pub1 = kvStoreWrapper->recvPublication();
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreWrapper->recvPublication();
    readPublication(pub2, got, gotDeleted);

    EXPECT_EQ(expected, got);
    EXPECT_EQ(0, gotDeleted.size());
  }

  //
  // 2. Inject prefix1 of VIP type from area B, {A, C} should receive
  // announcement. B withdraw old prefix from A
  //

  {
    // build prefixEntry for addr1 from area "B"
    auto prefixEntry1B = prefixEntry1;
    prefixEntry1B.type() = thrift::PrefixType::VIP;
    auto expectedPrefixEntry1B = prefixEntry1B;

    // append area_stack after area redistibution
    prefixEntry1B.area_stack() = {"65000"};
    expectedPrefixEntry1B.area_stack() = {"65000", areaStrB};

    // increase metrics.distance by 1 after area redistribution
    prefixEntry1B.metrics()->distance() = 1;
    expectedPrefixEntry1B.metrics()->distance() = 2;
    // PrefixType being overridden with RIB type after area redistribution
    expectedPrefixEntry1B.type() = thrift::PrefixType::RIB;

    auto unicast1B = RibUnicastEntry(
        toIPNetwork(addr1), {path1_2_2}, prefixEntry1B, areaStrB, false);

    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1B);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::pair<std::string, std::string>, thrift::PrefixEntry> expected,
        got, gotDeleted;
    expected.emplace(prefixKeyAreaA, expectedPrefixEntry1B);
    expected.emplace(prefixKeyAreaC, expectedPrefixEntry1B);

    auto pub1 = kvStoreWrapper->recvPublication();
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreWrapper->recvPublication();
    readPublication(pub2, got, gotDeleted);

    auto pub3 = kvStoreWrapper->recvPublication();
    readPublication(pub3, got, gotDeleted);

    EXPECT_EQ(expected, got);

    EXPECT_EQ(1, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(prefixKeyAreaB).prefix());
  }

  //
  // 3. Withdraw prefix1, {A, C} receive prefix withdrawal
  //

  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(addr1));
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::pair<std::string, std::string>, thrift::PrefixEntry> expected,
        got, gotDeleted;

    auto pub1 = kvStoreWrapper->recvPublication();
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreWrapper->recvPublication();
    readPublication(pub2, got, gotDeleted);

    EXPECT_EQ(0, got.size());

    EXPECT_EQ(2, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(prefixKeyAreaA).prefix());
    EXPECT_EQ(addr1, *gotDeleted.at(prefixKeyAreaC).prefix());
  }
}

/**
 * Test cross-AREA route redistribution for Decision RIB routes with:
 *  - nexthop updates
 */
TEST_F(PrefixManagerMultiAreaTestFixture, DecisionRouteNexthopUpdates) {
  const auto areaStrA{"A"};
  const auto areaStrB{"B"};
  const auto areaStrC{"C"};
  const auto prefixStr =
      PrefixKey(nodeId_, toIPNetwork(addr1), areaStrA).getPrefixKeyV2();
  const auto prefixKeyAreaA = std::make_pair(prefixStr, areaStrA);
  const auto prefixKeyAreaB = std::make_pair(prefixStr, areaStrB);
  const auto prefixKeyAreaC = std::make_pair(prefixStr, areaStrC);

  auto path1_2_1 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_1"),
      1);
  path1_2_1.area() = areaStrA;
  auto path1_2_2 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_2"),
      2);
  path1_2_2.area() = areaStrB;
  auto path1_2_3 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_3"),
      2);
  path1_2_3.area() = areaStrC;

  //
  // 1. Inject prefix1 with ecmp areas = [A, B], best area = A
  //    => only C receive announcement
  //

  // create unicast route for addr1 from area "A"
  auto prefixEntry1A = prefixEntry1;
  auto expectedPrefixEntry1A = prefixEntry1;

  // append area_stack after area redistibution
  expectedPrefixEntry1A.area_stack() = {areaStrA};

  // increase metrics.distance by 1 after area redistribution
  expectedPrefixEntry1A.metrics()->distance() = 1;

  // PrefixType being overridden with RIB type after area redistribution
  expectedPrefixEntry1A.type() = thrift::PrefixType::RIB;

  auto unicast1A = RibUnicastEntry(
      toIPNetwork(addr1),
      {path1_2_1, path1_2_2},
      prefixEntry1A,
      areaStrA,
      false);

  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1A);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::pair<std::string, std::string>, thrift::PrefixEntry> expected,
        got, gotDeleted;
    expected.emplace(prefixKeyAreaC, expectedPrefixEntry1A);

    auto pub1 = kvStoreWrapper->recvPublication();
    readPublication(pub1, got, gotDeleted);

    EXPECT_EQ(expected, got);
    EXPECT_EQ(0, gotDeleted.size());
  }

  //
  // 2. add C into ecmp group, ecmp areas = [A, B, C], best area = A
  //    => C receive withdraw
  //
  unicast1A.nexthops.emplace(path1_2_3);
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1A);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::pair<std::string, std::string>, thrift::PrefixEntry> got,
        gotDeleted;

    auto pub1 = kvStoreWrapper->recvPublication();
    readPublication(pub1, got, gotDeleted);

    EXPECT_EQ(0, got.size());
    EXPECT_EQ(1, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(prefixKeyAreaC).prefix());
  }

  //
  // 3. withdraw B from ecmp group, ecmp areas = [A, C], best area = A
  //    => B receive update
  //
  unicast1A.nexthops.erase(path1_2_2);
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1A);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::pair<std::string, std::string>, thrift::PrefixEntry> expected,
        got, gotDeleted;
    expected.emplace(prefixKeyAreaB, expectedPrefixEntry1A);

    auto pub1 = kvStoreWrapper->recvPublication();
    readPublication(pub1, got, gotDeleted);

    EXPECT_EQ(expected, got);
    EXPECT_EQ(0, gotDeleted.size());
  }

  //
  // 4. change ecmp group to [B], best area = B
  //    => B receive withdraw, {A, C} receive update
  //

  // create unicast route for addr1 from area "B"
  auto prefixEntry1B = prefixEntry1;
  auto expectedPrefixEntry1B = prefixEntry1;

  // append area_stack after area redistibution
  expectedPrefixEntry1B.area_stack() = {areaStrB};

  // increase metrics.distance by 1 after area redistribution
  expectedPrefixEntry1B.metrics()->distance() = 1;

  // PrefixType being overridden with RIB type after area redistribution
  expectedPrefixEntry1B.type() = thrift::PrefixType::RIB;

  auto unicast1B = RibUnicastEntry(
      toIPNetwork(addr1), {path1_2_2}, prefixEntry1B, areaStrB, false);
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1B);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::pair<std::string, std::string>, thrift::PrefixEntry> expected,
        got, gotDeleted;
    expected.emplace(prefixKeyAreaA, expectedPrefixEntry1B);
    expected.emplace(prefixKeyAreaC, expectedPrefixEntry1B);

    // this test is long, we might hit ttl updates
    // here skip ttl updates
    int expectedPubCnt{3}, gotPubCnt{0};
    while (gotPubCnt < expectedPubCnt) {
      auto pub = kvStoreWrapper->recvPublication();
      gotPubCnt += readPublication(pub, got, gotDeleted);
    }

    EXPECT_EQ(expected, got);

    EXPECT_EQ(1, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(prefixKeyAreaB).prefix());
  }

  //
  // 5. Withdraw prefix1
  //    => {A, C} receive prefix withdrawal
  //
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(addr1));
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::pair<std::string, std::string>, thrift::PrefixEntry> got,
        gotDeleted;

    while (gotDeleted.size() < 2) {
      auto pub = kvStoreWrapper->recvPublication();
      readPublication(pub, got, gotDeleted);
    }

    EXPECT_EQ(0, got.size());
    EXPECT_EQ(2, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(prefixKeyAreaA).prefix());
    EXPECT_EQ(addr1, *gotDeleted.at(prefixKeyAreaC).prefix());
  }
}

class RouteOriginationFixture : public PrefixManagerMultiAreaTestFixture {
 public:
  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4, originatedPrefixV6;
    originatedPrefixV4.prefix() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes() = minSupportingRouteV4_;
    originatedPrefixV4.install_to_fib() = true;
    originatedPrefixV6.prefix() = v6Prefix_;
    originatedPrefixV6.minimum_supporting_routes() = minSupportingRouteV6_;

    auto tConfig = PrefixManagerMultiAreaTestFixture::createConfig();
    tConfig.originated_prefixes() = {originatedPrefixV4, originatedPrefixV6};
    return tConfig;
  }

  std::unordered_map<std::string, thrift::OriginatedPrefixEntry>
  getOriginatedPrefixDb() {
    std::unordered_map<std::string, thrift::OriginatedPrefixEntry> mp;
    while (mp.size() < 2) {
      auto prefixEntries = *prefixManager->getOriginatedPrefixes().get();
      for (auto const& prefixEntry : prefixEntries) {
        if (prefixEntry.prefix()->prefix() == v4Prefix_) {
          mp[v4Prefix_] = prefixEntry;
        }
        if (prefixEntry.prefix()->prefix() == v6Prefix_) {
          mp[v6Prefix_] = prefixEntry;
        }
      }
      std::this_thread::yield();
    }
    return mp;
  }

  // return false if publication is tll update.
  void
  waitForKvStorePublication(
      messaging::RQueue<KvStorePublication>& reader,
      std::unordered_map<
          std::pair<std::string /* prefixStr */, std::string /* areaStr */>,
          thrift::PrefixEntry>& exp,
      std::unordered_set<std::pair<std::string, std::string>>& expDeleted) {
    while (exp.size() || expDeleted.size()) {
      auto maybePub = reader.get().value();
      if (auto* pub = std::get_if<thrift::Publication>(&maybePub)) {
        for (const auto& [key, thriftVal] : *pub->keyVals()) {
          if (!thriftVal.value().has_value()) {
            // skip TTL update
            continue;
          }

          auto db = readThriftObjStr<thrift::PrefixDatabase>(
              thriftVal.value().value(), serializer);
          auto isDeleted = *db.deletePrefix();
          auto prefixEntry = db.prefixEntries()->at(0);
          auto prefixKeyWithArea = std::make_pair(key, *pub->area());
          if (isDeleted && expDeleted.count(prefixKeyWithArea)) {
            VLOG(2) << fmt::format(
                "Withdraw of prefix: {} in area: {} received",
                prefixKeyWithArea.first,
                prefixKeyWithArea.second);
            expDeleted.erase(prefixKeyWithArea);
          }
          if ((!isDeleted) && exp.count(prefixKeyWithArea) &&
              prefixEntry == exp.at(prefixKeyWithArea)) {
            VLOG(2) << fmt::format(
                "Advertising of prefix: {} in area: {} received",
                prefixKeyWithArea.first,
                prefixKeyWithArea.second);
            exp.erase(prefixKeyWithArea);
          }
        }
      }

      // no hogging of CPU cycle
      std::this_thread::yield();
    }
  }

 protected:
  const std::string v4Prefix_ = "192.108.0.1/24";
  const std::string v6Prefix_ = "2001:1:2:3::1/64";

  const uint64_t minSupportingRouteV4_ = 1;
  const uint64_t minSupportingRouteV6_ = 2;

  const thrift::NextHopThrift nh_v4 = createNextHop(
      toBinaryAddress(Constants::kLocalRouteNexthopV4.toString()));
  const thrift::NextHopThrift nh_v6 = createNextHop(
      toBinaryAddress(Constants::kLocalRouteNexthopV6.toString()));

  const folly::CIDRNetwork v4Network_ =
      folly::IPAddress::createNetwork(v4Prefix_);
  const folly::CIDRNetwork v6Network_ =
      folly::IPAddress::createNetwork(v6Prefix_);

  const std::string areaStrA{"A"};
  const std::string areaStrB{"B"};
  const std::string areaStrC{"C"};

  const std::string prefixStrV4_ =
      PrefixKey(nodeId_, v4Network_, areaStrA).getPrefixKeyV2();
  const std::string prefixStrV6_ =
      PrefixKey(nodeId_, v6Network_, areaStrA).getPrefixKeyV2();

  const std::pair<std::string, std::string> prefixKeyV4AreaA_ =
      std::make_pair(prefixStrV4_, areaStrA);
  const std::pair<std::string, std::string> prefixKeyV4AreaB_ =
      std::make_pair(prefixStrV4_, areaStrB);
  const std::pair<std::string, std::string> prefixKeyV4AreaC_ =
      std::make_pair(prefixStrV4_, areaStrC);
  const std::pair<std::string, std::string> prefixKeyV6AreaA_ =
      std::make_pair(prefixStrV6_, areaStrA);
  const std::pair<std::string, std::string> prefixKeyV6AreaB_ =
      std::make_pair(prefixStrV6_, areaStrB);
  const std::pair<std::string, std::string> prefixKeyV6AreaC_ =
      std::make_pair(prefixStrV6_, areaStrC);
};

class RouteOriginationOverrideFixture : public RouteOriginationFixture {
 public:
  void
  SetUp() override {
    // Other set ups
    RouteOriginationFixture::SetUp();

    // Install route for v4Prefix_ since install_to_fib is true.
    DecisionRouteUpdate routeUpdate;
    auto addressV4 = toIpPrefix(v4Prefix_);
    const auto entryV4 =
        createPrefixEntry(addressV4, thrift::PrefixType::CONFIG);
    routeUpdate.addRouteToUpdate(RibUnicastEntry(
        toIPNetwork(addressV4),
        {},
        entryV4,
        Constants::kDefaultArea.toString()));
    fibRouteUpdatesQueue.push(std::move(routeUpdate));
  }

  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4, originatedPrefixV6;
    originatedPrefixV4.prefix() = v4Prefix_;
    originatedPrefixV6.prefix() = v6Prefix_;

    // ATTN: specify supporting route cnt to be 0 for immediate advertisement
    originatedPrefixV4.minimum_supporting_routes() = 0;
    originatedPrefixV6.minimum_supporting_routes() = 0;

    // Make sure we install the prefixes to FIB
    originatedPrefixV4.install_to_fib() = true;
    originatedPrefixV6.install_to_fib() = false; // we can check both cases

    auto tConfig = PrefixManagerMultiAreaTestFixture::createConfig();
    tConfig.originated_prefixes() = {originatedPrefixV4, originatedPrefixV6};
    return tConfig;
  }
};

TEST_F(RouteOriginationOverrideFixture, StaticRoutesAnnounce) {
  auto staticRoutesReaderPtr = getEarlyStaticRoutesReaderPtr();

  auto update =
      waitForRouteUpdate(*staticRoutesReaderPtr, thrift::PrefixType::CONFIG);
  EXPECT_TRUE(update.has_value());

  auto updatedRoutes = *update.value().unicastRoutesToUpdate();
  auto deletedRoutes = *update.value().unicastRoutesToDelete();
  EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
  EXPECT_THAT(deletedRoutes, testing::SizeIs(0));

  // Verify that the destination is the v4 address
  const auto& route = updatedRoutes.back();
  EXPECT_EQ(*route.dest(), toIpPrefix(v4Prefix_));

  // Verify the next hop list is empty
  const auto& nhs = *route.nextHops();
  EXPECT_THAT(nhs, testing::SizeIs(0));

  // Static route for the same config originated prefix is published once.
  update = waitForRouteUpdate(*staticRoutesReaderPtr);
  EXPECT_FALSE(update.has_value());
}

//
// Test case to verify prefix/attributes aligns with config read from
// `thrift::OpenrConfig`. This is the sanity check.
//
// Test also verifies that route with min_supporting_route=0 will be directly
// advertised to `KvStore`.
//
TEST_F(RouteOriginationOverrideFixture, ReadFromConfig) {
  // RQueue interface to read KvStore update
  auto kvStoreUpdatesReader = kvStoreWrapper->getReader();

  // read via public API
  auto mp = getOriginatedPrefixDb();
  auto& prefixEntryV4 = mp.at(v4Prefix_);
  auto& prefixEntryV6 = mp.at(v6Prefix_);

  // verify attributes from originated prefix config
  EXPECT_EQ(0, prefixEntryV4.supporting_prefixes()->size());
  EXPECT_EQ(0, prefixEntryV6.supporting_prefixes()->size());
  EXPECT_TRUE(*prefixEntryV4.installed());
  EXPECT_TRUE(*prefixEntryV6.installed());
  EXPECT_EQ(v4Prefix_, *prefixEntryV4.prefix()->prefix());
  EXPECT_EQ(v6Prefix_, *prefixEntryV6.prefix()->prefix());
  EXPECT_EQ(0, *prefixEntryV4.prefix()->minimum_supporting_routes());
  EXPECT_EQ(0, *prefixEntryV6.prefix()->minimum_supporting_routes());

  // prefixes originated have specific thrift::PrefixType::CONFIG
  const auto bestPrefixEntryV4_ =
      createPrefixEntry(toIpPrefix(v4Prefix_), thrift::PrefixType::CONFIG);
  const auto bestPrefixEntryV6_ =
      createPrefixEntry(toIpPrefix(v6Prefix_), thrift::PrefixType::CONFIG);

  // v4Prefix_ is advertised to ALL areas configured
  std::unordered_map<std::pair<std::string, std::string>, thrift::PrefixEntry>
      exp({
          {prefixKeyV4AreaA_, bestPrefixEntryV4_},
          {prefixKeyV4AreaB_, bestPrefixEntryV4_},
          {prefixKeyV4AreaC_, bestPrefixEntryV4_},
          {prefixKeyV6AreaA_, bestPrefixEntryV6_},
          {prefixKeyV6AreaB_, bestPrefixEntryV6_},
          {prefixKeyV6AreaC_, bestPrefixEntryV6_},
      });
  std::unordered_set<std::pair<std::string, std::string>> expDeleted{};

  // wait for condition to be met for KvStore publication
  waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
}

TEST_F(RouteOriginationFixture, BasicAdvertiseWithdraw) {
  // RQueue interface to read route updates
  auto staticRoutesReader = staticRouteUpdatesQueue.getReader();
  auto kvStoreUpdatesReader = kvStoreWrapper->getReader();

  // dummy nexthop
  auto nh_3 = createNextHop(toBinaryAddress("fe80::1"));
  nh_3.area().reset(); // empty area

  // supporting prefixes
  const std::string v4Prefix_1 = "192.108.0.8/30";
  const std::string v6Prefix_1 = "2001:1:2:3::1/70";
  const std::string v4Prefix_2 = "192.108.1.2/32";
  const std::string v6Prefix_2 = "2001:1:2:3::1/128";
  const auto v4Network_1 = folly::IPAddress::createNetwork(v4Prefix_1);
  const auto v6Network_1 = folly::IPAddress::createNetwork(v6Prefix_1);
  const auto v4Network_2 = folly::IPAddress::createNetwork(v4Prefix_2);
  const auto v6Network_2 = folly::IPAddress::createNetwork(v6Prefix_2);

  // prefixes originated have specific thrift::PrefixType::CONFIG
  const auto bestPrefixEntryV4_ =
      createPrefixEntry(toIpPrefix(v4Prefix_), thrift::PrefixType::CONFIG);
  const auto bestPrefixEntryV6_ =
      createPrefixEntry(toIpPrefix(v6Prefix_), thrift::PrefixType::CONFIG);

  // ATTN: PrefixType is unrelated for supporting routes
  const auto prefixEntryV4_1 =
      createPrefixEntry(toIpPrefix(v4Prefix_1), thrift::PrefixType::DEFAULT);
  const auto prefixEntryV6_1 =
      createPrefixEntry(toIpPrefix(v6Prefix_1), thrift::PrefixType::DEFAULT);
  const auto prefixEntryV4_2 =
      createPrefixEntry(toIpPrefix(v4Prefix_2), thrift::PrefixType::RIB);
  const auto prefixEntryV6_2 =
      createPrefixEntry(toIpPrefix(v6Prefix_2), thrift::PrefixType::RIB);
  auto unicastEntryV4_1 = RibUnicastEntry(
      v4Network_1,
      {nh_v4},
      prefixEntryV4_1,
      Constants::kDefaultArea.toString());
  auto unicastEntryV6_1 = RibUnicastEntry(
      v6Network_1,
      {nh_v6},
      prefixEntryV6_1,
      Constants::kDefaultArea.toString());
  auto unicastEntryV4_2 = RibUnicastEntry(
      v4Network_2,
      {nh_v4, nh_3},
      prefixEntryV4_2,
      Constants::kDefaultArea.toString());
  auto unicastEntryV6_2 = RibUnicastEntry(
      v6Network_2,
      {nh_v6, nh_3},
      prefixEntryV6_2,
      Constants::kDefaultArea.toString());

  auto addressV4 = toIpPrefix(v4Prefix_);
  const auto entryV4 = createPrefixEntry(addressV4, thrift::PrefixType::CONFIG);
  auto unicastEntryV4 = RibUnicastEntry(
      toIPNetwork(addressV4), {}, entryV4, Constants::kDefaultArea.toString());

  //
  // Step1: this tests:
  //  - originated prefix whose supporting routes passed across threshold
  //    will be advertised(v4);
  //  - otherwise it will NOT be advertised;
  //
  // Inject:
  //  - 1 supporting route for v4Prefix;
  //  - 1 supporting route for v6Prefix;
  // Expect:
  //  - v4Prefix_ will be advertised as `min_supporting_route=1`;
  //  - v6Prefix_ will NOT be advertised as `min_supporting_route=2`;
  //
  VLOG(1) << "Starting test step 1...";
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicastEntryV4);
    routeUpdate.addRouteToUpdate(unicastEntryV4_1);
    routeUpdate.addRouteToUpdate(unicastEntryV6_1);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    //  Verify 1): PrefixManager -> KvStore update
    {
      // v4Prefix_ is advertised to ALL areas configured
      std::unordered_map<
          std::pair<std::string, std::string>,
          thrift::PrefixEntry>
          exp({
              {prefixKeyV4AreaA_, bestPrefixEntryV4_},
              {prefixKeyV4AreaB_, bestPrefixEntryV4_},
              {prefixKeyV4AreaC_, bestPrefixEntryV4_},
          });
      std::unordered_set<std::pair<std::string, std::string>> expDeleted{};

      // wait for condition to be met for KvStore publication
      waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);

      auto update = waitForRouteUpdate(staticRoutesReader);
      EXPECT_TRUE(update.has_value());
      auto updatedRoutes = *update.value().unicastRoutesToUpdate();
      auto deletedRoutes = *update.value().unicastRoutesToDelete();
      EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
      EXPECT_THAT(deletedRoutes, testing::SizeIs(0));
      const auto& route = updatedRoutes.back();
      EXPECT_EQ(*route.dest(), toIpPrefix(v4Prefix_));
      const auto& nhs = *route.nextHops();
      EXPECT_THAT(nhs, testing::SizeIs(0));
    }

    // Verify 3): PrefixManager's public API
    {
      auto mp = getOriginatedPrefixDb();
      auto& prefixEntryV4 = mp.at(v4Prefix_);
      auto& prefixEntryV6 = mp.at(v6Prefix_);

      // v4Prefix - advertised, v6Prefix - NOT advertised
      EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                    return *i.installed_ref() == true &&
                        i.supporting_prefixes_ref()->size() == 1;
                  }));
      EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                    return *i.installed_ref() == false &&
                        i.supporting_prefixes_ref()->size() == 1;
                  }));

      EXPECT_THAT(
          *prefixEntryV4.supporting_prefixes(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v4Network_1)));
      EXPECT_THAT(
          *prefixEntryV6.supporting_prefixes(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v6Network_1)));
    }
  }

  //
  // Step2: this tests:
  //  - unrelated prefix will be ignored;
  //  - route deletion followed with addition will make no change
  //    although threshold has been bypassed in the middle;
  //
  // Inject:
  //  - 1 route which is NOT subnet of v4Prefix;
  //  - 1 supporting route for v6Prefix;
  // Withdraw:
  //  - 1 different supporting route for v6Prefix;
  // Expect:
  //  - # of supporting prefix for v4Prefix_ won't change;
  //  - # of supporting prefix for v6Prefix_ won't change;
  //
  VLOG(1) << "Starting test step 2...";
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicastEntryV4_2);
    routeUpdate.addRouteToUpdate(unicastEntryV6_2);
    routeUpdate.unicastRoutesToDelete.emplace_back(v6Network_2);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    // Verify 1): PrefixManager -> Decision update
    {
      // no more route update received
      EXPECT_FALSE(waitForRouteUpdate(staticRoutesReader).has_value());
    }

    // Verify 2): PrefixManager's public API
    {
      // verificaiton via public API
      auto mp = getOriginatedPrefixDb();
      auto& prefixEntryV4 = mp.at(v4Prefix_);
      auto& prefixEntryV6 = mp.at(v6Prefix_);

      // v4Prefix - advertised, v6Prefix - withdrawn
      EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                    return *i.installed_ref() == true &&
                        i.supporting_prefixes_ref()->size() == 1;
                  }));
      EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                    return *i.installed_ref() == false &&
                        i.supporting_prefixes_ref()->size() == 1;
                  }));

      EXPECT_THAT(
          *prefixEntryV4.supporting_prefixes(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v4Network_1)));
      EXPECT_THAT(
          *prefixEntryV6.supporting_prefixes(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v6Network_1)));
    }
  }

  //
  // Step3: this tests:
  //  - existing supporting prefix will be ignored;
  //  - originated prefix whose supporting routes passed across threshold
  //    will be advertised(v6);
  //
  // Inject:
  //  - exactly the same supporting route as previously for v4Prefix;
  //  - 1 supporting route for v6Prefix;
  // Expect:
  //  - v4Prefix_'s supporting routes doesn't change as same update is ignored
  //  - v6Prefix_ will be advertised to `KvStore` as `min_supporting_route=2`
  //  - v6Prefix_ will NOT be advertised to `Decision` as `install_to_fib=false`
  //
  VLOG(1) << "Starting test step 3...";
  {
    DecisionRouteUpdate routeUpdate;
    // ATTN: change ribEntry attributes to make sure no impact on ref-count
    auto tmpEntryV4 = unicastEntryV4_1;
    tmpEntryV4.nexthops = {createNextHop(toBinaryAddress("192.168.0.1"))};
    routeUpdate.addRouteToUpdate(tmpEntryV4);
    routeUpdate.addRouteToUpdate(unicastEntryV6_2);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    // Verify 1): PrefixManager -> Decision update
    {
      // no more route update received
      EXPECT_FALSE(waitForRouteUpdate(staticRoutesReader).has_value());
    }

    // Verify 2): PrefixManager -> KvStore update
    {
      // v6Prefix_ is advertised to ALL areas configured
      std::unordered_map<
          std::pair<std::string, std::string>,
          thrift::PrefixEntry>
          exp({
              {prefixKeyV6AreaA_, bestPrefixEntryV6_},
              {prefixKeyV6AreaB_, bestPrefixEntryV6_},
              {prefixKeyV6AreaC_, bestPrefixEntryV6_},
          });
      std::unordered_set<std::pair<std::string, std::string>> expDeleted{};

      // wait for condition to be met for KvStore publication
      waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
    }

    // Verify 3): PrefixManager's public API
    {
      // verificaiton via public API
      auto mp = getOriginatedPrefixDb();
      auto& prefixEntryV4 = mp.at(v4Prefix_);
      auto& prefixEntryV6 = mp.at(v6Prefix_);

      // v4Prefix - advertised, v6Prefix - advertised
      EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                    return *i.installed_ref() == true &&
                        i.supporting_prefixes_ref()->size() == 1;
                  }));
      EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                    return *i.installed_ref() == true &&
                        i.supporting_prefixes_ref()->size() == 2;
                  }));

      EXPECT_THAT(
          *prefixEntryV4.supporting_prefixes(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v4Network_1)));
      EXPECT_THAT(
          *prefixEntryV6.supporting_prefixes(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v6Network_1),
              folly::IPAddress::networkToString(v6Network_2)));
    }
  }

  // Step4: Withdraw:
  //  - 1 supporting route of v4Prefix;
  //  - 1 supporting route of v6Prefix;
  // Expect:
  //  - v4Prefix_ is withdrawn as `supporting_route_cnt=0`;
  //  - v6Prefix_ is withdrawn as `supporting_route_cnt=1`;
  //  - `Decision` won't receive routeUpdate for `v6Prefix_`
  //    since it has `install_to_fib=false`;
  //
  VLOG(1) << "Starting test step 4...";
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(v4Network_1);
    routeUpdate.unicastRoutesToDelete.emplace_back(v6Network_1);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    // Verify 1): PrefixManager -> Decision update
    {
      // ONLY v4 route withdrawn updates received
      auto update = waitForRouteUpdate(staticRoutesReader);
      EXPECT_TRUE(update.has_value());

      auto updatedRoutes = *update.value().unicastRoutesToUpdate();
      auto deletedRoutes = *update.value().unicastRoutesToDelete();
      EXPECT_THAT(updatedRoutes, testing::SizeIs(0));
      EXPECT_THAT(deletedRoutes, testing::SizeIs(1));
      EXPECT_THAT(
          deletedRoutes, testing::UnorderedElementsAre(toIpPrefix(v4Prefix_)));
    }

    //  Verify 2): PrefixManager -> KvStore update
    {
      // both v4Prefix_ + v6Prefix_ are withdrawn from ALL areas configured
      std::unordered_set<std::pair<std::string, std::string>> expDeleted{
          prefixKeyV4AreaA_,
          prefixKeyV4AreaB_,
          prefixKeyV4AreaC_,
          prefixKeyV6AreaA_,
          prefixKeyV6AreaB_,
          prefixKeyV6AreaC_};
      std::unordered_map<
          std::pair<std::string, std::string>,
          thrift::PrefixEntry>
          exp{};

      // wait for condition to be met for KvStore publication
      waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
    }

    // Verify 3): PrefixManager's public API
    {
      auto mp = getOriginatedPrefixDb();
      auto& prefixEntryV4 = mp.at(v4Prefix_);
      auto& prefixEntryV6 = mp.at(v6Prefix_);

      // v4Prefix - withdrawn, v6Prefix - withdrawn
      EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                    return *i.installed_ref() == false &&
                        i.supporting_prefixes_ref()->size() == 0;
                  }));
      EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                    return *i.installed_ref() == false &&
                        i.supporting_prefixes_ref()->size() == 1;
                  }));

      // verify attributes
      EXPECT_THAT(
          *prefixEntryV6.supporting_prefixes(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v6Network_2)));
    }
  }
}

class RouteOriginationV4OverV6ZeroSupportFixture
    : public RouteOriginationFixture {
 public:
  void
  SetUp() override {
    // We register the read in the beginning of the test
    earlyStaticRoutesReaderPtr =
        std::make_shared<messaging::RQueue<DecisionRouteUpdate>>(
            staticRouteUpdatesQueue.getReader());

    // Other set ups
    RouteOriginationFixture::SetUp();
  }

  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4;
    originatedPrefixV4.prefix() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes() = 0;
    originatedPrefixV4.install_to_fib() = true;

    auto tConfig = PrefixManagerMultiAreaTestFixture::createConfig();
    tConfig.originated_prefixes() = {originatedPrefixV4};
    // Enable v4 over v6 nexthop feature
    tConfig.v4_over_v6_nexthop() = true;
    tConfig_ = tConfig;
    return tConfig;
  }

  const openr::thrift::OpenrConfig
  getConfig() const {
    return tConfig_;
  }

  std::shared_ptr<messaging::RQueue<DecisionRouteUpdate>>
  getEarlyStaticRoutesReaderPtr() {
    return earlyStaticRoutesReaderPtr;
  }

 private:
  thrift::OpenrConfig tConfig_;
  std::shared_ptr<messaging::RQueue<DecisionRouteUpdate>>
      earlyStaticRoutesReaderPtr;
};

TEST_F(RouteOriginationV4OverV6ZeroSupportFixture, ConfigVerification) {
  const auto tConfig = getConfig();
  EXPECT_TRUE(tConfig.v4_over_v6_nexthop());
}

TEST_F(RouteOriginationV4OverV6ZeroSupportFixture, StateRouteAnnounce) {
  auto staticRoutesReaderPtr = getEarlyStaticRoutesReaderPtr();

  auto update = waitForRouteUpdate(*staticRoutesReaderPtr);
  EXPECT_TRUE(update.has_value());

  auto updatedRoutes = *update.value().unicastRoutesToUpdate();
  auto deletedRoutes = *update.value().unicastRoutesToDelete();
  EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
  EXPECT_THAT(deletedRoutes, testing::SizeIs(0));

  // verify thrift::NextHopThrift struct
  const auto& route = updatedRoutes.back();
  EXPECT_EQ(*route.dest(), toIpPrefix(v4Prefix_));

  const auto& nhs = *route.nextHops();
  EXPECT_THAT(nhs, testing::SizeIs(0));
}

class RouteOriginationV4OverV6NonZeroSupportFixture
    : public RouteOriginationFixture {
 public:
  void
  SetUp() override {
    // create KvStore + PrefixManager instances for testing
    initKvStoreWithPrefixManager();

    // trigger initialization sequence before writing to KvStore
    triggerInitializationEventForPrefixManager(
        fibRouteUpdatesQueue, kvStoreWrapper->getKvStoreUpdatesQueueWriter());

    // ATTN: do NOT wait for PREFIX_DB_SYNC event publication since
    // the test will validate queue events happening before initializaiton
    // event being published.
  }

  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4;
    originatedPrefixV4.prefix() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes() = 2; // 2 support pfxs
    originatedPrefixV4.install_to_fib() = true;

    auto tConfig = PrefixManagerMultiAreaTestFixture::createConfig();
    tConfig.originated_prefixes() = {originatedPrefixV4};
    // Enable v4 over v6 nexthop feature
    tConfig.v4_over_v6_nexthop() = true;
    tConfig_ = tConfig;
    return tConfig;
  }

  const openr::thrift::OpenrConfig
  getConfig() const {
    return tConfig_;
  }

 private:
  thrift::OpenrConfig tConfig_;
};

TEST_F(
    RouteOriginationV4OverV6NonZeroSupportFixture,
    StaticRoutesAnnounceNeedsSupport) {
  auto staticRoutesReader = staticRouteUpdatesQueue.getReader();
  auto v4PrefixKey = PrefixKey(
      nodeId_, folly::IPAddress::createNetwork(v4Prefix_), kTestingAreaName);
  auto v4PrefixKeyStr = v4PrefixKey.getPrefixKeyV2();
  {
    // Static route for prefix with `install_to_fib=true` should be published in
    // OpenR initialization process.
    // v4Prefix_ has install_to_fib set as true.
    auto update = waitForRouteUpdate(staticRoutesReader);
    EXPECT_TRUE(update.has_value());
    auto updatedRoutes = *update.value().unicastRoutesToUpdate();
    auto deletedRoutes = *update.value().unicastRoutesToDelete();
    EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
    EXPECT_THAT(deletedRoutes, testing::SizeIs(0));
    // verify thrift::NextHopThrift struct
    const auto& route = updatedRoutes.back();
    EXPECT_EQ(*route.dest(), toIpPrefix(v4Prefix_));
    const auto& nhs = *route.nextHops();
    EXPECT_THAT(nhs, testing::SizeIs(0));
  }

  {
    // First Fib route updates include v4 and one supporting prefix.
    // Note the v4Prefix is 192.108.0.1/24 :-)
    auto addressV4 = toIpPrefix(v4Prefix_);
    const auto entryV4 =
        createPrefixEntry(addressV4, thrift::PrefixType::CONFIG);
    const auto unicastEntryV4 = RibUnicastEntry(
        toIPNetwork(addressV4),
        {},
        entryV4,
        Constants::kDefaultArea.toString());
    const std::string v4Prefix_1 = "192.108.0.11/30";
    const auto v4Network_1 = folly::IPAddress::createNetwork(v4Prefix_1);
    const auto prefixEntryV4_1 =
        createPrefixEntry(toIpPrefix(v4Prefix_1), thrift::PrefixType::DEFAULT);
    auto unicastEntryV4_1 = RibUnicastEntry(
        v4Network_1,
        {nh_v6}, // doesn't matter but we are enabling v6 nexthop only :-)
        prefixEntryV4_1,
        Constants::kDefaultArea.toString());
    DecisionRouteUpdate routeUpdate1;
    routeUpdate1.addRouteToUpdate(unicastEntryV4);
    routeUpdate1.addRouteToUpdate(unicastEntryV4_1);
    fibRouteUpdatesQueue.push(std::move(routeUpdate1));

    // After first Fib route updates, minimum_supporting_routes=2 is not
    // fulfilled for config originated v4Prefix_. As a result, previously
    // published unicast route should be removed.
    auto update = waitForRouteUpdate(staticRoutesReader);
    EXPECT_TRUE(update.has_value());
    auto deletedRoutes = *update.value().unicastRoutesToDelete();
    EXPECT_THAT(deletedRoutes, testing::SizeIs(1));
    EXPECT_EQ(deletedRoutes.back(), toIpPrefix(v4Prefix_));

    // v4Prefix_ is not advertised yet.
    auto maybeValue = kvStoreWrapper->getKey(kTestingAreaName, v4PrefixKeyStr);
    EXPECT_FALSE(maybeValue.has_value());
  }

  {
    // Supporting prefix number 2
    // Note the v4Prefix is 192.108.0.1/24 :-)
    const std::string v4Prefix_2 = "192.108.0.22/30";
    const auto v4Network_2 = folly::IPAddress::createNetwork(v4Prefix_2);
    const auto prefixEntryV4_2 =
        createPrefixEntry(toIpPrefix(v4Prefix_2), thrift::PrefixType::DEFAULT);
    auto unicastEntryV4_2 = RibUnicastEntry(
        v4Network_2,
        {nh_v6}, // doesn't matter but we are enabling v6 nexhop only :-)
        prefixEntryV4_2,
        Constants::kDefaultArea.toString());
    DecisionRouteUpdate routeUpdate2;
    routeUpdate2.addRouteToUpdate(unicastEntryV4_2);
    fibRouteUpdatesQueue.push(std::move(routeUpdate2));

    // Two supporting routes are received. v4Prefix_ should be advertised.
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals()->count(v4PrefixKeyStr));

    // minimum_supporting_routes=2 is fulfilled, same static route for v4Prefix_
    // should not be published again.
    auto update = waitForRouteUpdate(staticRoutesReader);
    EXPECT_TRUE(update.has_value());
    auto updatedRoutes = *update.value().unicastRoutesToUpdate();
    auto deletedRoutes = *update.value().unicastRoutesToDelete();
    EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
    EXPECT_THAT(deletedRoutes, testing::SizeIs(0));
    // verify thrift::NextHopThrift struct
    const auto& route2 = updatedRoutes.back();
    EXPECT_EQ(*route2.dest(), toIpPrefix(v4Prefix_));
    EXPECT_THAT(*route2.nextHops(), testing::SizeIs(0));
  }
}

TEST(PrefixManagerPendingUpdates, updatePrefixes) {
  detail::PrefixManagerPendingUpdates updates;

  // verify initial state
  EXPECT_TRUE(updates.getChangedPrefixes().empty());

  // non-empty change
  auto network1 = toIPNetwork(addr1);
  auto network2 = toIPNetwork(addr2);
  updates.addPrefixChange(network1);
  updates.addPrefixChange(network2);
  EXPECT_THAT(
      updates.getChangedPrefixes(),
      testing::UnorderedElementsAre(network1, network2));

  // cleanup
  updates.clear();
  EXPECT_TRUE(updates.getChangedPrefixes().empty());
}

/*
 * Verify that the PrefixMgr API getAreaAdvertisedRoutes() returns the
 * correct preferred prefixes.
 */
TEST_F(PrefixManagerTestFixture, VerifyCLIWithMultipleClients) {
  const auto defaultPrefixLower = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(100, 0, 0));
  const auto openrPrefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::CONFIG, createMetrics(200, 0, 0));
  const auto defaultPrefixHigher = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 0, 0));

  thrift::AdvertisedRouteFilter emptyFilter;
  {
    // With only the CONFIG prefix, this will be advertised
    prefixManager->advertisePrefixes({openrPrefix});
    const auto routes = prefixManager
                            ->getAreaAdvertisedRoutes(
                                kTestingAreaName,
                                thrift::RouteFilterType::POSTFILTER_ADVERTISED,
                                emptyFilter)
                            .get();

    EXPECT_EQ(1, routes->size());
    const auto& route = routes->at(0);
    EXPECT_EQ(thrift::PrefixType::CONFIG, *route.key());
  }
  {
    // Even though DEFAULT prefix is higher preference, its metrics are lower
    prefixManager->advertisePrefixes({defaultPrefixLower});
    const auto routes = prefixManager
                            ->getAreaAdvertisedRoutes(
                                kTestingAreaName,
                                thrift::RouteFilterType::POSTFILTER_ADVERTISED,
                                emptyFilter)
                            .get();

    EXPECT_EQ(1, routes->size());
    const auto& route = routes->at(0);
    EXPECT_EQ(thrift::PrefixType::CONFIG, *route.key());
  }
  {
    // with higher/same metrics, DEFFAULT prefix will be preferred over CONFIG
    prefixManager->advertisePrefixes({defaultPrefixHigher});
    const auto routes = prefixManager
                            ->getAreaAdvertisedRoutes(
                                kTestingAreaName,
                                thrift::RouteFilterType::POSTFILTER_ADVERTISED,
                                emptyFilter)
                            .get();

    EXPECT_EQ(1, routes->size());
    const auto& route = routes->at(0);
    EXPECT_EQ(thrift::PrefixType::DEFAULT, *route.key());
  }
}

class RouteOriginationSingleAreaFixture : public RouteOriginationFixture {
 public:
  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4, originatedPrefixV6;
    originatedPrefixV4.prefix() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes() = minSupportingRouteV4_;
    originatedPrefixV4.install_to_fib() = true;
    originatedPrefixV6.prefix() = v6Prefix_;
    originatedPrefixV6.minimum_supporting_routes() = minSupportingRouteV6_;
    originatedPrefixV6.install_to_fib() = false;

    // create a single area config
    auto A = createAreaConfig("A", {"RSW.*"}, {".*"});
    auto tConfig = getBasicOpenrConfig(nodeId_, {A});
    tConfig.originated_prefixes() = {originatedPrefixV4, originatedPrefixV6};
    return tConfig;
  }
};

TEST_F(RouteOriginationSingleAreaFixture, BasicAdvertiseWithdraw) {
  // RQueue interface to read route updates sent by PrefixManager to
  // Decision. This queue is expressly used for originated routes
  auto staticRoutesReader = staticRouteUpdatesQueue.getReader();
  auto kvStoreUpdatesReader = kvStoreWrapper->getReader();

  // dummy nexthop
  auto nh_3 = createNextHop(toBinaryAddress("fe80::1"));
  nh_3.area().reset(); // empty area

  auto addressV4 = toIpPrefix(v4Prefix_);
  const auto entryV4 = createPrefixEntry(addressV4, thrift::PrefixType::CONFIG);
  const auto unicastEntryV4 = RibUnicastEntry(
      toIPNetwork(addressV4), {}, entryV4, Constants::kDefaultArea.toString());

  // supporting V4 prefix and related structs
  const std::string v4Prefix_1 = "192.108.0.8/30";
  const auto v4Network_1 = folly::IPAddress::createNetwork(v4Prefix_1);
  const auto prefixEntryV4_1 =
      createPrefixEntry(toIpPrefix(v4Prefix_1), thrift::PrefixType::DEFAULT);
  auto unicastEntryV4_1 = RibUnicastEntry(
      v4Network_1,
      {nh_v4},
      prefixEntryV4_1,
      Constants::kDefaultArea.toString());

  // supporting V6 prefix #1 and related RIB structs
  const std::string v6Prefix_1 = "2001:1:2:3::1/70";
  const auto v6Network_1 = folly::IPAddress::createNetwork(v6Prefix_1);
  const auto prefixEntryV6_1 =
      createPrefixEntry(toIpPrefix(v6Prefix_1), thrift::PrefixType::DEFAULT);
  auto unicastEntryV6_1 = RibUnicastEntry(
      v6Network_1,
      {nh_v6},
      prefixEntryV6_1,
      Constants::kDefaultArea.toString());

  // supporting V6 prefix #2 and related RIB structs
  const std::string v6Prefix_2 = "2001:1:2:3::1/120";
  const auto v6Network_2 = folly::IPAddress::createNetwork(v6Prefix_2);
  const auto prefixEntryV6_2 =
      createPrefixEntry(toIpPrefix(v6Prefix_2), thrift::PrefixType::RIB);
  auto unicastEntryV6_2 = RibUnicastEntry(
      v6Network_2,
      {nh_v6, nh_3},
      prefixEntryV6_2,
      Constants::kDefaultArea.toString());

  // Originated prefixes have specific thrift::PrefixType::CONFIG
  const auto bestPrefixEntryV4_ =
      createPrefixEntry(toIpPrefix(v4Prefix_), thrift::PrefixType::CONFIG);
  const auto bestPrefixEntryV6_ =
      createPrefixEntry(toIpPrefix(v6Prefix_), thrift::PrefixType::CONFIG);

  //
  // This test case tests the following:
  //  - originated prefix whose supporting routes passed across threshold
  //    will be advertised (v4, and eventually v6);
  //  - otherwise it will NOT be advertised (initially v6);
  //  - Route Advertisement to KvStore happens with single area configured
  //
  // Steps, briefly:
  //
  // 1. Inject the following into the fibRouteUpdatesQueue (simulating Fib to
  //    Prefixmgr interaction):
  //    - 1st supporting route for v4Prefix_;
  //    - 1st supporting route for v6Prefix_;
  // Verification:
  //    c. v4Prefix_ will be advertised to KvStore as `min_supporting_route=1`;
  //    d. v6Prefix_ will NOT be advertised as `min_supporting_route=2`;
  //    e. Config values and supporting routes count is as expected for both
  //       v4Prefix_ and v6Prefix_
  //
  // 2. Inject the following into the fibRouteUpdatesQueue (simulating Fib
  //    to Prefixmgr interaction):
  //    - 2nd supporting route for v6Prefix_;
  // Verification:
  //    a. v6Prefix_ will STILL NOT be sent to Decision on
  //       staticRouteUpdatesQueue since, while min_supporting_route is now met
  //       for v6Prefix_, the install_to_fib bit is NOT set for v6Prefix_
  //    b. v6Prefix_ will be advertised to KvStore as `min_supporting_route=2`;
  //    c. Config values and supporting routes count is as expected for both
  //       v4Prefix_ and v6Prefix_
  //
  // 3. Withdraw 1 supporting route from both v4Prefix_ and v6Prefix_
  //    - this will break the min_supporting_routes condition for both
  //      the prefixes.
  // Verification:
  //    a. delete only for the v4Prefix_ gets sent to Decision
  //    b. Both prefixes will be withdrawn from KvStore
  //    c. Supporting routes count for both prefixes will decrement by 1

  // Step 1 - inject 1 v4 and 1 v6 supporting prefix into fibRouteUpdatesQueue
  DecisionRouteUpdate routeUpdate;
  routeUpdate.addRouteToUpdate(unicastEntryV4);
  routeUpdate.addRouteToUpdate(unicastEntryV4_1);
  routeUpdate.addRouteToUpdate(unicastEntryV6_1);
  fibRouteUpdatesQueue.push(routeUpdate);

  // Verify 1c and 1d: PrefixManager -> Decision staticRouteupdate
  {
    // 1c. v4 route update received
    auto update = waitForRouteUpdate(staticRoutesReader);
    EXPECT_TRUE(update.has_value());
    auto updatedRoutes = *update.value().unicastRoutesToUpdate();
    auto deletedRoutes = *update.value().unicastRoutesToDelete();
    EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
    EXPECT_THAT(deletedRoutes, testing::SizeIs(0));
    const auto& route = updatedRoutes.back();
    EXPECT_EQ(*route.dest(), toIpPrefix(v4Prefix_));
    const auto& nhs = *route.nextHops();
    EXPECT_THAT(nhs, testing::SizeIs(0));

    // 1d. no v6 route update received
    EXPECT_FALSE(waitForRouteUpdate(staticRoutesReader).has_value());
  }

  //  Verify 1e and 1f: PrefixManager -> KvStore update
  {
    // v4Prefix_ is advertised to ALL areas configured, while v6Prefix_ is NOT
    std::unordered_map<std::pair<std::string, std::string>, thrift::PrefixEntry>
        exp({
            {prefixKeyV4AreaA_, bestPrefixEntryV4_},
        });
    std::unordered_set<std::pair<std::string, std::string>> expDeleted{};

    // wait for condition to be met for KvStore publication
    waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
  }

  // Verify 1g: Via PrefixManager's public API, verify the values for # of
  //  supporting routes for both the v4Prefix_ (1) and v6Orefix_(1)
  {
    auto mp = getOriginatedPrefixDb();
    auto& prefixEntryV4 = mp.at(v4Prefix_);
    auto& prefixEntryV6 = mp.at(v6Prefix_);

    // v4Prefix - advertised, v6Prefix - NOT advertised
    EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                  return *i.installed_ref() == true &&
                      i.supporting_prefixes_ref()->size() == 1;
                }));
    EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                  return *i.installed_ref() == false &&
                      i.supporting_prefixes_ref()->size() == 1;
                }));

    EXPECT_THAT(
        *prefixEntryV4.supporting_prefixes(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v4Network_1)));
    EXPECT_THAT(
        *prefixEntryV6.supporting_prefixes(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v6Network_1)));
  }

  // Step 2 - inject 1 v6 supporting prefix into fibRouteUpdatesQueue
  routeUpdate.addRouteToUpdate(unicastEntryV6_2);
  fibRouteUpdatesQueue.push(routeUpdate);

  // Verify 2a: PrefixManager -> Decision staticRouteUpdate
  {
    // 2a. NO v6 route update received
    auto update = waitForRouteUpdate(staticRoutesReader);
    EXPECT_FALSE(update.has_value());
  }

  //  Verify 2b: PrefixManager -> KvStore update
  {
    // v6Prefix_ is advertised to the SINGLE area configured
    std::unordered_map<std::pair<std::string, std::string>, thrift::PrefixEntry>
        exp({
            {prefixKeyV6AreaA_, bestPrefixEntryV6_},
        });
    std::unordered_set<std::pair<std::string, std::string>> expDeleted{};

    // 2b. wait for condition to be met for KvStore publication
    waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
  }

  // Verify 2c: Via PrefixManager's public API, verify the values for # of
  //  supporting routes for both the v4Prefix_ (1) and v6Orefix_(2 now)
  {
    auto mp = getOriginatedPrefixDb();
    auto& prefixEntryV4 = mp.at(v4Prefix_);
    auto& prefixEntryV6 = mp.at(v6Prefix_);

    // v4Prefix - advertised, v6Prefix - advertised
    EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                  return *i.installed_ref() == true &&
                      i.supporting_prefixes_ref()->size() == 1;
                }));
    EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                  return *i.installed_ref() == true &&
                      i.supporting_prefixes_ref()->size() == 2;
                }));

    EXPECT_THAT(
        *prefixEntryV4.supporting_prefixes(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v4Network_1)));
    EXPECT_THAT(
        *prefixEntryV6.supporting_prefixes(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v6Network_1),
            folly::IPAddress::networkToString(v6Network_2)));
  }

  // Step 3 - withdraw 1 v4 and 1 v6 supporting prefix
  routeUpdate.unicastRoutesToDelete.emplace_back(v4Network_1);
  routeUpdate.unicastRoutesToDelete.emplace_back(v6Network_1);
  fibRouteUpdatesQueue.push(routeUpdate);

  // Verify 3a PrefixManager -> Decision staticRouteupdate
  {
    // ONLY v4 route withdrawn updates are sent to Decision
    auto update = waitForRouteUpdate(staticRoutesReader);
    EXPECT_TRUE(update.has_value());

    auto updatedRoutes = *update.value().unicastRoutesToUpdate();
    auto deletedRoutes = *update.value().unicastRoutesToDelete();
    EXPECT_THAT(updatedRoutes, testing::SizeIs(0));
    EXPECT_THAT(deletedRoutes, testing::SizeIs(1));
    EXPECT_THAT(
        deletedRoutes, testing::UnorderedElementsAre(toIpPrefix(v4Prefix_)));
  }

  //  Verify 3b: PrefixManager -> KvStore update: both prefixes withdrawn
  {
    // both v4Prefix_ + v6Prefix_ are withdrawn from the single area configured
    std::unordered_set<std::pair<std::string, std::string>> expDeleted{
        prefixKeyV4AreaA_,
        prefixKeyV6AreaA_,
    };
    std::unordered_map<std::pair<std::string, std::string>, thrift::PrefixEntry>
        exp{};

    waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
  }

  // Verify 3c: Via PrefixManager's public API, verify that that supporting
  // routes count for v6Prefix_ is now 1 (and 0 for v4Prefix_)
  {
    auto mp = getOriginatedPrefixDb();
    auto& prefixEntryV4 = mp.at(v4Prefix_);
    auto& prefixEntryV6 = mp.at(v6Prefix_);

    // v4Prefix - withdrawn, v6Prefix - withdrawn
    EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                  return *i.installed_ref() == false &&
                      i.supporting_prefixes_ref()->size() == 0;
                }));
    EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                  return *i.installed_ref() == false &&
                      i.supporting_prefixes_ref()->size() == 1;
                }));

    EXPECT_THAT(
        *prefixEntryV6.supporting_prefixes(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v6Network_2)));
  }
}

TEST_F(PrefixManagerTestFixture, BasicKeyValueRequestQueue) {
  const auto prefixKey = "prefixKeyStr";
  const auto prefixVal = "prefixDbStr";
  const auto prefixDeletedVal = "prefixDeletedStr";
  int scheduleAt{0};
  // Persist key.
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        auto persistPrefixKeyVal =
            PersistKeyValueRequest(kTestingAreaName, prefixKey, prefixVal);
        kvRequestQueue.push(std::move(persistPrefixKeyVal));
      });

  // Check that key was correctly persisted. Wait for throttling in KvStore.
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreWrapper->getKey(kTestingAreaName, prefixKey);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version(), 1);
      });

  // Send an unset key request.
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto unsetPrefixRequest = ClearKeyValueRequest(
            kTestingAreaName, prefixKey, prefixDeletedVal, true);
        kvRequestQueue.push(std::move(unsetPrefixRequest));
      });

  // Check that key was unset properly. Key is still in KvStore because TTL has
  // not expired yet. TTL refreshing has stopped so TTL version remains at 0.
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreWrapper->getKey(kTestingAreaName, prefixKey);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version(), 2);
        EXPECT_EQ(*maybeValue.value().ttlVersion(), 0);
        evb.stop();
      });

  evb.run();
}

TEST_F(PrefixManagerTestFixture, AdvertisePrefixes) {
  int scheduleAt{0};
  auto prefixKey1 = PrefixKey(
      nodeId_,
      folly::IPAddress::createNetwork(toString(*prefixEntry1.prefix())),
      kTestingAreaName);
  auto prefixKey2 = PrefixKey(
      nodeId_,
      folly::IPAddress::createNetwork(toString(*prefixEntry2.prefix())),
      kTestingAreaName);

  // 1. Advertise prefix entry.
  // 2. Check that prefix entry is in KvStore.
  // 3. Advertise two prefix entries: previously advertised one and new one.
  // 4. Check that both prefixes are in KvStore. Neither's version are bumped.
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry1}).get();
      });

  // Wait for throttling. Throttling can come from:
  //  - `syncKvStore()` inside `PrefixManager`
  //  - `persistSelfOriginatedKey()` inside `KvStore`
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        // Check that prefix entry is in KvStore.
        auto prefixKeyStr = prefixKey1.getPrefixKeyV2();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version(), 1);

        // Advertise one previously advertised prefix and one new prefix.
        prefixManager->advertisePrefixes({prefixEntry1, prefixEntry2}).get();
      });

  // Check that both prefixes are in KvStore. Wait for throttling.
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        // First prefix was re-advertised with same value. Version should not
        // have been bumped.
        auto prefixKeyStr = prefixKey1.getPrefixKeyV2();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version(), 1);

        // Verify second prefix was advertised.
        prefixKeyStr = prefixKey2.getPrefixKeyV2();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        EXPECT_EQ(*maybeValue2.value().version(), 1);
        evb.stop();
      });

  evb.run();
}

TEST_F(PrefixManagerTestFixture, WithdrawPrefix) {
  int scheduleAt{0};
  auto prefixKeyStr =
      PrefixKey(
          nodeId_,
          folly::IPAddress::createNetwork(toString(*prefixEntry1.prefix())),
          kTestingAreaName)
          .getPrefixKeyV2();

  // 1. Advertise prefix entry.
  // 2. Check that prefix entry is in KvStore.
  // 3. Withdraw prefix entry.
  // 4. Check that prefix is withdrawn.

  // Advertise prefix entry.
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        prefixManager->advertisePrefixes({prefixEntry1}).get();
      });

  // Wait for throttling. Throttling can come from:
  //  - `syncKvStore()` inside `PrefixManager`
  //  - `persistSelfOriginatedKey()` inside `KvStore`
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        // Check that prefix is in KvStore.
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());

        // Withdraw prefix.
        prefixManager->withdrawPrefixes({prefixEntry1}).get();
      });

  // Wait for throttling. Verify key is withdrawn.
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        // Key is still in KvStore because TTL has not expired yet. TTL
        // refreshing has stopped so TTL version remains at 0.
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().ttlVersion(), 0);
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value().value(), serializer);
        EXPECT_NE(db.prefixEntries()->size(), 0);
        EXPECT_TRUE(*db.deletePrefix());

        evb.stop();
      });

  evb.run();
}

class PrefixManagerInitialKvStoreSyncTestFixture
    : public PrefixManagerTestFixture {
 protected:
  void
  SetUp() override {
    // create KvStore + PrefixManager instances for testing
    initKvStoreWithPrefixManager();

    // ATTN: do NOT trigger initialization event in SetUp() since the test
    // itself will validate that part of logic
  }
};

class PrefixManagerInitialKvStoreSyncVipEnabledTestFixture
    : public PrefixManagerInitialKvStoreSyncTestFixture {
 protected:
  thrift::OpenrConfig
  createConfig() override {
    auto tConfig = PrefixManagerInitialKvStoreSyncTestFixture::createConfig();
    // Enable Vip service.
    tConfig.enable_vip_service() = true;
    tConfig.vip_service_config() = vipconfig::config::VipServiceConfig();
    return tConfig;
  }
};

/**
 * Verifies that in OpenR initialization procedure, initial syncKvStore() is
 * triggered after all dependent signals are received.
 */
TEST_F(
    PrefixManagerInitialKvStoreSyncVipEnabledTestFixture,
    TriggerInitialKvStoreTest) {
  auto staticRoutesReader = staticRouteUpdatesQueue.getReader();

  int scheduleAt{0};
  auto prefixDbMarker = Constants::kPrefixDbMarker.toString() + nodeId_;

  PrefixEntry vipPrefixEntry(
      std::make_shared<thrift::PrefixEntry>(prefixEntry9), {});
  std::unordered_set<thrift::NextHopThrift> nexthops;
  nexthops.insert(createNextHop(toBinaryAddress("::1")));
  nexthops.insert(createNextHop(toBinaryAddress("::2")));
  vipPrefixEntry.nexthops = nexthops;

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // Initial prefix updates from VipRouteManager
        PrefixEvent vipPrefixEvent(
            PrefixEventType::ADD_PREFIXES, thrift::PrefixType::VIP);
        vipPrefixEvent.prefixEntries.emplace_back(vipPrefixEntry);
        prefixUpdatesQueue.push(std::move(vipPrefixEvent));

        // Expect initial static routes to be published for VIPs.
        auto update =
            waitForRouteUpdate(staticRoutesReader, thrift::PrefixType::VIP);
        EXPECT_EQ(update.value().unicastRoutesToUpdate()->size(), 1);
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // No prefixes advertised into KvStore.
        EXPECT_EQ(0, getNumPrefixes(prefixDbMarker));

        // Initial full Fib sync.
        DecisionRouteUpdate fullSyncUpdates;
        fullSyncUpdates.type = DecisionRouteUpdate::FULL_SYNC;
        fullSyncUpdates.addRouteToUpdate(RibUnicastEntry(
            toIPNetwork(addr9),
            nexthops,
            prefixEntry9,
            Constants::kDefaultArea.toString()));
        fibRouteUpdatesQueue.push(std::move(fullSyncUpdates));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial KvStore sync happens after initial full Fib updates and all
        // prefix updates are received.
        auto prefixKey9 = PrefixKey(
            nodeId_, toIPNetwork(*prefixEntry9.prefix()), kTestingAreaName);

        auto pub = kvStoreWrapper->recvPublication();
        // VIP and config originated prefix are both advertised.
        EXPECT_EQ(1, pub.keyVals()->size());
        EXPECT_EQ(1, pub.keyVals()->count(prefixKey9.getPrefixKeyV2()));

        evb.stop();
      });
  // let magic happen
  evb.run();
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  const folly::Init init(&argc, &argv);

  // Run the tests
  return RUN_ALL_TESTS();
}
