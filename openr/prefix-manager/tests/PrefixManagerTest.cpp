/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/prefix-manager/PrefixManager.h>

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

} // namespace

class PrefixManagerTestFixture : public testing::Test {
 public:
  void
  SetUp() override {
    // create config
    config = std::make_shared<Config>(createConfig());

    // spin up a kvstore
    kvStoreWrapper = std::make_shared<KvStoreWrapper>(context, config);
    kvStoreWrapper->run();
    LOG(INFO) << "The test KV store is running";

    // start a prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        staticRouteUpdatesQueue,
        prefixUpdatesQueue.getReader(),
        routeUpdatesQueue.getReader(),
        config,
        kvStoreWrapper->getKvStore(),
        std::chrono::seconds{0});

    prefixManagerThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "PrefixManager thread starting";
      prefixManager->run();
      LOG(INFO) << "PrefixManager thread finishing";
    });
    prefixManager->waitUntilRunning();
  }

  void
  TearDown() override {
    // Close queues
    prefixUpdatesQueue.close();
    routeUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
    kvStoreWrapper->closeQueue();

    // cleanup kvStoreClient
    if (kvStoreClient) {
      kvStoreClient.reset();
    }

    // this will be invoked before linkMonitorThread's d-tor
    LOG(INFO) << "Stopping prefixMgr thread";
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

  virtual thrift::OpenrConfig
  createConfig() {
    auto tConfig = getBasicOpenrConfig(nodeId_);
    tConfig.kvstore_config_ref()->sync_interval_s_ref() = 1;
    return tConfig;
  }

  // Get number of `advertised` prefixes
  uint32_t
  getNumPrefixes(const std::string& keyPrefix) {
    uint32_t count = 0;
    auto keyVals = kvStoreWrapper->dumpAll(
        kTestingAreaName, KvStoreFilters({keyPrefix}, {} /* originatorIds */));
    for (const auto& [_, val] : keyVals) {
      if (not val.value_ref()) {
        continue;
      }

      auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
          *val.value_ref(), serializer);
      if (*prefixDb.deletePrefix_ref()) {
        // skip prefixes marked for delete
        continue;
      }
      count += 1;
    }
    return count;
  }

  // NodeId to initialize
  const std::string nodeId_{"node-1"};

  fbzmq::Context context;
  OpenrEventBase evb;
  std::thread evbThread;

  // Queue for publishing entries to PrefixManager
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;

  // Create the serializer for write/read
  CompactSerializer serializer;
  std::shared_ptr<Config> config{nullptr};
  std::unique_ptr<PrefixManager> prefixManager{nullptr};
  std::unique_ptr<std::thread> prefixManagerThread{nullptr};
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper{nullptr};
  std::unique_ptr<KvStoreClientInternal> kvStoreClient{nullptr};
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
  auto prefixKey = PrefixKey(
      nodeId_, toIPNetwork(*prefixEntry1.prefix_ref()), kTestingAreaName);
  auto keyStr = prefixKey.getPrefixKey();
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
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_EQ(db.prefixEntries_ref()->size(), 1);

        prefixManager->withdrawPrefixes({prefixEntry1}).get();
        prefixManager->advertisePrefixes({prefixEntry2}).get();
        prefixManager->advertisePrefixes({prefixEntry3}).get();
        prefixManager->advertisePrefixes({prefixEntry4}).get();
        prefixManager->advertisePrefixes({prefixEntry5}).get();
        prefixManager->advertisePrefixes({prefixEntry6}).get();
        prefixManager->advertisePrefixes({prefixEntry7}).get();
        prefixManager->advertisePrefixes({prefixEntry8}).get();
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += Constants::kKvStoreSyncThrottleTimeout.count() / 2),
      [&]() {
        // Verify that before throttle expires, we don't see any update
        auto maybeValue1 = kvStoreWrapper->getKey(kTestingAreaName, keyStr);
        EXPECT_TRUE(maybeValue1.has_value());
        auto db1 = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue1.value().value_ref().value(), serializer);
        EXPECT_EQ(1, getNumPrefixes(prefixDbMarker));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Wait for throttled update to announce to kvstore
        auto maybeValue2 = kvStoreWrapper->getKey(kTestingAreaName, keyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        auto db2 = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue2.value().value_ref().value(), serializer);
        EXPECT_EQ(7, getNumPrefixes(prefixDbMarker));
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
            maybeValue3.value().value_ref().value(), serializer);
        EXPECT_EQ(5, getNumPrefixes(prefixDbMarker));

        evb.stop();
      });

  // let magic happen
  evb.run();
}

/**
 * Test prefix advertisement in KvStore with multiple clients.
 * NOTE: Priority LOOPBACK > DEFAULT > BGP
 * 1. Inject prefix1 with client-bgp - Verify KvStore
 * 2. Inject prefix1 with client-loopback and client-default - Verify KvStore
 * 3. Withdraw prefix1 with client-loopback - Verify KvStore
 * 4. Withdraw prefix1 with client-bgp - Verify KvStore
 * 5. Withdraw prefix1 with client-default - Verify KvStore
 */
TEST_F(PrefixManagerTestFixture, VerifyKvStoreMultipleClients) {
  auto createMetrics = [](int32_t pp, int32_t sp, int32_t d) {
    thrift::PrefixMetrics metrics;
    metrics.path_preference_ref() = pp;
    metrics.source_preference_ref() = sp;
    metrics.distance_ref() = d;
    return metrics;
  };
  auto createPrefixEntryWithMetrics = [](thrift::IpPrefix const& prefix,
                                         thrift::PrefixType const& type,
                                         thrift::PrefixMetrics const& metrics) {
    thrift::PrefixEntry entry;
    entry.prefix_ref() = prefix;
    entry.type_ref() = type;
    entry.metrics_ref() = metrics;
    return entry;
  };

  //
  // Order of prefix-entries -> loopback > bgp > default
  //
  const auto loopback_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::LOOPBACK, createMetrics(200, 0, 0));
  const auto default_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(100, 0, 0));
  const auto bgp_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::BGP, createMetrics(200, 0, 0));

  auto keyStr =
      PrefixKey(nodeId_, toIPNetwork(addr1), kTestingAreaName).getPrefixKey();

  // Synchronization primitive
  folly::Baton baton;
  std::optional<thrift::PrefixEntry> expectedPrefix;
  bool gotExpected = true;

  // start kvStoreClientInternal separately with different thread
  kvStoreClient = std::make_unique<KvStoreClientInternal>(
      &evb, nodeId_, kvStoreWrapper->getKvStore());

  kvStoreClient->subscribeKey(
      kTestingAreaName,
      keyStr,
      [&](std::string const&, std::optional<thrift::Value> val) mutable {
        ASSERT_TRUE(val.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            val->value_ref().value(), serializer);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        if (expectedPrefix.has_value() and
            db.prefixEntries_ref()->size() != 0) {
          // we should always be advertising one prefix until we withdraw all
          EXPECT_EQ(db.prefixEntries_ref()->size(), 1);
          EXPECT_EQ(expectedPrefix, db.prefixEntries_ref()->at(0));
          gotExpected = true;
        } else {
          EXPECT_TRUE(*db.deletePrefix_ref());
          EXPECT_TRUE(db.prefixEntries_ref()->size() == 1);
        }

        // Signal verification
        if (gotExpected) {
          baton.post();
        }
      });

  // Start event loop in it's own thread
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  //
  // 1. Inject prefix1 with client-bgp - Verify KvStore
  //
  expectedPrefix = bgp_prefix;
  gotExpected = false;
  prefixManager->advertisePrefixes({bgp_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 2. Inject prefix1 with client-loopback and client-default - Verify KvStore
  //
  expectedPrefix = loopback_prefix; // lowest client-id will win
  gotExpected = false;
  prefixManager->advertisePrefixes({loopback_prefix, default_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 3. Withdraw prefix1 with client-loopback - Verify KvStore
  //
  expectedPrefix = bgp_prefix;
  gotExpected = false;
  prefixManager->withdrawPrefixes({loopback_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 4. Withdraw prefix1 with client-bgp - Verify KvStore
  //
  expectedPrefix = default_prefix;
  gotExpected = true;
  prefixManager->withdrawPrefixes({bgp_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 4. Withdraw prefix1 with client-default - Verify KvStore
  //
  expectedPrefix = std::nullopt;
  gotExpected = true;
  prefixManager->withdrawPrefixes({default_prefix}).get();
  baton.wait();
  baton.reset();
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
      folly::IPAddress::createNetwork(toString(*prefixEntry1.prefix_ref())),
      kTestingAreaName);
  auto prefixKey2 = PrefixKey(
      nodeId_,
      folly::IPAddress::createNetwork(toString(*prefixEntry2.prefix_ref())),
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
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version_ref(), 1);

        // add another key
        prefixManager->advertisePrefixes({prefixEntry2}).get();
      });

  // version of first key should still be 1
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 4 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version_ref(), 1);

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        EXPECT_EQ(*maybeValue2.value().version_ref(), 1);

        // withdraw prefixEntry2
        prefixManager->withdrawPrefixes({prefixEntry2}).get();
      });

  // version of prefixEntry1 should still be 1
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version_ref(), 1);

        // verify key is withdrawn
        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue2.value().value_ref().value(), serializer);
        EXPECT_NE(db.prefixEntries_ref()->size(), 0);
        EXPECT_TRUE(*db.deletePrefix_ref());

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
  auto prefixKey = PrefixKey(
      nodeId_, toIPNetwork(*prefixEntry.prefix_ref()), kTestingAreaName);
  auto prefixKeyStr = prefixKey.getPrefixKey();

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
        EXPECT_TRUE(maybeValue.has_value());
        keyVersion = *maybeValue.value().version_ref();
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_EQ(db.prefixEntries_ref()->size(), 1);
        EXPECT_EQ(db.prefixEntries_ref()[0], prefixEntry);
      });

  // increment the key version in kvstore and set empty value. `PrefixManager`
  // will detect value changed, and retain the value present in peristent DB,
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
                writeThriftObjStr(emptyPrefixDb, serializer), /* value */
                Constants::kKvStoreDbTtl.count()));
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
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*maybeValue.value().version_ref(), keyVersion + 2);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_EQ(db.prefixEntries_ref()->size(), 1);
        EXPECT_EQ(db.prefixEntries_ref()[0], prefixEntry);
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
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*maybeValue.value().version_ref(), keyVersion + 3);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_NE(db.prefixEntries_ref()->size(), 0);
        EXPECT_TRUE(*db.deletePrefix_ref());
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
                writeThriftObjStr(prefixDb, serializer), /* value */
                Constants::kKvStoreDbTtl.count()));
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
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*maybeValue.value().version_ref(), staleKeyVersion + 1);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_NE(db.prefixEntries_ref()->size(), 0);
        EXPECT_TRUE(*db.deletePrefix_ref());

        evb.stop();
      });

  // let magic happen
  evb.run();
}

TEST_F(PrefixManagerTestFixture, PrefixWithdrawExpiry) {
  int scheduleAt{0};
  std::chrono::milliseconds ttl{100};
  const std::string nodeId{"node-2"};

  // spin up a new PrefixManager add verify that it loads the config
  auto tConfig = getBasicOpenrConfig(nodeId);
  tConfig.kvstore_config_ref()->key_ttl_ms_ref() = ttl.count();
  auto config = std::make_shared<Config>(tConfig);
  auto prefixManager2 = std::make_unique<PrefixManager>(
      staticRouteUpdatesQueue,
      prefixUpdatesQueue.getReader(),
      routeUpdatesQueue.getReader(),
      config,
      kvStoreWrapper->getKvStore(),
      std::chrono::seconds(0));

  auto prefixManagerThread2 =
      std::make_unique<std::thread>([&]() { prefixManager2->run(); });
  prefixManager2->waitUntilRunning();

  auto prefixKey1 = PrefixKey(
      nodeId, toIPNetwork(*prefixEntry1.prefix_ref()), kTestingAreaName);
  auto prefixKey2 = PrefixKey(
      nodeId, toIPNetwork(*prefixEntry2.prefix_ref()), kTestingAreaName);

  // insert two prefixes
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        prefixManager2->advertisePrefixes({prefixEntry1}).get();
        prefixManager2->advertisePrefixes({prefixEntry2}).get();
      });

  // check both prefixes are in kvstore
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        EXPECT_EQ(*maybeValue.value().version_ref(), 1);

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        EXPECT_EQ(*maybeValue2.value().version_ref(), 1);

        // withdraw prefixEntry1
        prefixManager2->withdrawPrefixes({prefixEntry1}).get();
      });

  // check `prefixEntry1` should have been expired, prefix 2 should be there
  // with same version
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt +=
          2 * Constants::kKvStoreSyncThrottleTimeout.count() + ttl.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_FALSE(maybeValue.has_value());

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue2.has_value());
        EXPECT_EQ(*maybeValue2.value().version_ref(), 1);

        evb.stop();
      });

  // let magic happen
  evb.run();

  // cleanup
  prefixUpdatesQueue.close();
  routeUpdatesQueue.close();
  kvStoreWrapper->closeQueue();
  prefixManager2->stop();
  prefixManagerThread2->join();
}

TEST_F(PrefixManagerTestFixture, GetPrefixes) {
  prefixManager->advertisePrefixes({prefixEntry1});
  prefixManager->advertisePrefixes({prefixEntry2});
  prefixManager->advertisePrefixes({prefixEntry3});
  prefixManager->advertisePrefixes({prefixEntry4});
  prefixManager->advertisePrefixes({prefixEntry5});
  prefixManager->advertisePrefixes({prefixEntry6});
  prefixManager->advertisePrefixes({prefixEntry7});

  auto resp1 = prefixManager->getPrefixes().get();
  ASSERT_TRUE(resp1);
  auto& prefixes1 = *resp1;
  EXPECT_EQ(7, prefixes1.size());
  EXPECT_NE(
      std::find(prefixes1.begin(), prefixes1.end(), prefixEntry4),
      prefixes1.end());
  EXPECT_EQ(
      std::find(prefixes1.begin(), prefixes1.end(), prefixEntry8),
      prefixes1.end());

  auto resp2 =
      prefixManager->getPrefixesByType(thrift::PrefixType::DEFAULT).get();
  ASSERT_TRUE(resp2);
  auto& prefixes2 = *resp2;
  EXPECT_EQ(3, prefixes2.size());
  EXPECT_NE(
      std::find(prefixes2.begin(), prefixes2.end(), prefixEntry3),
      prefixes2.end());
  EXPECT_EQ(
      std::find(prefixes2.begin(), prefixes2.end(), prefixEntry2),
      prefixes2.end());

  auto resp3 =
      prefixManager->withdrawPrefixesByType(thrift::PrefixType::DEFAULT).get();
  EXPECT_TRUE(resp3);

  auto resp4 =
      prefixManager->getPrefixesByType(thrift::PrefixType::DEFAULT).get();
  EXPECT_TRUE(resp4);
  EXPECT_EQ(0, resp4->size());
}

TEST_F(PrefixManagerTestFixture, PrefixUpdatesQueue) {
  // ADD_PREFIXES
  {
    // Send update request in queue
    PrefixEvent event(
        PrefixEventType::ADD_PREFIXES,
        std::nullopt,
        {prefixEntry1, prefixEntry7});
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore
    // ATTN: both prefixes should be updated via throttle
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(2, pub.keyVals_ref()->size());

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
    EXPECT_EQ(1, pub.keyVals_ref()->size());

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
    EXPECT_EQ(1, pub1.keyVals_ref()->size());

    // ATTN: 2nd pub is advertisement notification of `prefixEntry3`
    auto pub2 = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub2.keyVals_ref()->size());

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(1, prefixes->size());
    EXPECT_THAT(*prefixes, testing::Contains(prefixEntry3));
  }

  // WITHDRAW_PREFIXES
  {
    // Send update request in queue
    PrefixEvent event(
        PrefixEventType::WITHDRAW_PREFIXES, std::nullopt, {prefixEntry3});
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore (PrefixManager has processed the update)
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());

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
    PrefixEvent event(
        PrefixEventType::ADD_PREFIXES,
        std::nullopt,
        {createPrefixEntry(prefix, thrift::PrefixType::DEFAULT),
         createPrefixEntry(prefix, thrift::PrefixType::LOOPBACK)});
    prefixUpdatesQueue.push(std::move(event));
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
    EXPECT_EQ(prefix, *routeDetail.prefix_ref());
    EXPECT_EQ(thrift::PrefixType::LOOPBACK, *routeDetail.bestKey_ref());
    EXPECT_EQ(2, routeDetail.bestKeys_ref()->size());
    EXPECT_EQ(2, routeDetail.routes_ref()->size());
  }

  //
  // Filter on prefix
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixes_ref() = std::vector<thrift::IpPrefix>({prefix});
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(1, routes->size());

    auto& routeDetail = routes->at(0);
    EXPECT_EQ(prefix, *routeDetail.prefix_ref());
    EXPECT_EQ(thrift::PrefixType::LOOPBACK, *routeDetail.bestKey_ref());
    EXPECT_EQ(2, routeDetail.bestKeys_ref()->size());
    EXPECT_EQ(2, routeDetail.routes_ref()->size());
  }

  //
  // Filter on non-existing prefix
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixes_ref() =
        std::vector<thrift::IpPrefix>({toIpPrefix("11.0.0.0/8")});
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(0, routes->size());
  }

  //
  // Filter on empty prefix list. Should return empty list
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixes_ref() = std::vector<thrift::IpPrefix>();
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(0, routes->size());
  }

  //
  // Filter on type
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixType_ref() = thrift::PrefixType::DEFAULT;
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(1, routes->size());

    auto& routeDetail = routes->at(0);
    EXPECT_EQ(prefix, *routeDetail.prefix_ref());
    EXPECT_EQ(thrift::PrefixType::LOOPBACK, *routeDetail.bestKey_ref());
    EXPECT_EQ(2, routeDetail.bestKeys_ref()->size());
    EXPECT_EQ(1, routeDetail.routes_ref()->size());

    auto& route = routeDetail.routes_ref()->at(0);
    EXPECT_EQ(thrift::PrefixType::DEFAULT, route.key_ref());
  }

  //
  // Filter on non-existing type (BGP)
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixType_ref() = thrift::PrefixType::BGP;
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(0, routes->size());
  }
}

/**
 * Verifies the test case with empty entries. Other cases are exercised above
 */
TEST(PrefixManager, FilterAdvertisedRoutes) {
  std::vector<thrift::AdvertisedRouteDetail> routes;
  std::unordered_map<thrift::PrefixType, PrefixManager::PrefixEntry> entries;
  thrift::AdvertisedRouteFilter filter;
  PrefixManager::filterAndAddAdvertisedRoute(
      routes, filter.prefixType_ref(), folly::CIDRNetwork(), entries);
  EXPECT_TRUE(routes.empty());
}

class PrefixManagerMultiAreaTestFixture : public PrefixManagerTestFixture {
  thrift::OpenrConfig
  createConfig() override {
    // config three areas A B C without policy
    auto A = createAreaConfig("A", {"RSW.*"}, {".*"});
    auto B = createAreaConfig("B", {"FSW.*"}, {".*"});
    auto C = createAreaConfig("C", {"SSW.*"}, {".*"});

    auto tConfig = getBasicOpenrConfig(nodeId_, "domain", {A, B, C});
    tConfig.kvstore_config_ref()->sync_interval_s_ref() = 1;

    return tConfig;
  }

 public:
  // return false if publication is tll update.
  bool
  readPublication(
      thrift::Publication const& pub,
      std::map<std::string, thrift::PrefixEntry>& got,
      std::map<std::string, thrift::PrefixEntry>& gotDeleted) {
    EXPECT_EQ(1, pub.keyVals_ref()->size());
    auto kv = pub.keyVals_ref()->begin();

    if (not kv->second.value_ref().has_value()) {
      // skip TTL update
      CHECK_GT(*kv->second.ttlVersion_ref(), 0);
      return false;
    }

    auto db = readThriftObjStr<thrift::PrefixDatabase>(
        kv->second.value_ref().value(), serializer);
    EXPECT_EQ(1, db.prefixEntries_ref()->size());
    auto prefix = db.prefixEntries_ref()->at(0);
    if (*db.deletePrefix_ref()) {
      gotDeleted.emplace(kv->first, prefix);
    } else {
      got.emplace(kv->first, prefix);
    }
    return true;
  }
};

/**
 * Test prefix advertisement in KvStore with multiple clients.
 * NOTE: Priority LOOPBACK > DEFAULT > BGP
 * 1. Inject prefix1 with client-bgp - Verify KvStore
 * 2. Inject prefix1 with client-loopback and client-default - Verify KvStore
 * 3. Withdraw prefix1 with client-loopback - Verify KvStore
 * 4. Withdraw prefix1 with client-bgp, client-default - Verify KvStore
 */
TEST_F(PrefixManagerMultiAreaTestFixture, DecisionRouteUpdates) {
  auto kvStoreUpdatesQueue = kvStoreWrapper->getReader();

  auto path1_2_1 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_1"),
      1);
  path1_2_1.area_ref() = "A";
  auto path1_2_2 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_2"),
      2);
  path1_2_2.area_ref() = "B";

  // prefix key for addr1 in area A, B, C
  auto keyStrA = PrefixKey(nodeId_, toIPNetwork(addr1), "A").getPrefixKey();
  auto keyStrB = PrefixKey(nodeId_, toIPNetwork(addr1), "B").getPrefixKey();
  auto keyStrC = PrefixKey(nodeId_, toIPNetwork(addr1), "C").getPrefixKey();

  //
  // 1. Inject prefix1 from area A, B and C should receive announcement
  //

  // create unicast route for addr1 from area "A"
  auto prefixEntry1A = prefixEntry1;
  prefixEntry1A.area_stack_ref() = {"65000"};
  prefixEntry1A.metrics_ref()->distance_ref() = 1;
  prefixEntry1A.metrics_ref()->source_preference_ref() = 90;
  auto unicast1A = RibUnicastEntry(
      toIPNetwork(addr1), {path1_2_1}, prefixEntry1A, "A", false);
  // expected kvstore announcement to other area, append "A" in area stack
  auto expectedPrefixEntry1A = prefixEntry1A;
  expectedPrefixEntry1A.area_stack_ref()->push_back("A");
  ++*expectedPrefixEntry1A.metrics_ref()->distance_ref();
  expectedPrefixEntry1A.type_ref() = thrift::PrefixType::RIB;

  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1A);
    routeUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrB, expectedPrefixEntry1A);
    expected.emplace(keyStrC, expectedPrefixEntry1A);

    auto pub1 = kvStoreUpdatesQueue.get().value();
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreUpdatesQueue.get().value();
    readPublication(pub2, got, gotDeleted);

    EXPECT_EQ(expected, got);
    EXPECT_EQ(0, gotDeleted.size());
  }

  //
  // 2. Inject prefix1 from area B
  //    A, C receive announcement, B withdraw old prefix from A
  //

  // create unicast route for addr1 from area "B"
  auto prefixEntry1B = prefixEntry1;
  prefixEntry1B.area_stack_ref() = {"65000"}; // previous area stack
  prefixEntry1B.metrics_ref()->distance_ref() = 1;
  prefixEntry1B.metrics_ref()->source_preference_ref() = 100;
  auto unicast1B = RibUnicastEntry(
      toIPNetwork(addr1), {path1_2_2}, prefixEntry1B, "B", false);
  // expected kvstore announcement to other area, append "B" in area stack
  auto expectedPrefixEntry1B = prefixEntry1B;
  expectedPrefixEntry1B.area_stack_ref()->push_back("B");
  ++*expectedPrefixEntry1B.metrics_ref()->distance_ref();
  expectedPrefixEntry1B.type_ref() = thrift::PrefixType::RIB;

  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1B);
    routeUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrA, expectedPrefixEntry1B);
    expected.emplace(keyStrC, expectedPrefixEntry1B);

    auto pub1 = kvStoreUpdatesQueue.get().value();
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreUpdatesQueue.get().value();
    readPublication(pub2, got, gotDeleted);

    auto pub3 = kvStoreUpdatesQueue.get().value();
    readPublication(pub3, got, gotDeleted);

    EXPECT_EQ(expected, got);

    EXPECT_EQ(1, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrB).prefix_ref());
  }

  //
  // 3. Withdraw prefix1, A, C receive prefix withdrawal
  //
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(addr1));
    routeUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> got, gotDeleted;

    auto pub1 = kvStoreUpdatesQueue.get().value();
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreUpdatesQueue.get().value();
    readPublication(pub2, got, gotDeleted);

    EXPECT_EQ(0, got.size());

    EXPECT_EQ(2, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrA).prefix_ref());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrC).prefix_ref());
  }
}

TEST_F(PrefixManagerMultiAreaTestFixture, DecisionRouteNexthopUpdates) {
  auto kvStoreUpdatesQueue = kvStoreWrapper->getReader();

  auto path1_2_1 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_1"),
      1);
  path1_2_1.area_ref() = "A";
  auto path1_2_2 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_2"),
      2);
  path1_2_2.area_ref() = "B";
  auto path1_2_3 = createNextHop(
      toBinaryAddress(folly::IPAddress("fe80::2")),
      std::string("iface_1_2_3"),
      2);
  path1_2_3.area_ref() = "C";

  // prefix key for addr1 in area A, B, C
  auto keyStrA = PrefixKey(nodeId_, toIPNetwork(addr1), "A").getPrefixKey();
  auto keyStrB = PrefixKey(nodeId_, toIPNetwork(addr1), "B").getPrefixKey();
  auto keyStrC = PrefixKey(nodeId_, toIPNetwork(addr1), "C").getPrefixKey();

  //
  // 1. Inject prefix1 with ecmp areas = [A, B], best area = A
  //    => only C receive announcement
  //

  // create unicast route for addr1 from area "A"
  auto prefixEntry1A = prefixEntry1;
  prefixEntry1A.metrics_ref()->source_preference_ref() = 90;
  auto unicast1A = RibUnicastEntry(
      toIPNetwork(addr1), {path1_2_1, path1_2_2}, prefixEntry1A, "A", false);

  // expected kvstore announcement to other area, append "A" in area stack
  auto expectedPrefixEntry1A = prefixEntry1A;
  expectedPrefixEntry1A.area_stack_ref()->push_back("A");
  ++*expectedPrefixEntry1A.metrics_ref()->distance_ref();
  expectedPrefixEntry1A.type_ref() = thrift::PrefixType::RIB;

  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1A);
    routeUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrC, expectedPrefixEntry1A);

    auto pub1 = kvStoreUpdatesQueue.get().value();
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
    routeUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> got, gotDeleted;

    auto pub1 = kvStoreUpdatesQueue.get().value();
    readPublication(pub1, got, gotDeleted);

    EXPECT_EQ(0, got.size());
    EXPECT_EQ(1, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrC).prefix_ref());
  }

  //
  // 3. withdraw B from ecmp group, ecmp areas = [A, C], best area = A
  //    => B receive update
  //
  unicast1A.nexthops.erase(path1_2_2);
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1A);
    routeUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrB, expectedPrefixEntry1A);

    auto pub1 = kvStoreUpdatesQueue.get().value();
    readPublication(pub1, got, gotDeleted);

    EXPECT_EQ(expected, got);
    EXPECT_EQ(0, gotDeleted.size());
  }

  //
  // 4. change ecmp group to [B], best area = B
  //    => B receive withdraw, A, C receive update
  //

  // create unicast route for addr1 from area "B"
  auto prefixEntry1B = prefixEntry1;
  prefixEntry1B.metrics_ref()->source_preference_ref() = 100;
  auto unicast1B = RibUnicastEntry(
      toIPNetwork(addr1), {path1_2_2}, prefixEntry1B, "B", false);

  // expected kvstore announcement to other area, append "B" in area stack
  auto expectedPrefixEntry1B = prefixEntry1B;
  expectedPrefixEntry1B.area_stack_ref()->push_back("B");
  ++*expectedPrefixEntry1B.metrics_ref()->distance_ref();
  expectedPrefixEntry1B.type_ref() = thrift::PrefixType::RIB;

  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1B);
    routeUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrA, expectedPrefixEntry1B);
    expected.emplace(keyStrC, expectedPrefixEntry1B);

    // this test is long, we might hit ttl updates
    // here skip ttl updates
    int expectedPubCnt{3}, gotPubCnt{0};
    while (gotPubCnt < expectedPubCnt) {
      auto pub = kvStoreUpdatesQueue.get().value();
      gotPubCnt += readPublication(pub, got, gotDeleted);
    }

    EXPECT_EQ(expected, got);

    EXPECT_EQ(1, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrB).prefix_ref());
  }

  //
  // 5. Withdraw prefix1
  //    => A, C receive prefix withdrawal
  //
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(addr1));
    routeUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> got, gotDeleted;

    auto pub1 = kvStoreUpdatesQueue.get().value();
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreUpdatesQueue.get().value();
    readPublication(pub2, got, gotDeleted);

    EXPECT_EQ(0, got.size());

    EXPECT_EQ(2, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrA).prefix_ref());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrC).prefix_ref());
  }
}

class RouteOriginationFixture : public PrefixManagerTestFixture {
 public:
  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4, originatedPrefixV6;
    originatedPrefixV4.prefix_ref() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes_ref() = minSupportingRouteV4_;
    originatedPrefixV6.prefix_ref() = v6Prefix_;
    originatedPrefixV6.minimum_supporting_routes_ref() = minSupportingRouteV6_;
    originatedPrefixV6.install_to_fib_ref() = false;

    auto tConfig = PrefixManagerTestFixture::createConfig();
    tConfig.originated_prefixes_ref() = {
        originatedPrefixV4, originatedPrefixV6};
    return tConfig;
  }

  std::unordered_map<std::string, thrift::OriginatedPrefixEntry>
  getOriginatedPrefixDb() {
    std::unordered_map<std::string, thrift::OriginatedPrefixEntry> mp;
    while (mp.size() < 2) {
      auto prefixEntries = *prefixManager->getOriginatedPrefixes().get();
      for (auto const& prefixEntry : prefixEntries) {
        if (prefixEntry.prefix_ref()->prefix_ref() == v4Prefix_) {
          mp[v4Prefix_] = prefixEntry;
        }
        if (prefixEntry.prefix_ref()->prefix_ref() == v6Prefix_) {
          mp[v6Prefix_] = prefixEntry;
        }
      }
      std::this_thread::yield();
    }
    return mp;
  }

  std::optional<thrift::RouteDatabaseDelta>
  waitForRouteUpdate(
      messaging::RQueue<DecisionRouteUpdate>& reader,
      std::chrono::milliseconds timeout) {
    auto startTime = std::chrono::steady_clock::now();
    while (not reader.size()) {
      // break of timeout occurs
      auto now = std::chrono::steady_clock::now();
      if (now - startTime > timeout) {
        return std::nullopt;
      }
      // Yield the thread
      std::this_thread::yield();
    }
    return reader.get().value().toThrift();
  }

 protected:
  const std::string v4Prefix_ = "192.108.0.1/24";
  const std::string v6Prefix_ = "2001:1:2:3::1/64";
  const uint64_t minSupportingRouteV4_ = 1;
  const uint64_t minSupportingRouteV6_ = 2;
};

//
// Test case to verify prefix/attributes aligns with config read from
// `thrift::OpenrConfig`. This is the sanity check.
//
TEST_F(RouteOriginationFixture, ReadFromConfig) {
  // read via public API
  auto mp = getOriginatedPrefixDb();
  auto& prefixEntryV4 = mp.at(v4Prefix_);
  auto& prefixEntryV6 = mp.at(v6Prefix_);

  // verify attributes from originated prefix config
  EXPECT_EQ(0, prefixEntryV4.supporting_prefixes_ref()->size());
  EXPECT_EQ(0, prefixEntryV6.supporting_prefixes_ref()->size());
  EXPECT_FALSE(*prefixEntryV4.installed_ref());
  EXPECT_FALSE(*prefixEntryV6.installed_ref());
  EXPECT_EQ(v4Prefix_, *prefixEntryV4.prefix_ref()->prefix_ref());
  EXPECT_EQ(v6Prefix_, *prefixEntryV6.prefix_ref()->prefix_ref());
  EXPECT_EQ(
      minSupportingRouteV4_,
      *prefixEntryV4.prefix_ref()->minimum_supporting_routes_ref());
  EXPECT_EQ(
      minSupportingRouteV6_,
      *prefixEntryV6.prefix_ref()->minimum_supporting_routes_ref());
}

TEST_F(RouteOriginationFixture, BasicAdvertiseWithdraw) {
  // RQueue interface to read route updates
  auto reader = staticRouteUpdatesQueue.getReader();

  // ATTN: `area` must be populated for dstArea processing
  auto nh_1 = createNextHop(
      toBinaryAddress(Constants::kLocalRouteNexthopV4.toString()));
  auto nh_2 = createNextHop(
      toBinaryAddress(Constants::kLocalRouteNexthopV6.toString()));
  auto nh_3 = createNextHop(toBinaryAddress("fe80::1"));
  nh_1.area_ref() = thrift::Types_constants::kDefaultArea();
  nh_2.area_ref() = thrift::Types_constants::kDefaultArea();
  nh_3.area_ref().reset(); // empty next-hop

  const std::string v4Prefix_1 = "192.108.0.8/30";
  const std::string v6Prefix_1 = "2001:1:2:3::1/70";
  const std::string v4Prefix_2 = "192.108.1.2/32";
  const std::string v6Prefix_2 = "2001:1:2:3::1/128";
  const auto v4Network_1 = folly::IPAddress::createNetwork(v4Prefix_1);
  const auto v6Network_1 = folly::IPAddress::createNetwork(v6Prefix_1);
  const auto v4Network_2 = folly::IPAddress::createNetwork(v4Prefix_2);
  const auto v6Network_2 = folly::IPAddress::createNetwork(v6Prefix_2);

  // ATTN: PrefixType is unrelated for this testing
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
      {nh_1},
      prefixEntryV4_1,
      thrift::Types_constants::kDefaultArea());
  auto unicastEntryV6_1 = RibUnicastEntry(
      v6Network_1,
      {nh_2},
      prefixEntryV6_1,
      thrift::Types_constants::kDefaultArea());
  auto unicastEntryV4_2 = RibUnicastEntry(
      v4Network_2,
      {nh_1, nh_3},
      prefixEntryV4_2,
      thrift::Types_constants::kDefaultArea());
  auto unicastEntryV6_2 = RibUnicastEntry(
      v6Network_2,
      {nh_2, nh_3},
      prefixEntryV6_2,
      thrift::Types_constants::kDefaultArea());

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
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicastEntryV4_1);
    routeUpdate.addRouteToUpdate(unicastEntryV6_1);
    routeUpdatesQueue.push(std::move(routeUpdate));

    // v4 route update received
    auto update = waitForRouteUpdate(reader, kRouteUpdateTimeout);
    EXPECT_TRUE(update.has_value());

    auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
    auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();
    EXPECT_EQ(1, updatedRoutes.size());
    EXPECT_EQ(0, deletedRoutes.size());

    // verify thrift::NextHopThrift struct
    const auto& route = updatedRoutes.back();
    const auto& nhs = *route.nextHops_ref();
    EXPECT_EQ(toIpPrefix(v4Prefix_), *route.dest_ref());
    EXPECT_EQ(1, nhs.size());
    EXPECT_EQ(
        toBinaryAddress(Constants::kLocalRouteNexthopV4.toString()),
        *nhs.back().address_ref());

    // no more route update received
    EXPECT_FALSE(waitForRouteUpdate(reader, kRouteUpdateTimeout).has_value());

    // verificaiton via public API
    auto mp = getOriginatedPrefixDb();
    auto& prefixEntryV4 = mp.at(v4Prefix_);
    auto& prefixEntryV6 = mp.at(v6Prefix_);
    auto& supportingPrefixV4 = *prefixEntryV4.supporting_prefixes_ref();
    auto& supportingPrefixV6 = *prefixEntryV6.supporting_prefixes_ref();

    // v4Prefix - advertised, v6Prefix - NOT advertised
    EXPECT_TRUE(*prefixEntryV4.installed_ref());
    EXPECT_FALSE(*prefixEntryV6.installed_ref());

    // verify attributes
    EXPECT_EQ(1, supportingPrefixV4.size());
    EXPECT_EQ(1, supportingPrefixV6.size());
    EXPECT_EQ(
        supportingPrefixV4.back(),
        folly::IPAddress::networkToString(v4Network_1));
    EXPECT_EQ(
        supportingPrefixV6.back(),
        folly::IPAddress::networkToString(v6Network_1));
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
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicastEntryV4_2);
    routeUpdate.addRouteToUpdate(unicastEntryV6_2);
    routeUpdate.unicastRoutesToDelete.emplace_back(v6Network_2);
    routeUpdatesQueue.push(std::move(routeUpdate));

    // no more route update received
    EXPECT_FALSE(waitForRouteUpdate(reader, kRouteUpdateTimeout).has_value());

    // verificaiton via public API
    auto mp = getOriginatedPrefixDb();
    auto& prefixEntryV4 = mp.at(v4Prefix_);
    auto& prefixEntryV6 = mp.at(v6Prefix_);
    auto& supportingPrefixV4 = *prefixEntryV4.supporting_prefixes_ref();
    auto& supportingPrefixV6 = *prefixEntryV6.supporting_prefixes_ref();

    // v4Prefix - advertised, v6Prefix - advertised
    EXPECT_TRUE(*prefixEntryV4.installed_ref());
    EXPECT_FALSE(*prefixEntryV6.installed_ref());

    // verify attributes
    EXPECT_EQ(1, supportingPrefixV4.size());
    EXPECT_EQ(1, supportingPrefixV6.size());
    EXPECT_EQ(
        supportingPrefixV6.back(),
        folly::IPAddress::networkToString(v6Network_1));
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
  //  - supporting routes vector doesn't change as same update is ignored
  //  - v6Prefix_ will NOT be advertised even `min_supporting_route=2`
  //    as `install_to_fib=false`;
  //
  {
    DecisionRouteUpdate routeUpdate;
    // ATTN: change ribEntry attributes to make sure no impact on ref-count
    auto tmpEntryV4 = unicastEntryV4_1;
    tmpEntryV4.nexthops = {createNextHop(toBinaryAddress("192.168.0.1"))};
    routeUpdate.addRouteToUpdate(tmpEntryV4);
    routeUpdate.addRouteToUpdate(unicastEntryV6_2);
    routeUpdatesQueue.push(std::move(routeUpdate));

    // no more route update received
    EXPECT_FALSE(waitForRouteUpdate(reader, kRouteUpdateTimeout).has_value());

    // verificaiton via public API
    auto mp = getOriginatedPrefixDb();
    auto& prefixEntryV4 = mp.at(v4Prefix_);
    auto& prefixEntryV6 = mp.at(v6Prefix_);
    auto& supportingPrefixV4 = *prefixEntryV4.supporting_prefixes_ref();
    auto& supportingPrefixV6 = *prefixEntryV6.supporting_prefixes_ref();

    // v4Prefix - advertised, v6Prefix - advertised
    EXPECT_TRUE(*prefixEntryV4.installed_ref());
    EXPECT_TRUE(*prefixEntryV6.installed_ref());

    // verify supporting routes vector doesn't change
    EXPECT_EQ(1, supportingPrefixV4.size());
    EXPECT_EQ(2, supportingPrefixV6.size());
    EXPECT_TRUE(
        supportingPrefixV6.back() ==
            folly::IPAddress::networkToString(v6Network_2) or
        supportingPrefixV6.front() ==
            folly::IPAddress::networkToString(v6Network_2));
    EXPECT_NE(supportingPrefixV6.back(), supportingPrefixV6.front());
  }

  // Step4: Withdraw:
  //  - 1 supporting route of v4Prefix;
  //  - 1 supporting route for v6Prefix;
  //  - 1 unrelated v6 ribEntry;
  // Expect:
  //  - v4Prefix_ is withdrawn as `supporting_route_cnt=0`;
  //  - # of supporting prefix for v6Prefix_ will shrink to 1,
  //    but NOT shown as withdrawn as it will be ignored;
  //
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(v4Network_1);
    routeUpdate.unicastRoutesToDelete.emplace_back(v6Network_1);
    // intentionally inject random non-existing prefix
    routeUpdate.unicastRoutesToDelete.emplace_back(
        folly::IPAddress::createNetwork("fe80::2"));
    routeUpdatesQueue.push(std::move(routeUpdate));

    auto mp = getOriginatedPrefixDb();
    auto& prefixEntryV4 = mp.at(v4Prefix_);
    auto& prefixEntryV6 = mp.at(v6Prefix_);
    auto& supportingPrefixV4 = *prefixEntryV4.supporting_prefixes_ref();
    auto& supportingPrefixV6 = *prefixEntryV6.supporting_prefixes_ref();

    // v4Prefix - withdrawn, v6Prefix - advertised
    EXPECT_FALSE(*prefixEntryV4.installed_ref());
    EXPECT_FALSE(*prefixEntryV6.installed_ref());

    // verify attributes
    EXPECT_EQ(0, supportingPrefixV4.size());
    EXPECT_EQ(1, supportingPrefixV6.size());
    EXPECT_EQ(
        supportingPrefixV6.back(),
        folly::IPAddress::networkToString(v6Network_2));

    // v4/v6 route withdrawn updates received
    auto update = waitForRouteUpdate(reader, kRouteUpdateTimeout);
    EXPECT_TRUE(update.has_value());

    auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
    auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();

    // verify both v4/v6 prefixes get dropped
    EXPECT_EQ(0, updatedRoutes.size());
    EXPECT_EQ(1, deletedRoutes.size());

    EXPECT_TRUE(toIpPrefix(v4Prefix_) == deletedRoutes.back());
  }
}

TEST(PrefixManagerPendingUpdates, updatePrefixes) {
  detail::PrefixManagerPendingUpdates updates;

  // verify initial state
  EXPECT_TRUE(updates.getChangedPrefixes().empty());

  // empty update will ONLY change counter
  updates.applyPrefixChange({});
  EXPECT_TRUE(updates.getChangedPrefixes().empty());

  // non-empty change
  auto network1 = toIPNetwork(addr1);
  auto network2 = toIPNetwork(addr2);
  updates.applyPrefixChange({network1, network2});
  EXPECT_THAT(
      updates.getChangedPrefixes(),
      testing::UnorderedElementsAre(network1, network2));

  // cleanup
  updates.reset();
  EXPECT_TRUE(updates.getChangedPrefixes().empty());
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);

  // Run the tests
  return RUN_ALL_TESTS();
}
