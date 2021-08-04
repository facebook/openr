/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
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
const uint32_t label1 = 65001;
const uint32_t label2 = 65002;

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
const auto prefixEntry1WithLabel1 =
    createPrefixEntryWithPrependLabel(addr1, label1);
const auto prefixEntry1WithLabel2 =
    createPrefixEntryWithPrependLabel(addr1, label2);
const auto prefixEntry2WithLabel1 =
    createPrefixEntryWithPrependLabel(addr2, label1);
const auto prefixEntry3WithLabel2 =
    createPrefixEntryWithPrependLabel(addr3, label2);
const auto prefixEntry4WithLabel1 =
    createPrefixEntryWithPrependLabel(addr4, label1);
const auto prefixEntry5WithLabel2 =
    createPrefixEntryWithPrependLabel(addr5, label2);
const auto prefixEntry6WithLabel1 =
    createPrefixEntryWithPrependLabel(addr6, label1);
const auto prefixEntry7WithLabel2 =
    createPrefixEntryWithPrependLabel(addr7, label2);

} // namespace

class PrefixManagerTestFixture : public testing::Test {
 public:
  void
  SetUp() override {
    // create config
    config = std::make_shared<Config>(createConfig());

    // spin up a kvstore
    kvStoreWrapper = std::make_shared<KvStoreWrapper>(
        context, config, std::nullopt, kvRequestQueue.getReader());
    kvStoreWrapper->run();
    LOG(INFO) << "The test KV store is running";

    // spin up a prefix-mgr
    createPrefixManager(config);
  }

  void
  TearDown() override {
    // Close queues
    closeQueue();

    // cleanup kvStoreClient
    if (kvStoreClient) {
      kvStoreClient.reset();
    }

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
    staticRouteUpdatesQueue.close();
    fibRouteUpdatesQueue.close();
    kvStoreWrapper->closeQueue();
  }

  void
  openQueue() {
    kvRequestQueue.open();
    prefixUpdatesQueue.open();
    staticRouteUpdatesQueue.open();
    fibRouteUpdatesQueue.open();
    kvStoreWrapper->openQueue();
  }

  void
  createPrefixManager(std::shared_ptr<Config> cfg) {
    // start a prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        staticRouteUpdatesQueue,
        kvRequestQueue,
        prefixUpdatesQueue.getReader(),
        fibRouteUpdatesQueue.getReader(),
        cfg,
        kvStoreWrapper->getKvStore());

    prefixManagerThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "PrefixManager thread starting";
      prefixManager->run();
      LOG(INFO) << "PrefixManager thread finishing";
    });
    prefixManager->waitUntilRunning();
  }

  virtual thrift::OpenrConfig
  createConfig() {
    auto tConfig = getBasicOpenrConfig(nodeId_);
    tConfig.kvstore_config_ref()->sync_interval_s_ref() = 1;
    tConfig.enable_fib_ack_ref() = true;
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
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue;

  // Create the serializer for write/read
  CompactSerializer serializer;
  std::shared_ptr<Config> config{nullptr};
  std::unique_ptr<PrefixManager> prefixManager{nullptr};
  std::unique_ptr<std::thread> prefixManagerThread{nullptr};
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper{nullptr};
  std::unique_ptr<KvStoreClientInternal> kvStoreClient{nullptr};
};

class PrefixManagerPrefixKeyFormatTestFixture
    : public PrefixManagerTestFixture {
 protected:
  thrift::OpenrConfig
  createConfig() override {
    auto tConfig = getBasicOpenrConfig(nodeId_);
    tConfig.kvstore_config_ref()->sync_interval_s_ref() = 1;
    tConfig.enable_new_prefix_format_ref() = 1;

    return tConfig;
  }
};

/*
 * This is to test backward compatibility between old and new prefix key format.
 * We will make sure:
 *  1) There will be no crash due to parsing old/new prefix keys;
 *  2) Prefix key format upgrade/downgrade is supported;
 */
TEST_F(
    PrefixManagerPrefixKeyFormatTestFixture,
    PrefixKeyFormatBackwardCompatibility) {
  // Make sure we have old format of keys added
  auto prefixKey1 = PrefixKey(
      nodeId_, toIPNetwork(*prefixEntry1.prefix_ref()), kTestingAreaName, true);
  auto prefixKey2 = PrefixKey(
      nodeId_, toIPNetwork(*prefixEntry2.prefix_ref()), kTestingAreaName, true);

  // ATTN: remember v1 format of keys for future validation.
  auto keyStr1 = prefixKey1.getPrefixKeyV2();
  auto keyStr2 = prefixKey2.getPrefixKeyV2();

  // Inject 2 prefixes and validate format prefixStr
  {
    prefixManager->advertisePrefixes({prefixEntry1, prefixEntry2}).get();

    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(2, pub.keyVals_ref()->size());
    EXPECT_EQ(1, pub.keyVals_ref()->count(keyStr1));
    EXPECT_EQ(1, pub.keyVals_ref()->count(keyStr2));
  }

  // Withdraw 1 out of 2 previously advertised prefixes and validate format
  // prefixStr ATTNL: make sure there will be no crash for prefix manager
  {
    prefixManager->withdrawPrefixes({prefixEntry1}).get();

    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());
    EXPECT_EQ(1, pub.keyVals_ref()->count(prefixKey1.getPrefixKeyV2()));
  }

  // ATTN: this is to mimick the "downgrade/roll-back" step of PrefixManager
  // with `enable_new_prefix_format` change from true to false.
  {
    // mimick PrefixManager shutting down procedure
    closeQueue();

    prefixManager->stop();
    prefixManagerThread->join();
    prefixManager.reset();

    // mimick PrefixManager restarting procedure with knob turned off
    openQueue();
    auto cfg =
        std::make_shared<Config>(PrefixManagerTestFixture::createConfig());
    createPrefixManager(cfg); // util call will override `prefixManager`

    // Wait for throttled update
    // consider prefixManager throttle + kvstoreClientInternal throttle
    evb.scheduleTimeout(
        std::chrono::milliseconds(
            3 * Constants::kKvStoreSyncThrottleTimeout.count()),
        [&]() {
          // Wait for throttled update to announce to kvstore
          auto maybeValue1 = kvStoreWrapper->getKey(kTestingAreaName, keyStr1);
          auto maybeValue2 = kvStoreWrapper->getKey(kTestingAreaName, keyStr2);
          EXPECT_TRUE(maybeValue1.has_value());
          EXPECT_TRUE(maybeValue2.has_value());

          auto db1 = readThriftObjStr<thrift::PrefixDatabase>(
              maybeValue1.value().value_ref().value(), serializer);
          auto db2 = readThriftObjStr<thrift::PrefixDatabase>(
              maybeValue2.value().value_ref().value(), serializer);
          EXPECT_EQ(*db2.deletePrefix_ref(), false);
          EXPECT_EQ(*db1.deletePrefix_ref(), false);

          evb.stop();
        });

    // let magic happen
    evb.run();
  }
}

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
        prefixManager->advertisePrefixes({prefixEntry9}).get();
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
            maybeValue3.value().value_ref().value(), serializer);
        EXPECT_EQ(6, getNumPrefixes(prefixDbMarker));

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

  // TODO - reevaluate subscribeKey + folly::Baton for pushing along the UTs
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
      kvRequestQueue,
      prefixUpdatesQueue.getReader(),
      fibRouteUpdatesQueue.getReader(),
      config,
      kvStoreWrapper->getKvStore());

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
  fibRouteUpdatesQueue.close();
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
        PrefixEventType::WITHDRAW_PREFIXES,
        thrift::PrefixType::DEFAULT,
        {prefixEntry3});
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore (PrefixManager has processed the update)
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());

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
    // Send update request in queue
    PrefixEvent event(
        PrefixEventType::ADD_PREFIXES, thrift::PrefixType::VIP, {}, {});
    event.prefixEntries.push_back(cPrefixEntry);
    prefixUpdatesQueue.push(std::move(event));

    // Wait for update in KvStore
    // ATTN: both prefixes should be updated via throttle
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(1, prefixes->size());
    EXPECT_THAT(*prefixes, testing::Contains(prefixEntry9));
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
    EXPECT_EQ(1, pub.keyVals_ref()->size());

    // Verify
    auto prefixes = prefixManager->getPrefixes().get();
    EXPECT_EQ(0, prefixes->size());
  }
}

/**
 * Validate PrefixManager does not advertise prefixes with prepend labels to
 * KvStore, until receiving from Fib that associated label routes are already
 * programmed. Both FULL_SYNC and INCREMENTAL route update types are tested.
 * 1. Prefixes with prepend labels are advertised after FULL_SYNC route updates
 *    of all labels are received.
 * 2. INCREMENTAL delete of label route updates blocks the advertisement of
 *    follow-up prefix updates with deleted label routes.
 * 3. Next INCREMENTAL route update for above deleted label triggers the
 *    advertisement of above cached prefixes with prepend label.
 * 4. Follow-up FULL_SYNC route updates reset previously stored programmed
 *    labels in PrefixManager. Only prefixes with newly programmed label routes
 *    will be advertised.
 */
TEST_F(PrefixManagerTestFixture, FibAckForPrefixesWithMultiLabels) {
  // 1. Prefixes with prepend labels are advertised after FULL_SYNC route
  // updates of all labels are received.
  {
    // PrefixManager receives updates of prefixes with prepend label.
    PrefixEvent prefixEvent(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::BGP,
        {prefixEntry1WithLabel1,
         prefixEntry2WithLabel1,
         prefixEntry3WithLabel2});
    prefixUpdatesQueue.push(std::move(prefixEvent));

    // Full sync of programmed routes for label1/2 arrives.
    DecisionRouteUpdate fullSyncUpdates;
    fullSyncUpdates.type = DecisionRouteUpdate::FULL_SYNC;
    fullSyncUpdates.mplsRoutesToUpdate = {
        {label1, RibMplsEntry(label1)}, {label2, RibMplsEntry(label2)}};
    fibRouteUpdatesQueue.push(std::move(fullSyncUpdates));

    // Wait for update in KvStore
    // ATTN: prefixes with label1/2 should be updated via throttle.
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(3, pub.keyVals_ref()->size());
  }

  // 2. INCREMENTAL delete of label route updates blocks the advertisement of
  // follow-up prefix updates with deleted label routes.
  // NOTE: In practice, prefixes with deleting label should not appear in
  // prefixUpdatesQueue. BgpSpeaker replaces deleting label with new one in
  // prefix attributes. It sends new label routes for programming, meanwhile
  // sending prefixes with new label to prefixUpdatesQueue. Added the scenario
  // here just for test purposes.
  {
    // Route of label1 was deleted.
    DecisionRouteUpdate deletedRoutes;
    deletedRoutes.type = DecisionRouteUpdate::INCREMENTAL;
    deletedRoutes.mplsRoutesToDelete = {label1};
    fibRouteUpdatesQueue.push(std::move(deletedRoutes));

    // PrefixManager receives prefix updates of  with prepend label.
    PrefixEvent prefixEvent(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::BGP,
        {prefixEntry4WithLabel1, prefixEntry5WithLabel2});
    prefixUpdatesQueue.push(std::move(prefixEvent));

    // Wait for update in KvStore
    // ATTN: prefixes with label2 should be updated via throttle.
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());
  }

  // 3. Next INCREMENTAL route update including above deleted label triggers the
  // advertisement of above cached prefixes with prepend label.
  {
    // Routes of label1/2 was programmed.
    DecisionRouteUpdate updateRoutes;
    updateRoutes.type = DecisionRouteUpdate::INCREMENTAL;
    updateRoutes.mplsRoutesToUpdate = {
        {label1, RibMplsEntry(label1)}, {label2, RibMplsEntry(label2)}};
    fibRouteUpdatesQueue.push(std::move(updateRoutes));

    // Wait for update in KvStore
    // ATTN: prefixes with label1 should be updated via throttle.
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());
  }

  // 4. Follow-up FULL_SYNC route updates reset previously
  // stored programmed labels in PrefixManager. Only prefixes with newly
  // programmed label routes will be advertised.
  {
    // Full sync of programmed routes for label2 arrives.
    DecisionRouteUpdate fullSyncUpdates;
    fullSyncUpdates.type = DecisionRouteUpdate::FULL_SYNC;
    fullSyncUpdates.mplsRoutesToUpdate = {{label2, RibMplsEntry(label2)}};
    fibRouteUpdatesQueue.push(std::move(fullSyncUpdates));

    // PrefixManager receives prefix updates of  with prepend label.
    PrefixEvent prefixEvent(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::BGP,
        {prefixEntry6WithLabel1, prefixEntry7WithLabel2});
    prefixUpdatesQueue.push(std::move(prefixEvent));

    // Wait for update in KvStore
    // ATTN: prefixes with label2 should be updated via throttle.
    auto pub = kvStoreWrapper->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());
  }

  auto prefixes = prefixManager->getPrefixes().get();
  EXPECT_EQ(7, prefixes->size());
  EXPECT_THAT(
      *prefixes,
      testing::UnorderedElementsAre(
          prefixEntry1WithLabel1,
          prefixEntry2WithLabel1,
          prefixEntry3WithLabel2,
          prefixEntry4WithLabel1,
          prefixEntry5WithLabel2,
          prefixEntry6WithLabel1,
          prefixEntry7WithLabel2));
}

/**
 * Validate PrefixManager does not advertise one prefix with prepend labels to
 * KvStore, until receiving from Fib that associated label routes are already
 * programmed.
 * 1. Advertise <Prefix, Label=none>
 * 2. Donot advertise prefix update of <Prefix, Label1>
 * 3. Recived Label1 routes programmed signal; <Prefix, Label1> gets advertised.
 * 4. Prefix Update <Prefix, Label2> not advertised
 * 5. Prefix Update <Prefix, Label=none> gets updated again.
 *
 * TODO: PrefixManager needs to purge already advertised prefixes if associated
 * prefixes are unprogrammed later.
 */
TEST_F(PrefixManagerTestFixture, FibAckForOnePrefixWithMultiLabels) {
  int scheduleAt{0};

  auto prefixKey = PrefixKey(
      nodeId_, toIPNetwork(*prefixEntry1Bgp.prefix_ref()), kTestingAreaName);
  auto prefixKeyStr = prefixKey.getPrefixKey();

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // 1. PrefixManager receives updates of one prefix without label.
        PrefixEvent prefixEvent(
            PrefixEventType::ADD_PREFIXES,
            thrift::PrefixType::BGP,
            {prefixEntry1Bgp});
        prefixUpdatesQueue.push(std::move(prefixEvent));

        // Wait for update in KvStore
        auto pub = kvStoreWrapper->recvPublication();
        EXPECT_EQ(1, pub.keyVals_ref()->size());

        // ATTN: prefixes without label should be updated via throttle.
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_EQ(db.prefixEntries_ref()->size(), 1);
        EXPECT_FALSE(db.prefixEntries_ref()[0].prependLabel_ref().has_value());

        // 2.1. PrefixManager receives <Prefix, Label1>.
        PrefixEvent prefixEvent1(
            PrefixEventType::ADD_PREFIXES,
            thrift::PrefixType::BGP,
            {prefixEntry1WithLabel1});
        prefixUpdatesQueue.push(std::move(prefixEvent1));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // 2.2. Donot advertise prefix update of <Prefix, Label1> since label
        // routes has not programmed yet.
        // Note: previously advertised prefix with null label still persists.
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_EQ(db.prefixEntries_ref()->size(), 1);
        // Prepend label is still unset.
        EXPECT_FALSE(db.prefixEntries_ref()[0].prependLabel_ref().has_value());

        // 3.1. Recived route programmed signal of Label1.
        DecisionRouteUpdate routeUpdates;
        routeUpdates.type = DecisionRouteUpdate::INCREMENTAL;
        routeUpdates.mplsRoutesToUpdate = {{label1, RibMplsEntry(label1)}};
        fibRouteUpdatesQueue.push(std::move(routeUpdates));

        // Wait for update in KvStore
        auto pub = kvStoreWrapper->recvPublication();
        EXPECT_EQ(1, pub.keyVals_ref()->size());

        // 3.2. <Prefix, Label1> gets advertised.
        maybeValue = kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_EQ(db.prefixEntries_ref()->size(), 1);
        EXPECT_EQ(db.prefixEntries_ref()[0].prependLabel_ref().value(), label1);

        // 4.1. PrefixManager receives <Prefix, Label2>.
        prefixUpdatesQueue.push(PrefixEvent(
            PrefixEventType::ADD_PREFIXES,
            thrift::PrefixType::BGP,
            {prefixEntry1WithLabel2}));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 3 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // 4.2. Donot advertise prefix update of <Prefix, Label2> since label
        // routes has not programmed yet.
        auto maybeValue =
            kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_EQ(db.prefixEntries_ref()->size(), 1);
        // Prepend label is still label1.
        EXPECT_EQ(db.prefixEntries_ref()[0].prependLabel_ref().value(), label1);

        // 5.1. PrefixManager receives <Prefix, Label=none>.
        prefixUpdatesQueue.push(PrefixEvent(
            PrefixEventType::ADD_PREFIXES,
            thrift::PrefixType::BGP,
            {prefixEntry1}));

        // Wait for update in KvStore
        auto pub = kvStoreWrapper->recvPublication();
        EXPECT_EQ(1, pub.keyVals_ref()->size());

        // 5.2. <Prefix, Label=none> gets advertised.
        maybeValue = kvStoreWrapper->getKey(kTestingAreaName, prefixKeyStr);
        EXPECT_TRUE(maybeValue.has_value());
        db = readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value_ref().value(), serializer);
        EXPECT_EQ(*db.thisNodeName_ref(), nodeId_);
        EXPECT_EQ(db.prefixEntries_ref()->size(), 1);
        EXPECT_FALSE(db.prefixEntries_ref()[0].prependLabel_ref().has_value());

        evb.stop();
      });

  // let magic happen
  evb.run();
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
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixType_ref() = thrift::PrefixType::BGP;
    auto routes = prefixManager->getAdvertisedRoutesFiltered(filter).get();
    ASSERT_EQ(0, routes->size());
  }

  //
  // Filter on non-existing type (VIP)
  //
  {
    thrift::AdvertisedRouteFilter filter;
    filter.prefixType_ref() = thrift::PrefixType::VIP;
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
      routes, filter.prefixType_ref(), folly::CIDRNetwork(), entries);
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
 * Test cross-AREA behavior for Decision RIB routes:
 *  1. prefix update;
 *  2. nexthop update;
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
  // Set non-transitive attributes.
  prefixEntry1A.forwardingAlgorithm_ref() =
      thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
  prefixEntry1A.forwardingType_ref() = thrift::PrefixForwardingType::SR_MPLS;
  prefixEntry1A.minNexthop_ref() = 10;
  prefixEntry1A.prependLabel_ref() = 70000;

  auto unicast1A = RibUnicastEntry(
      toIPNetwork(addr1), {path1_2_1}, prefixEntry1A, "A", false);
  // expected kvstore announcement to other area, append "A" in area stack
  auto expectedPrefixEntry1A = prefixEntry1A;
  expectedPrefixEntry1A.area_stack_ref()->push_back("A");
  ++*expectedPrefixEntry1A.metrics_ref()->distance_ref();
  expectedPrefixEntry1A.type_ref() = thrift::PrefixType::RIB;
  // Non-transitive attributes should not be reset before redistribution.
  expectedPrefixEntry1A.forwardingAlgorithm_ref() =
      thrift::PrefixForwardingAlgorithm();
  expectedPrefixEntry1A.forwardingType_ref() = thrift::PrefixForwardingType();
  expectedPrefixEntry1A.minNexthop_ref().reset();
  expectedPrefixEntry1A.prependLabel_ref().reset();

  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1A);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrB, expectedPrefixEntry1A);
    expected.emplace(keyStrC, expectedPrefixEntry1A);

    auto pub1 = kvStoreUpdatesQueue.get().value().tPublication;
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreUpdatesQueue.get().value().tPublication;
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
  prefixEntry1B.prependLabel_ref() = 70001;
  auto unicast1B = RibUnicastEntry(
      toIPNetwork(addr1), {path1_2_2}, prefixEntry1B, "B", false);
  // expected kvstore announcement to other area, append "B" in area stack
  auto expectedPrefixEntry1B = prefixEntry1B;
  expectedPrefixEntry1B.area_stack_ref()->push_back("B");
  ++*expectedPrefixEntry1B.metrics_ref()->distance_ref();
  expectedPrefixEntry1B.type_ref() = thrift::PrefixType::RIB;
  // Prepend label is not expected to be leak into other areas.
  expectedPrefixEntry1B.prependLabel_ref().reset();

  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicast1B);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrA, expectedPrefixEntry1B);
    expected.emplace(keyStrC, expectedPrefixEntry1B);

    auto pub1 = kvStoreUpdatesQueue.get().value().tPublication;
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreUpdatesQueue.get().value().tPublication;
    readPublication(pub2, got, gotDeleted);

    auto pub3 = kvStoreUpdatesQueue.get().value().tPublication;
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
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> got, gotDeleted;

    auto pub1 = kvStoreUpdatesQueue.get().value().tPublication;
    readPublication(pub1, got, gotDeleted);

    auto pub2 = kvStoreUpdatesQueue.get().value().tPublication;
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
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrC, expectedPrefixEntry1A);

    auto pub1 = kvStoreUpdatesQueue.get().value().tPublication;
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

    std::map<std::string, thrift::PrefixEntry> got, gotDeleted;

    auto pub1 = kvStoreUpdatesQueue.get().value().tPublication;
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
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrB, expectedPrefixEntry1A);

    auto pub1 = kvStoreUpdatesQueue.get().value().tPublication;
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
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> expected, got, gotDeleted;
    expected.emplace(keyStrA, expectedPrefixEntry1B);
    expected.emplace(keyStrC, expectedPrefixEntry1B);

    // this test is long, we might hit ttl updates
    // here skip ttl updates
    int expectedPubCnt{3}, gotPubCnt{0};
    while (gotPubCnt < expectedPubCnt) {
      auto pub = kvStoreUpdatesQueue.get().value().tPublication;
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
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    std::map<std::string, thrift::PrefixEntry> got, gotDeleted;

    while (gotDeleted.size() < 2) {
      auto pub = kvStoreUpdatesQueue.get().value().tPublication;
      readPublication(pub, got, gotDeleted);
    }

    EXPECT_EQ(0, got.size());
    EXPECT_EQ(2, gotDeleted.size());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrA).prefix_ref());
    EXPECT_EQ(addr1, *gotDeleted.at(keyStrC).prefix_ref());
  }
}

class RouteOriginationFixture : public PrefixManagerMultiAreaTestFixture {
 public:
  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4, originatedPrefixV6;
    originatedPrefixV4.prefix_ref() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes_ref() = minSupportingRouteV4_;
    originatedPrefixV4.install_to_fib_ref() = true;
    originatedPrefixV6.prefix_ref() = v6Prefix_;
    originatedPrefixV6.minimum_supporting_routes_ref() = minSupportingRouteV6_;

    auto tConfig = PrefixManagerMultiAreaTestFixture::createConfig();
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

  // return false if publication is tll update.
  void
  waitForKvStorePublication(
      messaging::RQueue<Publication>& reader,
      std::unordered_map<std::string, thrift::PrefixEntry>& exp,
      std::unordered_set<std::string>& expDeleted) {
    while (exp.size() or expDeleted.size()) {
      auto pub = reader.get().value();
      for (const auto& [key, thriftVal] : *pub.tPublication.keyVals_ref()) {
        if (not thriftVal.value_ref().has_value()) {
          // skip TTL update
          continue;
        }

        auto db = readThriftObjStr<thrift::PrefixDatabase>(
            thriftVal.value_ref().value(), serializer);
        auto isDeleted = *db.deletePrefix_ref();
        auto prefixEntry = db.prefixEntries_ref()->at(0);
        if (isDeleted and expDeleted.count(key)) {
          VLOG(2) << "Withdraw of prefix: " << key << " received";
          expDeleted.erase(key);
        }
        if ((not isDeleted) and exp.count(key) and prefixEntry == exp.at(key)) {
          VLOG(2) << "Advertising of prefix: " << key << " received";
          exp.erase(key);
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

  const std::string keyStrAV4_ =
      PrefixKey(nodeId_, v4Network_, "A").getPrefixKey();
  const std::string keyStrBV4_ =
      PrefixKey(nodeId_, v4Network_, "B").getPrefixKey();
  const std::string keyStrCV4_ =
      PrefixKey(nodeId_, v4Network_, "C").getPrefixKey();
  const std::string keyStrAV6_ =
      PrefixKey(nodeId_, v6Network_, "A").getPrefixKey();
  const std::string keyStrBV6_ =
      PrefixKey(nodeId_, v6Network_, "B").getPrefixKey();
  const std::string keyStrCV6_ =
      PrefixKey(nodeId_, v6Network_, "C").getPrefixKey();
};

class RouteOriginationOverrideFixture : public RouteOriginationFixture {
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
    thrift::OriginatedPrefix originatedPrefixV4, originatedPrefixV6;
    originatedPrefixV4.prefix_ref() = v4Prefix_;
    originatedPrefixV6.prefix_ref() = v6Prefix_;

    // ATTN: specify supporting route cnt to be 0 for immediate advertisement
    originatedPrefixV4.minimum_supporting_routes_ref() = 0;
    originatedPrefixV6.minimum_supporting_routes_ref() = 0;

    // Make sure we install the prefixes to FIB
    originatedPrefixV4.install_to_fib_ref() = true;
    originatedPrefixV6.install_to_fib_ref() = false; // we can check both cases

    auto tConfig = PrefixManagerMultiAreaTestFixture::createConfig();
    tConfig.originated_prefixes_ref() = {
        originatedPrefixV4, originatedPrefixV6};
    return tConfig;
  }

  std::shared_ptr<messaging::RQueue<DecisionRouteUpdate>>
  getEarlyStaticRoutesReaderPtr() {
    return earlyStaticRoutesReaderPtr;
  }

 private:
  std::shared_ptr<messaging::RQueue<DecisionRouteUpdate>>
      earlyStaticRoutesReaderPtr;
};

TEST_F(RouteOriginationOverrideFixture, StaticRoutesAnnounce) {
  auto staticRoutesReaderPtr = getEarlyStaticRoutesReaderPtr();

  auto update = waitForRouteUpdate(*staticRoutesReaderPtr, kRouteUpdateTimeout);
  EXPECT_TRUE(update.has_value());

  auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
  auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();
  EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
  EXPECT_THAT(deletedRoutes, testing::SizeIs(0));

  // Verify that the destination is the v4 address
  const auto& route = updatedRoutes.back();
  EXPECT_EQ(*route.dest_ref(), toIpPrefix(v4Prefix_));

  // Verify the next hop address is also v4 address
  const auto& nhs = *route.nextHops_ref();
  EXPECT_THAT(nhs, testing::SizeIs(1));
  EXPECT_THAT(
      nhs,
      testing::UnorderedElementsAre(createNextHop(
          toBinaryAddress(Constants::kLocalRouteNexthopV4.toString()))));
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
  EXPECT_EQ(0, prefixEntryV4.supporting_prefixes_ref()->size());
  EXPECT_EQ(0, prefixEntryV6.supporting_prefixes_ref()->size());
  EXPECT_TRUE(*prefixEntryV4.installed_ref());
  EXPECT_TRUE(*prefixEntryV6.installed_ref());
  EXPECT_EQ(v4Prefix_, *prefixEntryV4.prefix_ref()->prefix_ref());
  EXPECT_EQ(v6Prefix_, *prefixEntryV6.prefix_ref()->prefix_ref());
  EXPECT_EQ(0, *prefixEntryV4.prefix_ref()->minimum_supporting_routes_ref());
  EXPECT_EQ(0, *prefixEntryV6.prefix_ref()->minimum_supporting_routes_ref());

  // prefixes originated have specific thrift::PrefixType::CONFIG
  const auto bestPrefixEntryV4_ =
      createPrefixEntry(toIpPrefix(v4Prefix_), thrift::PrefixType::CONFIG);
  const auto bestPrefixEntryV6_ =
      createPrefixEntry(toIpPrefix(v6Prefix_), thrift::PrefixType::CONFIG);

  // v4Prefix_ is advertised to ALL areas configured
  std::unordered_map<std::string, thrift::PrefixEntry> exp({
      {keyStrAV4_, bestPrefixEntryV4_},
      {keyStrBV4_, bestPrefixEntryV4_},
      {keyStrCV4_, bestPrefixEntryV4_},
      {keyStrAV6_, bestPrefixEntryV6_},
      {keyStrBV6_, bestPrefixEntryV6_},
      {keyStrCV6_, bestPrefixEntryV6_},
  });
  std::unordered_set<std::string> expDeleted{};

  // wait for condition to be met for KvStore publication
  waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
}

TEST_F(RouteOriginationFixture, BasicAdvertiseWithdraw) {
  // RQueue interface to read route updates
  auto staticRoutesReader = staticRouteUpdatesQueue.getReader();
  auto kvStoreUpdatesReader = kvStoreWrapper->getReader();

  // dummy nexthop
  auto nh_3 = createNextHop(toBinaryAddress("fe80::1"));
  nh_3.area_ref().reset(); // empty area

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
      thrift::Types_constants::kDefaultArea());
  auto unicastEntryV6_1 = RibUnicastEntry(
      v6Network_1,
      {nh_v6},
      prefixEntryV6_1,
      thrift::Types_constants::kDefaultArea());
  auto unicastEntryV4_2 = RibUnicastEntry(
      v4Network_2,
      {nh_v4, nh_3},
      prefixEntryV4_2,
      thrift::Types_constants::kDefaultArea());
  auto unicastEntryV6_2 = RibUnicastEntry(
      v6Network_2,
      {nh_v6, nh_3},
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
  VLOG(1) << "Starting test step 1...";
  {
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(unicastEntryV4_1);
    routeUpdate.addRouteToUpdate(unicastEntryV6_1);
    fibRouteUpdatesQueue.push(std::move(routeUpdate));

    // Verify 1): PrefixManager -> Decision update
    {
      // v4 route update received
      auto update = waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout);
      EXPECT_TRUE(update.has_value());

      auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
      auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();
      EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
      EXPECT_THAT(deletedRoutes, testing::SizeIs(0));

      // verify thrift::NextHopThrift struct
      const auto& route = updatedRoutes.back();
      EXPECT_EQ(*route.dest_ref(), toIpPrefix(v4Prefix_));

      const auto& nhs = *route.nextHops_ref();
      EXPECT_THAT(nhs, testing::SizeIs(1));
      EXPECT_THAT(
          nhs,
          testing::UnorderedElementsAre(createNextHop(
              toBinaryAddress(Constants::kLocalRouteNexthopV4.toString()))));

      // no v6 route update received
      EXPECT_FALSE(waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout)
                       .has_value());
    }

    // Verify 2): PrefixManager -> KvStore update
    {
      // v4Prefix_ is advertised to ALL areas configured
      std::unordered_map<std::string, thrift::PrefixEntry> exp({
          {keyStrAV4_, bestPrefixEntryV4_},
          {keyStrBV4_, bestPrefixEntryV4_},
          {keyStrCV4_, bestPrefixEntryV4_},
      });
      std::unordered_set<std::string> expDeleted{};

      // wait for condition to be met for KvStore publication
      waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
    }

    // Verify 3): PrefixManager's public API
    {
      auto mp = getOriginatedPrefixDb();
      auto& prefixEntryV4 = mp.at(v4Prefix_);
      auto& prefixEntryV6 = mp.at(v6Prefix_);

      // v4Prefix - advertised, v6Prefix - NOT advertised
      EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                    return *i.installed_ref() == true and
                        i.supporting_prefixes_ref()->size() == 1;
                  }));
      EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                    return *i.installed_ref() == false and
                        i.supporting_prefixes_ref()->size() == 1;
                  }));

      EXPECT_THAT(
          *prefixEntryV4.supporting_prefixes_ref(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v4Network_1)));
      EXPECT_THAT(
          *prefixEntryV6.supporting_prefixes_ref(),
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
      EXPECT_FALSE(waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout)
                       .has_value());
    }

    // Verify 2): PrefixManager's public API
    {
      // verificaiton via public API
      auto mp = getOriginatedPrefixDb();
      auto& prefixEntryV4 = mp.at(v4Prefix_);
      auto& prefixEntryV6 = mp.at(v6Prefix_);

      // v4Prefix - advertised, v6Prefix - withdrawn
      EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                    return *i.installed_ref() == true and
                        i.supporting_prefixes_ref()->size() == 1;
                  }));
      EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                    return *i.installed_ref() == false and
                        i.supporting_prefixes_ref()->size() == 1;
                  }));

      EXPECT_THAT(
          *prefixEntryV4.supporting_prefixes_ref(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v4Network_1)));
      EXPECT_THAT(
          *prefixEntryV6.supporting_prefixes_ref(),
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
      EXPECT_FALSE(waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout)
                       .has_value());
    }

    // Verify 2): PrefixManager -> KvStore update
    {
      // v6Prefix_ is advertised to ALL areas configured
      std::unordered_map<std::string, thrift::PrefixEntry> exp({
          {keyStrAV6_, bestPrefixEntryV6_},
          {keyStrBV6_, bestPrefixEntryV6_},
          {keyStrCV6_, bestPrefixEntryV6_},
      });
      std::unordered_set<std::string> expDeleted{};

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
                    return *i.installed_ref() == true and
                        i.supporting_prefixes_ref()->size() == 1;
                  }));
      EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                    return *i.installed_ref() == true and
                        i.supporting_prefixes_ref()->size() == 2;
                  }));

      EXPECT_THAT(
          *prefixEntryV4.supporting_prefixes_ref(),
          testing::UnorderedElementsAre(
              folly::IPAddress::networkToString(v4Network_1)));
      EXPECT_THAT(
          *prefixEntryV6.supporting_prefixes_ref(),
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
      auto update = waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout);
      EXPECT_TRUE(update.has_value());

      auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
      auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();
      EXPECT_THAT(updatedRoutes, testing::SizeIs(0));
      EXPECT_THAT(deletedRoutes, testing::SizeIs(1));
      EXPECT_THAT(
          deletedRoutes, testing::UnorderedElementsAre(toIpPrefix(v4Prefix_)));
    }

    // Verify 2): PrefixManager -> KvStore update
    {
      // both v4Prefix_ + v6Prefix_ are withdrawn from ALL areas configured
      std::unordered_set<std::string> expDeleted{
          keyStrAV4_,
          keyStrBV4_,
          keyStrCV4_,
          keyStrAV6_,
          keyStrBV6_,
          keyStrCV6_};
      std::unordered_map<std::string, thrift::PrefixEntry> exp{};

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
                    return *i.installed_ref() == false and
                        i.supporting_prefixes_ref()->size() == 0;
                  }));
      EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                    return *i.installed_ref() == false and
                        i.supporting_prefixes_ref()->size() == 1;
                  }));

      // verify attributes
      EXPECT_THAT(
          *prefixEntryV6.supporting_prefixes_ref(),
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
    originatedPrefixV4.prefix_ref() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes_ref() = 0;
    originatedPrefixV4.install_to_fib_ref() = true;

    auto tConfig = PrefixManagerMultiAreaTestFixture::createConfig();
    tConfig.originated_prefixes_ref() = {originatedPrefixV4};
    // Enable v4 over v6 nexthop feature
    tConfig.v4_over_v6_nexthop_ref() = true;
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
  EXPECT_TRUE(tConfig.v4_over_v6_nexthop_ref());
}

TEST_F(RouteOriginationV4OverV6ZeroSupportFixture, StateRouteAnnounce) {
  auto staticRoutesReaderPtr = getEarlyStaticRoutesReaderPtr();

  auto update = waitForRouteUpdate(*staticRoutesReaderPtr, kRouteUpdateTimeout);
  EXPECT_TRUE(update.has_value());

  auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
  auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();
  EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
  EXPECT_THAT(deletedRoutes, testing::SizeIs(0));

  // verify thrift::NextHopThrift struct
  const auto& route = updatedRoutes.back();
  EXPECT_EQ(*route.dest_ref(), toIpPrefix(v4Prefix_));

  const auto& nhs = *route.nextHops_ref();
  EXPECT_THAT(nhs, testing::SizeIs(1));
  // we expect the nexthop is V6
  EXPECT_THAT(
      nhs,
      testing::UnorderedElementsAre(createNextHop(
          toBinaryAddress(Constants::kLocalRouteNexthopV6.toString()))));
}

class RouteOriginationV4OverV6NonZeroSupportFixture
    : public RouteOriginationFixture {
 public:
  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4;
    originatedPrefixV4.prefix_ref() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes_ref() = 2; // 2 support pfxs
    originatedPrefixV4.install_to_fib_ref() = true;

    auto tConfig = PrefixManagerMultiAreaTestFixture::createConfig();
    tConfig.originated_prefixes_ref() = {originatedPrefixV4};
    // Enable v4 over v6 nexthop feature
    tConfig.v4_over_v6_nexthop_ref() = true;
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

  // Supporting prefix number 1
  // Note the v4Prefix is 192.108.0.1/24 :-)
  const std::string v4Prefix_1 = "192.108.0.11/30";
  const auto v4Network_1 = folly::IPAddress::createNetwork(v4Prefix_1);
  const auto prefixEntryV4_1 =
      createPrefixEntry(toIpPrefix(v4Prefix_1), thrift::PrefixType::DEFAULT);
  auto unicastEntryV4_1 = RibUnicastEntry(
      v4Network_1,
      {nh_v6}, // doesn't matter but we are enabling v6 nexthop only :-)
      prefixEntryV4_1,
      thrift::Types_constants::kDefaultArea());
  DecisionRouteUpdate routeUpdate1;
  routeUpdate1.addRouteToUpdate(unicastEntryV4_1);
  fibRouteUpdatesQueue.push(std::move(routeUpdate1));

  auto update = waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout);
  EXPECT_FALSE(update.has_value()); // we need two supports, but only has 1

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
      thrift::Types_constants::kDefaultArea());
  DecisionRouteUpdate routeUpdate2;
  routeUpdate2.addRouteToUpdate(unicastEntryV4_2);
  fibRouteUpdatesQueue.push(std::move(routeUpdate2));

  update = waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout);
  EXPECT_TRUE(update.has_value()); // now we have two ;-)

  auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
  auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();
  EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
  EXPECT_THAT(deletedRoutes, testing::SizeIs(0));

  // verify thrift::NextHopThrift struct
  const auto& route = updatedRoutes.back();
  EXPECT_EQ(*route.dest_ref(), toIpPrefix(v4Prefix_));

  const auto& nhs = *route.nextHops_ref();
  EXPECT_THAT(nhs, testing::SizeIs(1));
  // we expect the nexthop is V6
  EXPECT_THAT(
      nhs,
      testing::UnorderedElementsAre(createNextHop(
          toBinaryAddress(Constants::kLocalRouteNexthopV6.toString()))));
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

class RouteOriginationKnobTestFixture : public PrefixManagerTestFixture {
 protected:
  thrift::OpenrConfig
  createConfig() override {
    auto tConfig = getBasicOpenrConfig(nodeId_);
    tConfig.kvstore_config_ref()->sync_interval_s_ref() = 1;
    tConfig.prefer_openr_originated_routes_ref() = 1;

    return tConfig;
  }
};

// Verify that thrift::PrefixType::CONFIG prefixes (Open/R originated)
// take precedence over thrift::PrefixType::BGP prefixes when they both
// both have the same metrics, and when prefer_openr_originated_routes KNOB
// is turned ON
// Also verify that this KNOB does not interfere with any other
// thrift::PrefixType prefixes, like thrift::PrefixType::LOOPBACK
// This ensures that no other existing functionality has changed
TEST_F(RouteOriginationKnobTestFixture, VerifyKvStoreMultipleClients) {
  /*
   * Order of prefix-entries without config knob:
   *    loopback > bgp > config > default
   *
   * With knob turned on, just BGP and CONFIG get swapped:
   *    loopback > config > bgp > default
   */
  const auto loopback_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::LOOPBACK, createMetrics(200, 0, 0));
  const auto default_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(100, 0, 0));
  const auto bgp_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::BGP, createMetrics(200, 0, 0));
  const auto openr_prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::CONFIG, createMetrics(200, 0, 0));

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
  // 2. Inject prefix1 with client-loopback, default and config - Verify KvStore
  //
  expectedPrefix = loopback_prefix; // lowest client-id will win
  gotExpected = false;
  prefixManager
      ->advertisePrefixes({loopback_prefix, default_prefix, openr_prefix})
      .get();
  baton.wait();
  baton.reset();

  //
  // 3. Withdraw prefix1 with client-loopback - Verify KvStore
  // with loopback gone, BGP will become lowest client_id.
  // Since config KNOB is turned on, and CONFIG is present, CONFIG will win
  //
  expectedPrefix = openr_prefix;
  gotExpected = false;
  prefixManager->withdrawPrefixes({loopback_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 4. Withdraw prefix1 with client-config - Verify KvStore
  //
  expectedPrefix = bgp_prefix;
  gotExpected = false;
  prefixManager->withdrawPrefixes({openr_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 5. Withdraw prefix1 with client-bgp - Verify KvStore
  //
  expectedPrefix = default_prefix;
  gotExpected = false;
  prefixManager->withdrawPrefixes({bgp_prefix}).get();
  baton.wait();
  baton.reset();

  //
  // 6. Withdraw prefix1 with client-default - Verify KvStore
  //
  expectedPrefix = std::nullopt;
  gotExpected = true;
  prefixManager->withdrawPrefixes({default_prefix}).get();
  baton.wait();
  baton.reset();
}

// Verify that the PrefixMgr API getAreaAdvertisedRoutes() returns the
// correct preferred prefixes. Specifically, thrift::PrefixType::CONFIG
// take precedence over thrift::PrefixType::BGP prefixes when they both
// both have the same metrics, and when prefer_openr_originated_routes KNOB
// is turned ON
// Also verify that this API's output does not interfere with any other
// thrift::PrefixType prefixes, like thrift::PrefixType::DEFAULT below.
// This ensures that no other existing functionality has changed.

TEST_F(RouteOriginationKnobTestFixture, VerifyCLIWithMultipleClients) {
  const auto defaultPrefixLower = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(100, 0, 0));
  const auto bgpPrefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::BGP, createMetrics(200, 0, 0));
  const auto openrPrefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::CONFIG, createMetrics(200, 0, 0));
  const auto defaultPrefixHigher = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 0, 0));

  thrift::AdvertisedRouteFilter emptyFilter;
  {
    // With only the BGP prefix, this will be advertised
    prefixManager->advertisePrefixes({bgpPrefix});
    const auto routes = prefixManager
                            ->getAreaAdvertisedRoutes(
                                kTestingAreaName,
                                thrift::RouteFilterType::POSTFILTER_ADVERTISED,
                                emptyFilter)
                            .get();

    EXPECT_EQ(1, routes->size());
    const auto& route = routes->at(0);
    EXPECT_EQ(thrift::PrefixType::BGP, *route.key_ref());
  }
  {
    // Between BGP and CONFIG prefix, CONFIG will be advertised
    prefixManager->advertisePrefixes({openrPrefix});
    const auto routes = prefixManager
                            ->getAreaAdvertisedRoutes(
                                kTestingAreaName,
                                thrift::RouteFilterType::POSTFILTER_ADVERTISED,
                                emptyFilter)
                            .get();

    EXPECT_EQ(1, routes->size());
    const auto& route = routes->at(0);
    EXPECT_EQ(thrift::PrefixType::CONFIG, *route.key_ref());
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
    EXPECT_EQ(thrift::PrefixType::CONFIG, *route.key_ref());
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
    EXPECT_EQ(thrift::PrefixType::DEFAULT, *route.key_ref());
  }
}

class RouteOriginationSingleAreaFixture : public RouteOriginationFixture {
 public:
  openr::thrift::OpenrConfig
  createConfig() override {
    thrift::OriginatedPrefix originatedPrefixV4, originatedPrefixV6;
    originatedPrefixV4.prefix_ref() = v4Prefix_;
    originatedPrefixV4.minimum_supporting_routes_ref() = minSupportingRouteV4_;
    originatedPrefixV4.install_to_fib_ref() = true;
    originatedPrefixV6.prefix_ref() = v6Prefix_;
    originatedPrefixV6.minimum_supporting_routes_ref() = minSupportingRouteV6_;
    originatedPrefixV6.install_to_fib_ref() = false;

    // create a single area config
    auto A = createAreaConfig("A", {"RSW.*"}, {".*"});
    auto tConfig = getBasicOpenrConfig(nodeId_, "domain", {A});
    tConfig.kvstore_config_ref()->sync_interval_s_ref() = 1;
    tConfig.originated_prefixes_ref() = {
        originatedPrefixV4, originatedPrefixV6};
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
  nh_3.area_ref().reset(); // empty area

  // supporting V4 prefix and related structs
  const std::string v4Prefix_1 = "192.108.0.8/30";
  const auto v4Network_1 = folly::IPAddress::createNetwork(v4Prefix_1);
  const auto prefixEntryV4_1 =
      createPrefixEntry(toIpPrefix(v4Prefix_1), thrift::PrefixType::DEFAULT);
  auto unicastEntryV4_1 = RibUnicastEntry(
      v4Network_1,
      {nh_v4},
      prefixEntryV4_1,
      thrift::Types_constants::kDefaultArea());

  // supporting V6 prefix #1 and related RIB structs
  const std::string v6Prefix_1 = "2001:1:2:3::1/70";
  const auto v6Network_1 = folly::IPAddress::createNetwork(v6Prefix_1);
  const auto prefixEntryV6_1 =
      createPrefixEntry(toIpPrefix(v6Prefix_1), thrift::PrefixType::DEFAULT);
  auto unicastEntryV6_1 = RibUnicastEntry(
      v6Network_1,
      {nh_v6},
      prefixEntryV6_1,
      thrift::Types_constants::kDefaultArea());

  // supporting V6 prefix #2 and related RIB structs
  const std::string v6Prefix_2 = "2001:1:2:3::1/120";
  const auto v6Network_2 = folly::IPAddress::createNetwork(v6Prefix_2);
  const auto prefixEntryV6_2 =
      createPrefixEntry(toIpPrefix(v6Prefix_2), thrift::PrefixType::RIB);
  auto unicastEntryV6_2 = RibUnicastEntry(
      v6Network_2,
      {nh_v6, nh_3},
      prefixEntryV6_2,
      thrift::Types_constants::kDefaultArea());

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
  //    a. v4Prefix_ will be sent to Decision on staticRouteUpdatesQueue (since
  //       the install_to_fib bit is set for v4Prefix_)
  //    b. v6Prefix_ will NOT be sent to Decision on staticRouteUpdatesQueue
  //       (since min_supporting_route is not met for v6Prefix_, plus the
  //       install_to_fib bit is NOT set for v6Prefix_)
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
  routeUpdate.addRouteToUpdate(unicastEntryV4_1);
  routeUpdate.addRouteToUpdate(unicastEntryV6_1);
  fibRouteUpdatesQueue.push(std::move(routeUpdate));

  // Verify 1a and 1b: PrefixManager -> Decision staticRouteupdate
  {
    auto update = waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout);
    // 1a. v4 route update received
    EXPECT_TRUE(update.has_value());

    auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
    auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();
    EXPECT_THAT(updatedRoutes, testing::SizeIs(1));
    EXPECT_THAT(deletedRoutes, testing::SizeIs(0));

    // verify thrift::NextHopThrift struct
    const auto& route = updatedRoutes.back();
    EXPECT_EQ(*route.dest_ref(), toIpPrefix(v4Prefix_));

    const auto& nhs = *route.nextHops_ref();
    EXPECT_THAT(nhs, testing::SizeIs(1));
    EXPECT_THAT(
        nhs,
        testing::UnorderedElementsAre(createNextHop(
            toBinaryAddress(Constants::kLocalRouteNexthopV4.toString()))));

    // 1b. no v6 route update received
    EXPECT_FALSE(waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout)
                     .has_value());
  }

  // Verify 1c and 1d: PrefixManager -> KvStore update
  {
    // v4Prefix_ is advertised to ALL areas configured, while v6Prefix_ is NOT
    std::unordered_map<std::string, thrift::PrefixEntry> exp({
        {keyStrAV4_, bestPrefixEntryV4_},
    });
    std::unordered_set<std::string> expDeleted{};

    // wait for condition to be met for KvStore publication
    waitForKvStorePublication(kvStoreUpdatesReader, exp, expDeleted);
  }

  // Verify 1e: Via PrefixManager's public API, verify the values for # of
  //  supporting routes for both the v4Prefix_ (1) and v6Orefix_(1)
  {
    auto mp = getOriginatedPrefixDb();
    auto& prefixEntryV4 = mp.at(v4Prefix_);
    auto& prefixEntryV6 = mp.at(v6Prefix_);

    // v4Prefix - advertised, v6Prefix - NOT advertised
    EXPECT_THAT(prefixEntryV4, testing::Truly([&](auto i) {
                  return *i.installed_ref() == true and
                      i.supporting_prefixes_ref()->size() == 1;
                }));
    EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                  return *i.installed_ref() == false and
                      i.supporting_prefixes_ref()->size() == 1;
                }));

    EXPECT_THAT(
        *prefixEntryV4.supporting_prefixes_ref(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v4Network_1)));
    EXPECT_THAT(
        *prefixEntryV6.supporting_prefixes_ref(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v6Network_1)));
  }

  // Step 2 - inject 1 v6 supporting prefix into fibRouteUpdatesQueue
  routeUpdate.addRouteToUpdate(unicastEntryV6_2);
  fibRouteUpdatesQueue.push(std::move(routeUpdate));

  // Verify 2a: PrefixManager -> Decision staticRouteUpdate
  {
    // 2a. NO v6 route update received
    auto update = waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout);
    EXPECT_FALSE(update.has_value());
  }

  // Verify 2b: PrefixManager -> KvStore update
  {
    // v6Prefix_ is advertised to the SINGLE area configured
    std::unordered_map<std::string, thrift::PrefixEntry> exp({
        {keyStrAV6_, bestPrefixEntryV6_},
    });
    std::unordered_set<std::string> expDeleted{};

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
                  return *i.installed_ref() == true and
                      i.supporting_prefixes_ref()->size() == 1;
                }));
    EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                  return *i.installed_ref() == true and
                      i.supporting_prefixes_ref()->size() == 2;
                }));

    EXPECT_THAT(
        *prefixEntryV4.supporting_prefixes_ref(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v4Network_1)));
    EXPECT_THAT(
        *prefixEntryV6.supporting_prefixes_ref(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v6Network_1),
            folly::IPAddress::networkToString(v6Network_2)));
  }

  // Step 3 - withdraw 1 v4 and 1 v6 supporting prefix
  routeUpdate.unicastRoutesToDelete.emplace_back(v4Network_1);
  routeUpdate.unicastRoutesToDelete.emplace_back(v6Network_1);
  fibRouteUpdatesQueue.push(std::move(routeUpdate));

  // Verify 3a PrefixManager -> Decision staticRouteupdate
  {
    // ONLY v4 route withdrawn updates are sent to Decision
    auto update = waitForRouteUpdate(staticRoutesReader, kRouteUpdateTimeout);
    EXPECT_TRUE(update.has_value());

    auto updatedRoutes = *update.value().unicastRoutesToUpdate_ref();
    auto deletedRoutes = *update.value().unicastRoutesToDelete_ref();
    EXPECT_THAT(updatedRoutes, testing::SizeIs(0));
    EXPECT_THAT(deletedRoutes, testing::SizeIs(1));
    EXPECT_THAT(
        deletedRoutes, testing::UnorderedElementsAre(toIpPrefix(v4Prefix_)));
  }

  // Verify 3b: PrefixManager -> KvStore update: both prefixes withdrawn
  {
    // both v4Prefix_ + v6Prefix_ are withdrawn from the single area configured
    std::unordered_set<std::string> expDeleted{
        keyStrAV4_,
        keyStrAV6_,
    };
    std::unordered_map<std::string, thrift::PrefixEntry> exp{};

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
                  return *i.installed_ref() == false and
                      i.supporting_prefixes_ref()->size() == 0;
                }));
    EXPECT_THAT(prefixEntryV6, testing::Truly([&](auto i) {
                  return *i.installed_ref() == false and
                      i.supporting_prefixes_ref()->size() == 1;
                }));

    EXPECT_THAT(
        *prefixEntryV6.supporting_prefixes_ref(),
        testing::UnorderedElementsAre(
            folly::IPAddress::networkToString(v6Network_2)));
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);

  // Run the tests
  return RUN_ALL_TESTS();
}
