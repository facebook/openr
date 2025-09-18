/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <gtest/gtest.h>

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;

namespace {

// TTL in ms
const uint64_t kShortTtl{2000}; // 2 seconds
const uint64_t kLongTtl{300000}; // 5 minutes
const uint32_t kSelfAdjTimeout{1000}; // 1 second

/**
 * Fixture for abstracting out common functionality for unittests.
 */
class KvStoreSelfOriginatedKeyValueRequestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // nothing to do
  }

  void
  TearDown() override {
    // close queue before stopping kvstore so fiber task can end
    kvRequestQueue_.close();
    kvStore_->stop();
  }

  /**
   * Helper function to create KvStoreWrapper and start the KvStore.
   * KvStore uses request queue to receive key-value updates from clients.
   */
  void
  initKvStore(
      std::string nodeId,
      uint32_t keyTtl = kLongTtl,
      uint32_t selfAdjTimeout = 0) {
    // create KvStoreConfig
    thrift::KvStoreConfig kvStoreConfig;
    const std::unordered_set<std::string> areaIds{kTestingAreaName};

    // Override ttl for kvstore self-originated keys
    kvStoreConfig.node_name() = nodeId;
    kvStoreConfig.key_ttl_ms() = keyTtl;
    if (selfAdjTimeout) {
      kvStoreConfig.self_adjacency_timeout_ms() = selfAdjTimeout;
    }

    // start kvstore
    kvStore_ =
        std::make_unique<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            areaIds,
            kvStoreConfig,
            std::nullopt /* peerUpdatesQueue */,
            kvRequestQueue_.getReader());
    kvStore_->run();
  }

 protected:
  std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>> kvStore_;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
};
} // namespace

/**
 * Validate SetKeyValueRequest processing, key-setting, and ttl-refreshing.
 * Send SetKeyValueRequest via queue to KvStore.
 */
TEST_F(KvStoreSelfOriginatedKeyValueRequestFixture, ProcessSetKeyValueRequest) {
  // create and start kv store with kvRequestQueue enabled
  const std::string nodeId = "node21";
  initKvStore(nodeId, kShortTtl);

  // create request to set key-val
  const std::string key = "key1";
  const std::string value = "value1";
  auto setKvRequest = SetKeyValueRequest(kTestingAreaName, key, value);

  // push request to queue -- trigger processKeyValueRequest() in KvStore
  kvRequestQueue_.push(std::move(setKvRequest));

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    // wait for kvstore handle SetKeyValue request and flood new key-val
    auto pub = kvStore_->recvPublication();
    EXPECT_EQ(1, pub.keyVals()->size());
    EXPECT_EQ(0, *(pub.keyVals()->at(key).ttlVersion()));

    // check advertised self-originated key-value was stored in kvstore
    auto kvStoreCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
    EXPECT_EQ(1, kvStoreCache.size());
    EXPECT_EQ(value, *kvStoreCache.at(key).value.value());

    // check that ttl version was bumped up and
    // key-val with updated ttl version was flooded
    auto pub2 = kvStore_->recvPublication();
    EXPECT_EQ(1, pub2.keyVals()->size());
    EXPECT_EQ(1, *(pub2.keyVals()->at(key).ttlVersion()));
  });

  // check that key-val was not expired after ttl time has passed
  evb.scheduleTimeout(std::chrono::milliseconds(kShortTtl * 2), [&]() noexcept {
    auto recVal = kvStore_->getKey(kTestingAreaName, key);
    EXPECT_TRUE(recVal.has_value());
    EXPECT_GE(*(recVal.value().ttlVersion()), 4);
    evb.stop();
  });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/**
 * Validate versioning for receiving multiple SetKeyValueRequests.
 */
TEST_F(KvStoreSelfOriginatedKeyValueRequestFixture, SetKeyTwice) {
  // create and start kv store with kvRequestQueue enabled
  const std::string nodeId = "black-widow";
  initKvStore(nodeId, kShortTtl);

  // Push first request to set key-val.
  const std::string key = "key-settwice";
  const std::string valueFirst = "value-first";
  auto setFirstKvRequest =
      SetKeyValueRequest(kTestingAreaName, key, valueFirst);
  kvRequestQueue_.push(std::move(setFirstKvRequest));

  // Key is new to KvStore. Value version should be 1.
  auto pubFirst = kvStore_->recvPublication();
  EXPECT_EQ(1, pubFirst.keyVals()->size());
  EXPECT_EQ(1, *pubFirst.keyVals()->at(key).version());
  EXPECT_EQ(valueFirst, *pubFirst.keyVals()->at(key).value());

  // Create request to set same key, different value.
  const std::string valueSecond = "value-second";
  auto setSecondKvRequest =
      SetKeyValueRequest(kTestingAreaName, key, valueSecond);
  kvRequestQueue_.push(std::move(setSecondKvRequest));

  // Check value is updated and version is bumped.
  auto pubSecond = kvStore_->recvPublication();
  EXPECT_EQ(1, pubSecond.keyVals()->size());
  EXPECT_EQ(2, *pubSecond.keyVals()->at(key).version());
  EXPECT_EQ(valueSecond, *pubSecond.keyVals()->at(key).value());

  // Validate that advertised self-originated key-value matches KvStore cache.
  auto kvStoreCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
  EXPECT_EQ(1, kvStoreCache.size());
  EXPECT_EQ(2, *kvStoreCache.at(key).value.version());
  EXPECT_EQ(valueSecond, *kvStoreCache.at(key).value.value());
}

/**
 * Validate versioning for receiving multiple SetKeyValueRequests.
 */
TEST_F(KvStoreSelfOriginatedKeyValueRequestFixture, SetKeyVersion) {
  // clean up counters before testing
  fb303::fbData->resetAllData();

  // create and start kv store with kvRequestQueue enabled
  const std::string nodeId = "yelena";
  initKvStore(nodeId, kShortTtl);

  // Push first request to set key-val.
  const std::string key = "key-red-guardian";
  const std::string value = "value-red-guardian";
  const uint32_t version = 10;
  auto setKvRequest = SetKeyValueRequest(kTestingAreaName, key, value, version);
  kvRequestQueue_.push(std::move(setKvRequest));

  // Key is new to KvStore. Value version should be 1.
  auto pubFirst = kvStore_->recvPublication();
  EXPECT_EQ(1, pubFirst.keyVals()->size());
  EXPECT_EQ(version, *pubFirst.keyVals()->at(key).version());

  // Validate that advertised self-originated key-value matches KvStore cache.
  auto kvStoreCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
  EXPECT_EQ(1, kvStoreCache.size());
  EXPECT_EQ(version, *kvStoreCache.at(key).value.version());

  // Make sure self-originated keys will NOT increase counter
  auto counters = fb303::fbData->getCounters();
  ASSERT_TRUE(counters.contains("kvstore.received_publications.count"));
  EXPECT_EQ(0, counters.at("kvstore.received_publications.count"));
}

/**
 * Validate PersistKeyValueRequest advertisement, version overriding, and
 * ttl-refreshing. Send PersistKeyValueRequest via queue to KvStore.
 */
TEST_F(
    KvStoreSelfOriginatedKeyValueRequestFixture,
    ProcessPersistKeyValueRequest) {
  // create and start kv store with kvRequestQueue enabled
  const std::string nodeId = "node22";
  initKvStore(nodeId, kShortTtl);

  const std::string key = "persist-key";
  const std::string value = "persist-value";
  const std::string newValue = "persist-changedvalue";

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    // Test 1: - persist key (first-time persisting)
    //         - check key-value advertisement
    //         - check ttl version update advertisement
    {
      EXPECT_TRUE(kvStore_->checkInitialSelfOriginatedKeysTimerScheduled());

      // create request to persist key-val
      auto persistKvRequest =
          PersistKeyValueRequest(kTestingAreaName, key, value);
      // push request to queue -- trigger processKeyValueRequest() in KvStore
      kvRequestQueue_.push(std::move(persistKvRequest));

      // wait for kvstore handle PersistKeyValue request and flood new key-val
      auto pub = kvStore_->recvPublication();
      EXPECT_EQ(1, pub.keyVals()->size());
      EXPECT_EQ(0, *(pub.keyVals()->at(key).ttlVersion()));
      EXPECT_EQ(1, *(pub.keyVals()->at(key).version()));
      EXPECT_FALSE(kvStore_->checkInitialSelfOriginatedKeysTimerScheduled());
      kvStore_->recvSelfAdjSyncedSignal();

      // check advertised self-originated key-value was stored in kvstore
      auto kvStoreCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
      EXPECT_EQ(1, kvStoreCache.size());
      EXPECT_EQ(value, *kvStoreCache.at(key).value.value());

      // check that ttl version was bumped up and
      // key-val with updated ttl version was flooded
      auto pub2 = kvStore_->recvPublication();
      EXPECT_EQ(1, pub2.keyVals()->size());
      EXPECT_EQ(1, *(pub2.keyVals()->at(key).ttlVersion()));
    }

    // Test 2: - persist same key with a different value
    //         - check that new key was advertised
    //         - check that version was bumped up
    {
      // persist key with new value
      auto persistSameKeyRequest =
          PersistKeyValueRequest(kTestingAreaName, key, newValue);
      kvRequestQueue_.push(std::move(persistSameKeyRequest));

      // new key-value will be advertised and version will be bumped up
      auto newValPub = kvStore_->recvPublication();
      EXPECT_EQ(1, newValPub.keyVals()->size());
      EXPECT_EQ(0, *(newValPub.keyVals()->at(key).ttlVersion()));
      EXPECT_EQ(2, *(newValPub.keyVals()->at(key).version()));

      // check cache of self-originated key-vals has stored the new value
      auto updatedCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
      EXPECT_EQ(1, updatedCache.size());
      EXPECT_EQ(newValue, *updatedCache.at(key).value.value());
    }

    // Test 3: - persist same key with same value
    //         - check that version is the same and ttl version
    //           continues being bumped
    {
      // persist key with same value
      auto persistSameValueRequest =
          PersistKeyValueRequest(kTestingAreaName, key, newValue);
      kvRequestQueue_.push(std::move(persistSameValueRequest));
    }
  });

  evb.scheduleTimeout(std::chrono::milliseconds(kShortTtl * 2), [&]() noexcept {
    // Check that key-val was not expired after ttl time has passed.
    auto recVal = kvStore_->getKey(kTestingAreaName, key);
    EXPECT_TRUE(recVal.has_value());

    // Test 3 (continued):
    // Check that version is the same and ttl version has been bumped.
    EXPECT_EQ(2, *(recVal.value().version()));
    EXPECT_GE(*(recVal.value().ttlVersion()), 4);
    evb.stop();
  });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/*
 * Verify KvStoreDb will override self-originated key version when received
 * KvStore publication. Make sure key-override happens and re-advertise higher
 * version of key-vals.
 */
TEST_F(
    KvStoreSelfOriginatedKeyValueRequestFixture,
    PersistKeyWithVersionOverriding) {
  // ATTN: nodId/diffNodeId and val/diffVal has the lexicographical order.
  const std::string nodeId{"A"};
  const std::string diffNodeId{"Z"};
  const std::string key{"key"};
  const std::string val{"a"};
  const std::string diffVal{"z"};

  initKvStore(nodeId, kShortTtl);

  //
  // Test 1: test currVersion < rcvdVersion. Key overriding will happen.
  //
  {
    //
    // Step1: - persist key X;
    //        - expect key version to be 1 as it KvStore is empty before
    //
    auto persistKvRequest = PersistKeyValueRequest(kTestingAreaName, key, val);
    kvRequestQueue_.push(std::move(persistKvRequest));

    auto pub1 = kvStore_->recvPublication();
    EXPECT_EQ(1, pub1.keyVals()->size());
    kvStore_->recvSelfAdjSyncedSignal();

    // validate version = 1
    const auto tVal1 = pub1.keyVals()->at(key);
    EXPECT_EQ(1, *tVal1.version());
    EXPECT_EQ(nodeId, *tVal1.originatorId());

    //
    // Step2: - manually set key X with existing version + 1 to mimick receiving
    //          publication(e.g. via FULL_SYNC);
    //        - expect KvStore re-advertise version + 2 to override;
    //
    kvStore_->setKey(
        kTestingAreaName,
        key,
        createThriftValue(
            *tVal1.version() + 1 /* version */,
            nodeId /* originatorId */,
            val /* value */,
            Constants::kTtlInfinity /* ttl */));

    // validate setKey() takes effect
    auto pub2 = kvStore_->recvPublication();
    const auto tVal2 = pub2.keyVals()->at(key);
    EXPECT_EQ(*tVal1.version() + 1, *tVal2.version());
    EXPECT_EQ(nodeId, *tVal2.originatorId());

    // validate version is overridden
    auto pub3 = kvStore_->recvPublication();
    const auto tVal3 = pub3.keyVals()->at(key);
    EXPECT_EQ(*tVal1.version() + 2, *tVal3.version());
    EXPECT_EQ(nodeId, *tVal3.originatorId());
  }

  //
  // Test 2: test currVersion > rcvdVersion. Ignore.
  //
  {
    kvStore_->setKey(
        kTestingAreaName,
        key,
        createThriftValue(
            1 /* version */,
            nodeId /* originatorId */,
            diffVal /* value */,
            Constants::kTtlInfinity /* ttl */));
    // Ensure key exists
    auto maybeVal = kvStore_->getKey(kTestingAreaName, key);
    ASSERT_TRUE(maybeVal.has_value());
    EXPECT_NE(1, *maybeVal->version());
    EXPECT_EQ(val, maybeVal->value());
  }

  //
  // Test 3: test currVersion == rcvdVersion.
  //
  {
    //
    // Step1: - manually set key X with SAME version to mimick receiving
    //          publication(e.g. via FULL_SYNC);
    //        - explicitly set different VALUE with same ORIGINATOR_ID.
    //          Expect KvStore re-advertises version + 1 to override;
    //
    auto maybeVal = kvStore_->getKey(kTestingAreaName, key);
    ASSERT_TRUE(maybeVal.has_value());
    const auto version = *maybeVal->version();
    kvStore_->setKey(
        kTestingAreaName,
        key,
        createThriftValue(
            version /* version */,
            nodeId /* originatorId */,
            diffVal /* value */,
            Constants::kTtlInfinity /* ttl */));

    // validate setKey() takes effect
    auto pub1 = kvStore_->recvPublication();
    const auto tVal1 = pub1.keyVals()->at(key);
    EXPECT_EQ(version, *tVal1.version());
    EXPECT_EQ(diffVal, *tVal1.value());
    EXPECT_EQ(nodeId, *tVal1.originatorId());

    // validate version is overridden
    auto pub2 = kvStore_->recvPublication();
    const auto tVal2 = pub2.keyVals()->at(key);
    EXPECT_EQ(version + 1, *tVal2.version());
    EXPECT_EQ(val, *tVal2.value());
    EXPECT_EQ(nodeId, *tVal2.originatorId());

    //
    // Step2: - manually set key X with SAME version to mimick receiving
    //          publication(e.g. via FULL_SYNC);
    //        - explicitly set different ORIGINATOR_ID. Expect KvStore
    //          re-advertises version + 1 to override;
    //
    kvStore_->setKey(
        kTestingAreaName,
        key,
        createThriftValue(
            version + 1 /* version */,
            diffNodeId /* originatorId */,
            val /* value */,
            Constants::kTtlInfinity /* ttl */));

    // validate setKey() takes effect
    auto pub3 = kvStore_->recvPublication();
    const auto tVal3 = pub3.keyVals()->at(key);
    EXPECT_EQ(version + 1, *tVal3.version());
    EXPECT_EQ(val, *tVal3.value());
    EXPECT_EQ(diffNodeId, *tVal3.originatorId());

    // validate version is overridden
    auto pub4 = kvStore_->recvPublication();
    const auto tVal4 = pub4.keyVals()->at(key);
    EXPECT_EQ(version + 2, *tVal4.version());
    EXPECT_EQ(val, *tVal4.value());
    EXPECT_EQ(nodeId, *tVal4.originatorId());
  }
}

/**
 * Validate PersistKeyValueRequest version overriding of self-originated key-val
 * if another originator has advertised same key.
 */
TEST_F(
    KvStoreSelfOriginatedKeyValueRequestFixture,
    PersistKeyWithValueOverriding) {
  const std::string otherNodeId = "node-other";
  const std::string myNodeId = "node-myself";
  initKvStore(myNodeId);

  const std::string key = "persist-override-key";
  const std::string value = "value-to-override";

  // Set a key in KvStore with other node as originator.
  const thrift::Value thriftVal = createThriftValue(
      1 /* version */,
      otherNodeId /* originatorId */,
      value /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(
          1, otherNodeId, thrift::Value().value() = std::string(value)));
  kvStore_->setKey(kTestingAreaName, key, thriftVal);

  // First publication is from flooding the set key. Check that originator is
  // other node.
  auto setPub = kvStore_->recvPublication();
  EXPECT_EQ(1, setPub.keyVals()->size());
  EXPECT_EQ(0, *(setPub.keyVals()->at(key).ttlVersion()));
  EXPECT_EQ(1, *(setPub.keyVals()->at(key).version()));
  EXPECT_EQ(otherNodeId, *(setPub.keyVals()->at(key).originatorId()));

  // Persist key-val using request queue.
  auto persistKvRequest = PersistKeyValueRequest(kTestingAreaName, key, value);
  kvRequestQueue_.push(std::move(persistKvRequest));

  // Second publication is from flooding the persist key originator change.
  // Check that key-val has been overridden and originator has changed to my
  // node.
  auto persistPub = kvStore_->recvPublication();
  kvStore_->recvSelfAdjSyncedSignal();
  EXPECT_EQ(1, persistPub.keyVals()->size());
  EXPECT_EQ(2, *(persistPub.keyVals()->at(key).version()));
  EXPECT_EQ(myNodeId, *(persistPub.keyVals()->at(key).originatorId()));
  // TTL refresh could happen before persist publication.
  auto ttlVersion = *(persistPub.keyVals()->at(key).ttlVersion());
  EXPECT_TRUE(0 == ttlVersion || 1 == ttlVersion);
}

/**
 * Test ttl change with persist key while keeping value and version same
 * - Set key with kShortTtl: 2s
 *   - Verify key is set and remains
 *   - Verify "0s < ttl <= 2s"
 * - Update key with kLongTtl: 300s
 *   - Verify key remains after key persistence
 *   - Verify "2s < ttl <= 300s"
 */
TEST_F(KvStoreSelfOriginatedKeyValueRequestFixture, PersistKeyChangeTtlTest) {
  const std::string nodeId = "test-nodeId";
  const std::string key{"test-key"};
  const std::string val{"test-value"};
  int scheduleAt{0};

  // Create and initialize KvStore
  initKvStore(nodeId, kLongTtl);

  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // Set a key in KvStore
        const thrift::Value thriftVal = createThriftValue(
            1 /* version */,
            nodeId /* originatorId */,
            val /* value */,
            kShortTtl /* ttl = 5s */);
        kvStore_->setKey(kTestingAreaName, key, thriftVal);
      });

  // Verify key exists after 200ms given 100ms throttling time
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 200), [&]() noexcept {
        // Ensure key exists
        auto maybeVal = kvStore_->getKey(kTestingAreaName, key);
        ASSERT_TRUE(maybeVal.has_value());
        EXPECT_EQ(1, *maybeVal->version());
        EXPECT_EQ(val, maybeVal->value());
        EXPECT_LT(0, *maybeVal->ttl());
        EXPECT_GE(kShortTtl, *maybeVal->ttl());

        // Set key with smaller kShortTtl=5s(default with KvStore)
        auto persistKvRequest =
            PersistKeyValueRequest(kTestingAreaName, key, val);
        kvRequestQueue_.push(std::move(persistKvRequest));
      });

  // Verify key exists after 200ms given 100ms throttling time
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 200), [&]() noexcept {
        // Ensure key exists
        auto maybeVal = kvStore_->getKey(kTestingAreaName, key);
        ASSERT_TRUE(maybeVal.has_value());
        EXPECT_EQ(1, *maybeVal->version());
        EXPECT_EQ(val, maybeVal->value());
        EXPECT_LT(kShortTtl, *maybeVal->ttl());
        EXPECT_GE(kLongTtl, *maybeVal->ttl());

        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/**
 * Validate ClearKeyValue request to erase a key. Removes key-val from self
 * originated cache and stops ttl refreshing of erased key-val.
 *
 */
TEST_F(KvStoreSelfOriginatedKeyValueRequestFixture, EraseKeyValue) {
  // Create and start kv store with kvRequestQueue enabled with 2 second ttl.
  const std::string nodeId = "node-testerasekey";
  initKvStore(nodeId, kShortTtl);

  const std::string eraseKey = "erase-key";
  const std::string eraseValue = "erase-value";
  const std::string setKey = "set-key";
  const std::string setValue = "set-value";

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    /** Set 2 key-vals. Check that they are set correctly. **/

    // Push SetKeyValue request for "erase-key" key to queue.
    auto setKvRequestToErase =
        SetKeyValueRequest(kTestingAreaName, eraseKey, eraseValue);
    kvRequestQueue_.push(std::move(setKvRequestToErase));

    // Receive publication for "erase-key" key.
    auto pubSetKey = kvStore_->recvPublication();
    EXPECT_EQ(1, pubSetKey.keyVals()->size());
    EXPECT_EQ(0, *(pubSetKey.keyVals()->at(eraseKey).ttlVersion()));
    EXPECT_EQ(1, *(pubSetKey.keyVals()->at(eraseKey).version()));

    // Push SetKeyValue request for "set-key" key to queue.
    auto setKvRequest = SetKeyValueRequest(kTestingAreaName, setKey, setValue);
    kvRequestQueue_.push(std::move(setKvRequest));

    // Receive publication for "set-key" key.
    auto pubSetKey2 = kvStore_->recvPublication();
    EXPECT_EQ(1, pubSetKey2.keyVals()->size());
    EXPECT_EQ(0, *(pubSetKey2.keyVals()->at(setKey).ttlVersion()));
    EXPECT_EQ(1, *(pubSetKey2.keyVals()->at(setKey).version()));

    // Make sure "erase-key" key is in self-originated cache.
    auto kvStoreCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
    EXPECT_EQ(2, kvStoreCache.size());
    EXPECT_TRUE(kvStoreCache.contains(eraseKey));
    EXPECT_EQ(eraseValue, *kvStoreCache.at(eraseKey).value.value());

    /** Erase one key. Check that erased key does NOT emit ttl updates. **/

    // Push EraseKeyValue request
    auto eraseKvRequest = ClearKeyValueRequest(kTestingAreaName, eraseKey);
    kvRequestQueue_.push(std::move(eraseKvRequest));

    // Receive ttl refresh publication for "set-key" (ttl version 1).
    auto pubSetKeyTtlUpdate = kvStore_->recvPublication();
    EXPECT_EQ(1, pubSetKeyTtlUpdate.keyVals()->size());
    EXPECT_EQ(1, *(pubSetKeyTtlUpdate.keyVals()->at(setKey).ttlVersion()));
    EXPECT_EQ(1, *(pubSetKeyTtlUpdate.keyVals()->at(setKey).version()));

    // Erased key is still in KvStore but not in self-originated cache.
    auto recVal = kvStore_->getKey(kTestingAreaName, eraseKey);
    EXPECT_TRUE(recVal.has_value());

    auto updatedCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
    EXPECT_EQ(1, updatedCache.size());
    EXPECT_FALSE(updatedCache.contains(eraseKey));

    // Receive ttl refresh publication for "set-key" (ttl version 2).
    auto pubSetKeyTtlUpdate2 = kvStore_->recvPublication();
    EXPECT_EQ(1, pubSetKeyTtlUpdate2.keyVals()->size());
    EXPECT_EQ(2, *(pubSetKeyTtlUpdate2.keyVals()->at(setKey).ttlVersion()));
    EXPECT_EQ(1, *(pubSetKeyTtlUpdate2.keyVals()->at(setKey).version()));
  });

  // After ttl time, erased key should expire.
  evb.scheduleTimeout(std::chrono::milliseconds(kShortTtl * 2), [&]() noexcept {
    auto recVal = kvStore_->getKey(kTestingAreaName, eraseKey);
    EXPECT_FALSE(recVal.has_value());
    evb.stop();
  });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/**
 * Validate ClearKeyValue request that unsets the key. Floods a new value,
 * removes the key-val from the self-originated cache, and stops ttl refreshing
 * of unset key-val.
 */
TEST_F(KvStoreSelfOriginatedKeyValueRequestFixture, UnsetKeyValue) {
  // Create and start kv store with kvRequestQueue enabled with 2 second ttl.
  const std::string nodeId = "node-testunsetkey";
  initKvStore(nodeId, kShortTtl);

  const std::string unsetKey = "unset-key";
  const std::string valueBeforeUnset = "value-before-unset";
  const std::string valueAfterUnset = "value-after-unset";
  const std::string setKey = "set-key";
  const std::string setValue = "set-value";

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    /** Set 2 key-vals. Check that they are set correctly. **/

    // Push SetKeyValue request for "unset-key" key to queue. Check key was set
    // correctly.
    auto setKvRequestToUnset =
        SetKeyValueRequest(kTestingAreaName, unsetKey, valueBeforeUnset);
    kvRequestQueue_.push(std::move(setKvRequestToUnset));
    auto pubSetKey = kvStore_->recvPublication();
    EXPECT_EQ(1, pubSetKey.keyVals()->contains(unsetKey));

    // Push SetKeyValue request for "set-key" key to queue. Check key was set
    // correctly.
    auto setKvRequest = SetKeyValueRequest(kTestingAreaName, setKey, setValue);
    kvRequestQueue_.push(std::move(setKvRequest));
    auto pubSetKey2 = kvStore_->recvPublication();
    EXPECT_EQ(1, pubSetKey2.keyVals()->contains(setKey));

    // Make sure "unset-key" key is in self-originated cache.
    auto kvStoreCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
    EXPECT_TRUE(kvStoreCache.contains(unsetKey));
    EXPECT_EQ(valueBeforeUnset, *kvStoreCache.at(unsetKey).value.value());

    /** Unset one key. Check that unset key does NOT emit ttl updates. **/

    // Push ClearKeyValue request to unset the key
    auto unsetKvRequest =
        ClearKeyValueRequest(kTestingAreaName, unsetKey, valueAfterUnset, true);
    kvRequestQueue_.push(std::move(unsetKvRequest));

    // Receive publication for new value set to "unset-key". Version should be
    // bumped up and ttl version should be reset.
    auto pubUnsetKey = kvStore_->recvPublication();
    EXPECT_EQ(1, pubUnsetKey.keyVals()->contains(unsetKey));
    EXPECT_EQ(2, *pubUnsetKey.keyVals()->at(unsetKey).version());
    EXPECT_EQ(0, *pubUnsetKey.keyVals()->at(unsetKey).ttlVersion());
    EXPECT_EQ(valueAfterUnset, *pubUnsetKey.keyVals()->at(unsetKey).value());

    // "unset-key" should still be in KvStore with new value but NOT in
    // self-originated cache.
    auto recVal = kvStore_->getKey(kTestingAreaName, unsetKey);
    auto updatedCache = kvStore_->dumpAllSelfOriginated(kTestingAreaName);
    EXPECT_TRUE(recVal.has_value());
    EXPECT_EQ(valueAfterUnset, *recVal.value().value());
    EXPECT_EQ(1, updatedCache.size());
    EXPECT_FALSE(updatedCache.contains(unsetKey));

    // Receive ttl refresh publication for "set-key" (version 1).
    auto pubSetKeyTtlUpdate = kvStore_->recvPublication();
    EXPECT_EQ(1, pubSetKeyTtlUpdate.keyVals()->contains(setKey));
    EXPECT_EQ(1, *(pubSetKeyTtlUpdate.keyVals()->at(setKey).ttlVersion()));

    // Receive ttl refresh publication for "set-key" (version 2).
    auto pubSetKeyTtlUpdate2 = kvStore_->recvPublication();
    EXPECT_EQ(1, pubSetKeyTtlUpdate2.keyVals()->contains(setKey));
    EXPECT_EQ(2, *(pubSetKeyTtlUpdate2.keyVals()->at(setKey).ttlVersion()));
  });

  // After ttl time, unset key should expire.
  evb.scheduleTimeout(std::chrono::milliseconds(kShortTtl * 2), [&]() noexcept {
    auto recVal = kvStore_->getKey(kTestingAreaName, unsetKey);
    EXPECT_FALSE(recVal.has_value());
    evb.stop();
  });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/**
 * Validate throttling that batches together requests to persist and unset keys
 * to avoid unnecessary changes to the KvStore's key-vals. Verify that
 * chronology of incoming requests is maintained with throttling.
 */
TEST_F(
    KvStoreSelfOriginatedKeyValueRequestFixture, PersistKeyUnsetKeyThrottle) {
  const std::string nodeId{"throttle-node"};
  const std::string key{"k1"};
  const std::string val{"v1"};
  const std::string deleteVal{"delete-v1"};
  int scheduleAt{0};
  OpenrEventBase evb;

  initKvStore(nodeId, kShortTtl);

  //
  // Test1: - persist key X;
  //        - unset key X;
  //        - expect key X is NOT received by `KvStore` at all
  //
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // persistKey will throttle the request and hold for 100ms
        auto persistKvRequest =
            PersistKeyValueRequest(kTestingAreaName, key, val);
        kvRequestQueue_.push(std::move(persistKvRequest));

        // immediately unset this key with empty value to mimick race condition
        auto unsetKvRequest =
            ClearKeyValueRequest(kTestingAreaName, key, deleteVal, true);
        kvRequestQueue_.push(std::move(unsetKvRequest));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 200), [&]() noexcept {
        // No key-val injected after throttled time
        auto maybeThriftVal = kvStore_->getKey(kTestingAreaName, key);
        ASSERT_FALSE(maybeThriftVal.has_value());
      });

  //
  // Test2: - persist key X and wait for throttle to kick in;
  //        - unset key X;
  //        - persist key X again before `unsetKey()` throttle kicks in;
  //        - expect key X is presented in `KvStore` as persistKey() operation
  //          happens chronologically later;
  //
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 100), [&]() noexcept {
        auto persistKvRequest =
            PersistKeyValueRequest(kTestingAreaName, key, val);
        kvRequestQueue_.push(std::move(persistKvRequest));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 200), [&]() noexcept {
        // Wait for throttling. Verify k1 has been populated to KvStore.
        auto maybeThriftVal = kvStore_->getKey(kTestingAreaName, key);
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ(val, maybeThriftVal.value().value());

        // unsetKey() call with throttled fashion
        auto unsetKvRequest =
            ClearKeyValueRequest(kTestingAreaName, key, deleteVal, true);
        kvRequestQueue_.push(std::move(unsetKvRequest));

        // immediately persist this key again to mimick race condition
        auto persistKvRequest =
            PersistKeyValueRequest(kTestingAreaName, key, val);
        kvRequestQueue_.push(std::move(persistKvRequest));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 200), [&]() noexcept {
        // Wait for throttling. Verify k1 is still populated to KvStore
        // without corruption by unsetKey() call
        auto maybeThriftVal = kvStore_->getKey(kTestingAreaName, key);
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_EQ(val, maybeThriftVal.value().value());

        // End test.
        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/**
 * Validate PersistKeyValueRequest advertisement, version overriding, and
 * ttl-refreshing. Send PersistKeyValueRequest via queue to KvStore.
 */
TEST_F(
    KvStoreSelfOriginatedKeyValueRequestFixture,
    ValidateSelfOriginatedKeyTimerTimeout) {
  const std::string nodeId{"throttle-node"};
  initKvStore(nodeId, kShortTtl, kSelfAdjTimeout);

  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::milliseconds(2 * kSelfAdjTimeout), [&]() noexcept {
        EXPECT_FALSE(kvStore_->checkInitialSelfOriginatedKeysTimerScheduled());
        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
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
