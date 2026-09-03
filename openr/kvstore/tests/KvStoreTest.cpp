/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/container/F14Map.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;
using namespace std::chrono;
using ::testing::Eq;
using ::testing::IsTrue;

namespace fb303 = facebook::fb303;

namespace {

// TTL in ms
const int64_t kTtlMs = 1000;

// wait time before checking counter
const std::chrono::milliseconds counterUpdateWaitTime(5500);

// Timeout of checking keys are propagated in all KvStores in the same area.
const std::chrono::milliseconds kTimeoutOfKvStorePropagation(500);

folly::StringPiece
getContainingDirectory(folly::StringPiece input) {
  auto pos = folly::rfind(input, '/');
  return (pos == std::string::npos) ? "" : input.subpiece(0, pos + 1);
}

const std::string kTestDir = getContainingDirectory(__FILE__).str();

// [CONFIG OVERRIDE]
thrift::KvStoreConfig
getTestKvConf(std::string nodeId) {
  thrift::KvStoreConfig kvConf;
  kvConf.node_name() = nodeId;
  return kvConf;
}

/**
 * Fixture for abstracting out common functionality for unittests.
 */
class KvStoreTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // nothing to do
  }

  void
  TearDown() override {
    for (auto& store : stores_) {
      store->stop();
    }
  }

  /**
   * Helper function to create KvStoreWrapper. The underlying stores will be
   * stopped as well as destroyed automatically when test exits.
   * Returned raw pointer of an object will be freed as well.
   */
  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>*
  createKvStore(
      thrift::KvStoreConfig kvStoreConf,
      const folly::F14FastSet<std::string>& areaIds = {kTestingAreaName.t},
      std::optional<messaging::RQueue<PeerEvent>> peerUpdatesQueue =
          std::nullopt,
      std::optional<messaging::RQueue<KeyValueRequest>> kvRequestQueue =
          std::nullopt,
      std::optional<FabricConfig> fabricConfig = std::nullopt) {
    stores_.emplace_back(
        std::make_unique<
            KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>>(
            areaIds,
            kvStoreConf,
            peerUpdatesQueue,
            kvRequestQueue,
            std::move(fabricConfig)));
    return stores_.back().get();
  }

  /**
   * Utility function to create a nodeId/originId based on it's index and
   * prefix
   */
  std::string
  getNodeId(const std::string& prefix, size_t index) const {
    return fmt::format("{}{}", prefix, index);
  }

  void
  waitForAllPeersInitialized() const {
    bool allInitialized = false;
    while (!allInitialized) {
      std::this_thread::yield();
      allInitialized = true;
      for (auto const& store : stores_) {
        for (auto const& area : store->getAreaIds()) {
          for (auto const& [_, spec] : store->getPeers(AreaId{area})) {
            allInitialized &=
                ((*spec.state() == thrift::KvStorePeerState::INITIALIZED) ? 1
                                                                          : 0);
          }
        }
      }
    }
    ASSERT_TRUE(allInitialized);
    LOG(INFO) << "All kvStore peers got initial synced.";
  }

  void
  waitForKeyInStoreWithTimeout(
      KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* store,
      AreaId const& areaId,
      std::string const& key) const {
    auto const start = std::chrono::steady_clock::now();
    while (!store->getKey(areaId, key).has_value() &&
           (std::chrono::steady_clock::now() - start <
            kTimeoutOfKvStorePropagation)) {
      std::this_thread::yield();
    }
    ASSERT_TRUE(store->getKey(areaId, key).has_value());
  }

  void
  validateThriftValue(
      const thrift::Value& actualVal, const thrift::Value& expVal) {
    // validate thrift value field except ttl/ttlVersion
    EXPECT_EQ(*actualVal.version(), *expVal.version());
    EXPECT_EQ(*actualVal.originatorId(), *expVal.originatorId());
    EXPECT_EQ(
        *apache::thrift::get_pointer(actualVal.value()),
        *apache::thrift::get_pointer(expVal.value()));
  }

  // Internal stores
  std::vector<std::unique_ptr<
      KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>>>
      stores_{};
};

} // namespace

/**
 * Validate retrieval of a single key-value from a node's KvStore.
 */
CO_TEST_F(KvStoreTestFixture, BasicGetKey) {
  // Create and start KvStore.
  const std::string nodeId = "node-for-retrieval";
  auto kvStore_ = createKvStore(getTestKvConf(nodeId));
  kvStore_->run();

  const std::string key = "get-key-key";
  const std::string value = "get-key-value";

  // 1. Get key. Make sure it doesn't exist in KvStore yet.
  // 2. Set key manually using KvStoreWrapper.
  // 3. Get key. Make sure it exists and value matches.

  thrift::KeyGetParams paramsBefore;
  paramsBefore.keys()->emplace_back(key);
  auto pub = co_await kvStore_->getKvStore()->co_getKvStoreKeyVals(
      kTestingAreaName, std::move(paramsBefore));
  auto it = pub->keyVals()->find(key);
  EXPECT_EQ(it, pub->keyVals()->end());

  // Set a key in KvStore.
  const thrift::Value thriftVal = createThriftValue(
      1 /* version */,
      nodeId /* originatorId */,
      value /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(1, nodeId, thrift::Value().value() = std::string(value)));
  kvStore_->setKey(kTestingAreaName, key, thriftVal);

  // Check that value retrieved is same as value that was set.
  thrift::KeyGetParams paramsAfter;
  paramsAfter.keys()->emplace_back(key);
  auto pubAfter = co_await kvStore_->getKvStore()->co_getKvStoreKeyVals(
      kTestingAreaName, std::move(paramsAfter));
  auto itAfter = pubAfter->keyVals()->find(key);
  EXPECT_NE(itAfter, pubAfter->keyVals()->end());
  auto& valueFromStore = itAfter->second;
  EXPECT_EQ(valueFromStore.value(), value);
  EXPECT_EQ(*valueFromStore.version(), 1);
  EXPECT_EQ(*valueFromStore.ttlVersion(), 0);
}

CO_TEST_F(KvStoreTestFixture, SelfOriginatedKeyApis) {
  const std::string nodeId = "self-originated-key-node";
  auto* kvStore = createKvStore(getTestKvConf(nodeId));
  kvStore->run();

  const std::string key = "self-originated-key";
  const std::string persistedValue = "persisted-value";
  const std::string finalValue = "final-value";
  const auto makeParams = [&](std::string value) {
    thrift::KeySetParams params;
    params.keyVals()->emplace(
        key,
        createThriftValue(
            1, nodeId, std::move(value), Constants::kTtlInfinity, 0));
    return params;
  };

  auto persistParams = makeParams(persistedValue);
  co_await kvStore->getKvStore()->co_persistSelfOriginatedKey(
      kTestingAreaName, std::move(persistParams));
  auto selfOriginatedKeys = kvStore->dumpAllSelfOriginated(kTestingAreaName);
  CO_ASSERT_TRUE(selfOriginatedKeys.contains(key));
  EXPECT_EQ(persistedValue, *selfOriginatedKeys.at(key).value.value());

  auto unsetParams = makeParams(finalValue);
  co_await kvStore->getKvStore()->co_unsetSelfOriginatedKey(
      kTestingAreaName, std::move(unsetParams));
  EXPECT_EQ(0, kvStore->dumpAllSelfOriginated(kTestingAreaName).count(key));
}

CO_TEST_F(KvStoreTestFixture, SelfOriginatedKeyTasksRejectInvalidArea) {
  const std::string nodeId = "self-originated-key-invalid-area";
  auto* kvStore = createKvStore(getTestKvConf(nodeId));
  kvStore->run();

  const std::string invalidArea = "invalid-area";
  const auto makeParams = [&]() {
    thrift::KeySetParams params;
    params.keyVals()->emplace(
        "self-originated-key",
        createThriftValue(1, nodeId, "value", Constants::kTtlInfinity, 0));
    return params;
  };

  auto persistParams = makeParams();
  CO_ASSERT_THROW(
      co_await kvStore->getKvStore()->co_persistSelfOriginatedKey(
          invalidArea, std::move(persistParams)),
      thrift::KvStoreError);
  auto unsetParams = makeParams();
  CO_ASSERT_THROW(
      co_await kvStore->getKvStore()->co_unsetSelfOriginatedKey(
          invalidArea, std::move(unsetParams)),
      thrift::KvStoreError);
}

/**
 * Validate retrieval of all key-values matching a given prefix.
 */
TEST_F(KvStoreTestFixture, DumpKeysWithPrefix) {
  // Create and start KvStore.
  const std::string nodeId = "node-for-dump";
  auto kvStore_ = createKvStore(getTestKvConf(nodeId));
  kvStore_->run();

  const std::string prefixRegex = "10\\.0\\.0\\.";
  const std::string badPrefixRegex = "[10\\.0\\.0\\.";
  const std::string prefix1 = "10.0.0.96";
  const std::string prefix2 = "10.0.0.128";
  const std::string prefix3 = "192.10.0.0";
  const std::string prefix4 = "192.168.0.0";

  // 1. Dump keys with no matches.
  // 2. Set keys manully. 2 include prefix, 2 do not.
  // 3. Dump keys. Verify 2 that include prefix are in dump, others are not.
  std::optional<thrift::KeyVals> maybeKeyMap;
  try {
    thrift::KeyDumpParams params;
    params.keys() = {prefixRegex};
    auto pub = *kvStore_->getKvStore()
                    ->semifuture_dumpKvStoreKeys(
                        std::move(params), {kTestingAreaName.t})
                    .get()
                    ->begin();
    maybeKeyMap = *pub.keyVals();
  } catch (const std::exception&) {
    maybeKeyMap = std::nullopt;
  }
  EXPECT_TRUE(maybeKeyMap.has_value());
  EXPECT_EQ(maybeKeyMap.value().size(), 0);

  const std::string genValue = "generic-value";
  const thrift::Value thriftVal = createThriftValue(
      1 /* version */,
      nodeId /* originatorId */,
      genValue /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(1, nodeId, thrift::Value().value() = std::string(genValue)));
  kvStore_->setKey(kTestingAreaName, prefix1, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix2, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix3, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix4, thriftVal);

  // Check that keys retrieved are those with prefix "10.0.0".
  std::optional<thrift::KeyVals> maybeKeysAfterInsert;
  try {
    thrift::KeyDumpParams params;
    params.keys() = {prefixRegex};
    auto pub = *kvStore_->getKvStore()
                    ->semifuture_dumpKvStoreKeys(
                        std::move(params), {kTestingAreaName.t})
                    .get()
                    ->begin();
    maybeKeysAfterInsert = *pub.keyVals();
  } catch (const std::exception&) {
    maybeKeysAfterInsert = std::nullopt;
  }
  EXPECT_TRUE(maybeKeysAfterInsert.has_value());
  auto keysFromStore = maybeKeysAfterInsert.value();
  EXPECT_EQ(keysFromStore.size(), 2);
  EXPECT_EQ(keysFromStore.count(prefix1), 1);
  EXPECT_EQ(keysFromStore.count(prefix2), 1);
  EXPECT_EQ(keysFromStore.count(prefix3), 0);
  EXPECT_EQ(keysFromStore.count(prefix4), 0);

  // Check that all keys are retrieved when bad prefix "[10.0.0" (missing
  // right bracket) is given.
  try {
    thrift::KeyDumpParams params;
    params.keys() = {badPrefixRegex};
    auto pub = *kvStore_->getKvStore()
                    ->semifuture_dumpKvStoreKeys(
                        std::move(params), {kTestingAreaName.t})
                    .get()
                    ->begin();
    maybeKeysAfterInsert = *pub.keyVals();
  } catch (const std::exception&) {
    maybeKeysAfterInsert = std::nullopt;
  }
  EXPECT_TRUE(maybeKeysAfterInsert.has_value());
  keysFromStore = maybeKeysAfterInsert.value();
  EXPECT_EQ(keysFromStore.size(), 4);
  EXPECT_EQ(keysFromStore.count(prefix1), 1);
  EXPECT_EQ(keysFromStore.count(prefix2), 1);
  EXPECT_EQ(keysFromStore.count(prefix3), 1);
  EXPECT_EQ(keysFromStore.count(prefix4), 1);
}

#if FOLLY_HAS_COROUTINES
CO_TEST_F(KvStoreTestFixture, CoDumpKeysWithPrefix) {
  // Create and start KvStore.
  const std::string nodeId = "node-for-dump";
  auto kvStore_ = createKvStore(getTestKvConf(nodeId));
  kvStore_->run();

  const std::string prefixRegex = "10\\.0\\.0\\.";
  const std::string badPrefixRegex = "[10\\.0\\.0\\.";
  const std::string prefix1 = "10.0.0.96";
  const std::string prefix2 = "10.0.0.128";
  const std::string prefix3 = "192.10.0.0";
  const std::string prefix4 = "192.168.0.0";

  // 1. Dump keys with no matches.
  // 2. Set keys manully. 2 include prefix, 2 do not.
  // 3. Dump keys. Verify 2 that include prefix are in dump, others are not.
  std::optional<thrift::KeyVals> maybeKeyMap;
  try {
    thrift::KeyDumpParams params;
    params.keys() = {prefixRegex};
    auto pub = *kvStore_->getKvStore()
                    ->semifuture_dumpKvStoreKeys(
                        std::move(params), {kTestingAreaName.t})
                    .get()
                    ->begin();
    maybeKeyMap = *pub.keyVals();
  } catch (const std::exception&) {
    maybeKeyMap = std::nullopt;
  }
  EXPECT_TRUE(maybeKeyMap.has_value());
  EXPECT_EQ(maybeKeyMap.value().size(), 0);

  const std::string genValue = "generic-value";
  const thrift::Value thriftVal = createThriftValue(
      1 /* version */,
      nodeId /* originatorId */,
      genValue /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(1, nodeId, thrift::Value().value() = std::string(genValue)));
  kvStore_->setKey(kTestingAreaName, prefix1, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix2, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix3, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix4, thriftVal);

  // Check that keys retrieved are those with prefix "10.0.0".
  std::optional<thrift::KeyVals> maybeKeysAfterInsert;
  try {
    thrift::KeyDumpParams params;
    params.keys() = {prefixRegex};
    std::set<std::string> areas = {kTestingAreaName.t};
    auto pub = co_await kvStore_->getKvStore()->co_dumpKvStoreKeys(
        std::move(params), areas);
    maybeKeysAfterInsert = *pub->begin()->keyVals();
  } catch (const std::exception&) {
    maybeKeysAfterInsert = std::nullopt;
  }
  EXPECT_TRUE(maybeKeysAfterInsert.has_value());
  auto keysFromStore = maybeKeysAfterInsert.value();
  EXPECT_EQ(keysFromStore.size(), 2);
  EXPECT_EQ(keysFromStore.count(prefix1), 1);
  EXPECT_EQ(keysFromStore.count(prefix2), 1);
  EXPECT_EQ(keysFromStore.count(prefix3), 0);
  EXPECT_EQ(keysFromStore.count(prefix4), 0);

  // Check that all keys are retrieved when bad prefix "[10.0.0" (missing
  // right bracket) is given.
  try {
    thrift::KeyDumpParams params;
    params.keys() = {badPrefixRegex};
    std::set<std::string> badAreas = {kTestingAreaName.t};
    auto pub = co_await kvStore_->getKvStore()->co_dumpKvStoreKeys(
        std::move(params), badAreas);
    maybeKeysAfterInsert = *pub->begin()->keyVals();
  } catch (const std::exception&) {
    maybeKeysAfterInsert = std::nullopt;
  }
  EXPECT_TRUE(maybeKeysAfterInsert.has_value());
  keysFromStore = maybeKeysAfterInsert.value();
  EXPECT_EQ(keysFromStore.size(), 4);
  EXPECT_EQ(keysFromStore.count(prefix1), 1);
  EXPECT_EQ(keysFromStore.count(prefix2), 1);
  EXPECT_EQ(keysFromStore.count(prefix3), 1);
  EXPECT_EQ(keysFromStore.count(prefix4), 1);
}
#endif

/**
 * Verify KvStore publishes kvStoreSynced signal even when receiving empty peers
 * in initialization process.
 */
TEST_F(KvStoreTestFixture, PublishKvStoreSyncedForEmptyPeerEvent) {
  messaging::ReplicateQueue<PeerEvent> myPeerUpdatesQueue;
  auto myStore = createKvStore(
      getTestKvConf("node1"),
      {kTestingAreaName} /* areas */,
      myPeerUpdatesQueue.getReader());
  myStore->run();
  // Publish empty peers.
  myPeerUpdatesQueue.push(PeerEvent());
  // Expect to receive kvStoreSynced signal.
  myStore->recvKvStoreSyncedSignal();
}

/**
 * Verify KvStore publishes kvStoreSynced signal when receiving peers in some
 * configured areas but not others.
 */
TEST_F(KvStoreTestFixture, PublishKvStoreSyncedIfNoPeersInSomeAreas) {
  messaging::ReplicateQueue<PeerEvent> storeAPeerUpdatesQueue;
  messaging::ReplicateQueue<PeerEvent> storeBPeerUpdatesQueue;
  auto* storeA = createKvStore(
      getTestKvConf("storeA"), {"area1"}, storeAPeerUpdatesQueue.getReader());
  // storeB is configured with two areas.
  auto* storeB = createKvStore(
      getTestKvConf("storeB"),
      {"area1", "area2"},
      storeBPeerUpdatesQueue.getReader());
  storeA->run();
  storeB->run();

  // storeA receives peers in the only "area1", and published kvStoreSynced
  // signal.
  thrift::PeersMap peersA;
  peersA.emplace(storeB->getNodeId(), storeB->getPeerSpec());
  PeerEvent peerEventA{{"area1", AreaPeerEvent(peersA, {} /*peersToDel*/)}};
  storeAPeerUpdatesQueue.push(peerEventA);
  storeA->recvKvStoreSyncedSignal();

  // storeB receives one peer in "area1" but empty peers in "area2". OpenR
  // initialization is converged and kvStoreSynced signal is published.
  thrift::PeersMap peersB;
  peersB.emplace(storeA->getNodeId(), storeA->getPeerSpec());
  PeerEvent peerEventB{{"area1", AreaPeerEvent(peersB, {} /*peersToDel*/)}};
  storeBPeerUpdatesQueue.push(peerEventB);
  storeB->recvKvStoreSyncedSignal();
}

/*
 * Verify if an inconsistent update(a ttl update with missing key) is received,
 * kvStores will resync and reach eventual consistency.
 *
 * Topology:
 *
 * [originator]  [inconsistency detector] [rest of the peers]
 *      |                  |                   |
 *      A(key, 1)  ----- B(null, null) ---- C(key, 1)
 *
 * Setup:
 *  - A(inconsistent store) originiates (key, 1) and keeps in sync with B.
 *  - Force B to remove
 */
TEST_F(KvStoreTestFixture, ResyncUponTtlUpdateWithMissingKey) {
  // setup inconsistent store and persist key via queue from test
  const auto ttl{1000};
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
  auto config = getTestKvConf("storeA");
  config.key_ttl_ms() = ttl; // set ttl to trigger ttl update later
  auto* inconsistentStore = createKvStore(
      std::move(config),
      {kTestingAreaName},
      std::nullopt,
      kvRequestQueue_.getReader());

  // setup B, C and connect them
  auto* storeB = createKvStore(getTestKvConf("storeB"));
  auto* storeC = createKvStore(getTestKvConf("storeC"));
  inconsistentStore->run();
  storeB->run();
  storeC->run();

  // A - B
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName,
      inconsistentStore->getNodeId(),
      inconsistentStore->getPeerSpec()));
  EXPECT_TRUE(inconsistentStore->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  // B - C
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeC->getNodeId(), storeC->getPeerSpec()));
  EXPECT_TRUE(storeC->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  waitForAllPeersInitialized();

  // Inconsistent store originates key, with default version=1
  // B and C will automatically get the update of (key, 1) in their stores
  const std::string key{"key"};
  const std::string value{"val"};
  const auto version{1};
  kvRequestQueue_.push(PersistKeyValueRequest(kTestingAreaName, key, value));

  // make sure storeB and storeC received the update
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, key);
  waitForKeyInStoreWithTimeout(storeC, kTestingAreaName, key);

  LOG(INFO) << "All stores have received expected key update.";

  // Force B to have higher version but expire immediately.
  // Set the `nodeIds` to mark A and C have already received the updates.
  const thrift::Value thriftVal = createThriftValue(
      version + 1 /* version */,
      inconsistentStore->getNodeId() /* originatorId */,
      value /* value */,
      1 /* short ttl to trigger expiration */,
      0 /* ttl version */);

  storeB->setKey(
      kTestingAreaName,
      key,
      thriftVal,
      std::optional<std::vector<std::string>>({"storeA", "storeC"})
      /* set nodeIds to prevent update */);

  // Check all stores in sync after a TTL update from A. With higher version.
  OpenrEventBase evb;
  int scheduleAt{0};

  // wait until a TTL update is send and full resync is done between A and B
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += ttl), [&]() noexcept {
        // All kvstores are in sync
        const auto expValue = createThriftValue(
            version /* version */,
            inconsistentStore->getNodeId() /* originatorId */,
            value /* value */);
        {
          auto allKeyVals = storeB->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }
        {
          auto allKeyVals = inconsistentStore->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }
        {
          auto allKeyVals = storeC->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }

        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/*
 * Verify if an inconsistent update (a ttl update with incorrect key version) is
 * received, kvStores will resync and reach eventual consistency.
 *
 * Topology:
 *
 * [originator]  [inconsistency detector] [rest of the peers]
 *      |                  |                   |
 *      A(key, 1) ---- B(key, 20) ---- C(key, 1)
 *
 * Setup:
 *  - A(inconsistent store) originates (key, 1) and keeps in sync with B.
 *  - Force B to set key version with (key, 20) and prevent {A, C} receiving
 *    flooding update by marking they are "visited" nodeIds.
 *  - A(inconsisten store) sends a TTL update at some point to B.
 *  - B's mergeKeyVals call Will trigger resync and {A, B, C} will be eventually
 *    in consistent state again.
 *
 * NOTE: C will not directly resync with B, but received updated version from A
 * via B since A is persisting the key.
 */
TEST_F(KvStoreTestFixture, ResyncUponTtlUpdateWithInconsistentVersion) {
  const auto ttl{1000};
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
  auto config = getTestKvConf("storeA");
  config.key_ttl_ms() = ttl; // set ttl to trigger ttl update later
  auto* inconsistentStore = createKvStore(
      std::move(config),
      {kTestingAreaName},
      std::nullopt,
      kvRequestQueue_.getReader());
  auto* storeB = createKvStore(getTestKvConf("storeB"));
  auto* storeC = createKvStore(getTestKvConf("storeC"));
  inconsistentStore->run();
  storeB->run();
  storeC->run();

  // A - B
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName,
      inconsistentStore->getNodeId(),
      inconsistentStore->getPeerSpec()));
  EXPECT_TRUE(inconsistentStore->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  // B - C
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeC->getNodeId(), storeC->getPeerSpec()));
  EXPECT_TRUE(storeC->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  waitForAllPeersInitialized();

  const std::string key{"key"};
  const std::string value{"val"};
  const auto version{20};

  // Inconsistent store originates key, with default version=1
  // B and C will automatically get the update of (key, 1) in their stores
  kvRequestQueue_.push(PersistKeyValueRequest(kTestingAreaName, key, value));

  // make sure storeB and storeC received the update
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, key);
  waitForKeyInStoreWithTimeout(storeC, kTestingAreaName, key);

  // Force B to have higher version. Store A and C is not updated (By setting
  // the `nodeIds` to mark A and C already received the updates)
  const thrift::Value thriftVal = createThriftValue(
      version /* version */,
      inconsistentStore->getNodeId() /* originatorId */,
      value /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */);

  storeB->setKey(
      kTestingAreaName,
      key,
      thriftVal,
      std::optional<std::vector<std::string>>({"storeA", "storeC"})
      /* set nodeIds to prevent update */);

  OpenrEventBase evb;
  int scheduleAt{0};
  // Check both store to be in sync after a TTL update from A. With higher
  // version

  // wait until a TTL update is send and full resync is done between A and B
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += ttl), [&]() noexcept {
        // All kvstores are in sync
        const auto expValue = createThriftValue(
            version + 1 /* version */,
            inconsistentStore->getNodeId() /* originatorId */,
            value /* value */);
        {
          auto allKeyVals = storeB->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }
        {
          auto allKeyVals = inconsistentStore->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }
        {
          auto allKeyVals = storeC->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }

        auto cmpPeers = storeB->getPeers(kTestingAreaName);
        EXPECT_LT(*cmpPeers["storeA"].stateElapsedTimeMs(), ttl);
        EXPECT_EQ(*cmpPeers["storeA"].flaps(), 2);
        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/**
 * 1) Inject thrift failure for one of the established peer
 * 2) Verify syncing for the peer at configured "Initial" backoff period
 * 3) Re-inject thrift failure for same peer multiple times (twice)
 *    With configured backoff, 2nd thrift failure shall push backoff to max
 * 4) Verify syncing for the peer at configured "Max" backoff period
 */
TEST_F(KvStoreTestFixture, PeerResyncWithConfiguredBackoff) {
  const std::chrono::milliseconds ksyncInitialBackoff(1000);
  const std::chrono::milliseconds ksyncMaxBackoff(1800);
  const std::chrono::milliseconds ksyncValidationTime(2500);

  auto config = getTestKvConf("storeA");
  config.sync_initial_backoff_ms() = ksyncInitialBackoff.count();
  config.sync_max_backoff_ms() = ksyncMaxBackoff.count();

  auto* storeA = createKvStore(config, {kTestingAreaName});
  auto* storeB = createKvStore(getTestKvConf("storeB"));
  auto* storeC = createKvStore(getTestKvConf("storeC"));
  storeA->run();
  storeB->run();
  storeC->run();

  // A - B
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()));
  EXPECT_TRUE(storeA->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  // B - C
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeC->getNodeId(), storeC->getPeerSpec()));
  EXPECT_TRUE(storeC->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  waitForAllPeersInitialized();
  auto cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::INITIALIZED);

  storeA->injectThriftFailure(kTestingAreaName, "storeB");
  auto start = std::chrono::steady_clock::now();

  cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::IDLE);

  waitForAllPeersInitialized();
  auto elapsedTime =
      duration_cast<milliseconds>(steady_clock::now() - start).count();
  // discount 1ms. We have seen some-times elapsed time is
  //     999 ms instead of 1000 ms of Initial Backoff
  EXPECT_GE(elapsedTime, ksyncInitialBackoff.count() - 1);
  EXPECT_LT(elapsedTime, ksyncMaxBackoff.count());

  cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::INITIALIZED);

  start = steady_clock::now();
  storeA->injectThriftFailure(kTestingAreaName, "storeB");

  OpenrEventBase evb;
  int scheduleAt{0};
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += (ksyncInitialBackoff.count() / 2)),
      [&]() noexcept {
        storeA->injectThriftFailure(kTestingAreaName, "storeB");
        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();

  waitForAllPeersInitialized();
  elapsedTime =
      duration_cast<milliseconds>(steady_clock::now() - start).count();
  EXPECT_GT(elapsedTime, ksyncMaxBackoff.count());
  EXPECT_LT(elapsedTime, ksyncValidationTime.count());

  cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::INITIALIZED);
}

TEST_F(KvStoreTestFixture, PeerResyncWithEqualConfiguredBackoff) {
  const std::chrono::milliseconds ksyncInitialBackoff(1000);
  const std::chrono::milliseconds ksyncMaxBackoff(1000);
  const std::chrono::milliseconds ksyncValidationTime(2000);

  auto config = getTestKvConf("storeA");
  config.sync_initial_backoff_ms() = ksyncInitialBackoff.count();
  config.sync_max_backoff_ms() = ksyncMaxBackoff.count();

  auto* storeA = createKvStore(config, {kTestingAreaName});
  auto* storeB = createKvStore(getTestKvConf("storeB"));
  auto* storeC = createKvStore(getTestKvConf("storeC"));
  storeA->run();
  storeB->run();
  storeC->run();

  // A - B
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()));
  EXPECT_TRUE(storeA->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  // B - C
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeC->getNodeId(), storeC->getPeerSpec()));
  EXPECT_TRUE(storeC->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  waitForAllPeersInitialized();
  auto cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::INITIALIZED);

  storeA->injectThriftFailure(kTestingAreaName, "storeB");
  auto start = std::chrono::steady_clock::now();

  cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::IDLE);

  waitForAllPeersInitialized();
  auto elapsedTime =
      duration_cast<milliseconds>(steady_clock::now() - start).count();
  // discount 1ms. We have seen some-times elapsed time is
  //     999 ms instead of 1000 ms of Initial Backoff
  EXPECT_GE(elapsedTime, ksyncInitialBackoff.count() - 1);
  EXPECT_LT(elapsedTime, ksyncValidationTime.count());

  cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::INITIALIZED);

  start = steady_clock::now();
  storeA->injectThriftFailure(kTestingAreaName, "storeB");

  OpenrEventBase evb;
  int scheduleAt{0};
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += (ksyncInitialBackoff.count() / 2)),
      [&]() noexcept {
        storeA->injectThriftFailure(kTestingAreaName, "storeB");
        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();

  waitForAllPeersInitialized();
  elapsedTime =
      duration_cast<milliseconds>(steady_clock::now() - start).count();
  EXPECT_GT(elapsedTime, ksyncMaxBackoff.count());
  EXPECT_LT(elapsedTime, ksyncValidationTime.count());

  cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::INITIALIZED);
}

TEST_F(KvStoreTestFixture, PeerResyncWithDefaultBackoff) {
  auto* storeA = createKvStore(getTestKvConf("storeA"));
  auto* storeB = createKvStore(getTestKvConf("storeB"));
  auto* storeC = createKvStore(getTestKvConf("storeC"));
  storeA->run();
  storeB->run();
  storeC->run();

  // A - B
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()));
  EXPECT_TRUE(storeA->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  // B - C
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeC->getNodeId(), storeC->getPeerSpec()));
  EXPECT_TRUE(storeC->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  waitForAllPeersInitialized();
  auto cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::INITIALIZED);

  auto start = std::chrono::steady_clock::now();
  storeA->injectThriftFailure(kTestingAreaName, "storeB");

  cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::IDLE);

  waitForAllPeersInitialized();
  auto elapsedTime =
      duration_cast<milliseconds>(steady_clock::now() - start).count();
  // discount 2ms. We have seen some-times elapsed time is
  //     3999 ms instead of 4000 ms of Initial Backoff
  EXPECT_GT(elapsedTime, (Constants::kKvstoreSyncInitialBackoff.count() - 2));
  EXPECT_LT(elapsedTime, Constants::kKvstoreSyncMaxBackoff.count());

  cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());
  EXPECT_EQ(*cmpPeers["storeB"].state(), thrift::KvStorePeerState::INITIALIZED);
}

/**
 * Let KVSTORE_SYNCED signal learned as soon when there are no peers
 *
 * Create KvStore just for one node without any peers
 *
 * Then wait for KVSTORE_SYNCED signal and verify that sync signal was
 * immediately
 */
TEST_F(KvStoreTestFixture, KvStoreSyncWithoutTimeout) {
  messaging::ReplicateQueue<PeerEvent> myPeerUpdatesQueue;

  auto config = getTestKvConf("storeA");

  auto const start = std::chrono::steady_clock::now();
  auto* storeA =
      createKvStore(config, {kTestingAreaName}, myPeerUpdatesQueue.getReader());
  storeA->run();

  // Publish empty peers.
  myPeerUpdatesQueue.push(PeerEvent());

  // Wait for KVSTORE_SYNCED signal
  storeA->recvKvStoreSyncedSignal();

  auto cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(0, cmpPeers.size());
  auto elapsedTime =
      duration_cast<milliseconds>(steady_clock::now() - start).count();

  // Should receive KVSTORE_SYNCED much before 1000 ms
  EXPECT_LT(elapsedTime, 1000);
}

/**
 * Explicitly set kvstoreConfig for timeout to declare KVSTORE_SYNCED
 * when there are no peers that can be learned (with empty PeerEvent)
 *
 * Create KvStore just for one node without any peers
 *
 * Then wait for KVSTORE_SYNCED signal and verify that sync signal was
 * received only after the timeout value
 */
TEST_F(KvStoreTestFixture, KvStoreSyncTimeoutWithEmptyPeerUpdate) {
  messaging::ReplicateQueue<PeerEvent> myPeerUpdatesQueue;
  const std::chrono::milliseconds kKvStoreSyncTimeout(2000);
  const std::chrono::milliseconds kKvStoreSyncTimeoutUpperCheck(2200);

  auto config = getTestKvConf("storeA");
  config.kvstore_sync_timeout_ms() = kKvStoreSyncTimeout.count();

  auto const start = std::chrono::steady_clock::now();
  auto* storeA =
      createKvStore(config, {kTestingAreaName}, myPeerUpdatesQueue.getReader());
  storeA->run();

  // Publish empty peers.
  myPeerUpdatesQueue.push(PeerEvent());

  // Wait for KVSTORE_SYNCED signal
  storeA->recvKvStoreSyncedSignal();

  auto cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(0, cmpPeers.size());

  auto elapsedTime =
      duration_cast<milliseconds>(steady_clock::now() - start).count();
  EXPECT_GT(elapsedTime, kKvStoreSyncTimeout.count());
  EXPECT_LT(elapsedTime, kKvStoreSyncTimeoutUpperCheck.count());
}

/**
 * Explicitly set kvstoreConfig for timeout to declare KVSTORE_SYNCED
 * when there are no peers that can be learned
 *
 * Create KvStore just for one node without any peers
 *
 * Then wait for KVSTORE_SYNCED signal and verify that sync signal was
 * received only after the timeout value
 */
TEST_F(KvStoreTestFixture, KvStoreSyncTimeoutWithoutPeerUpdate) {
  messaging::ReplicateQueue<PeerEvent> myPeerUpdatesQueue;
  const std::chrono::milliseconds kKvStoreSyncTimeout(2000);
  const std::chrono::milliseconds kKvStoreSyncTimeoutUpperCheck(2200);

  auto config = getTestKvConf("storeA");
  config.kvstore_sync_timeout_ms() = kKvStoreSyncTimeout.count();

  auto const start = std::chrono::steady_clock::now();
  auto* storeA =
      createKvStore(config, {kTestingAreaName}, myPeerUpdatesQueue.getReader());
  storeA->run();

  // Wait for KVSTORE_SYNCED signal
  storeA->recvKvStoreSyncedSignal();

  auto cmpPeers = storeA->getPeers(kTestingAreaName);
  EXPECT_EQ(0, cmpPeers.size());

  auto elapsedTime =
      duration_cast<milliseconds>(steady_clock::now() - start).count();
  EXPECT_GT(elapsedTime, kKvStoreSyncTimeout.count());
  EXPECT_LT(elapsedTime, kKvStoreSyncTimeoutUpperCheck.count());
}

// When you receive a update from 'other' about a key you originates,
//  with some inconsistency (higher ttl_version)
// 1. you should never delete it
// 2. you should update ttl (so other's keyVal do not expire)
TEST_F(KvStoreTestFixture, noDeleteForSelfOriginatedKey) {
  const auto ttlMe{200000}; // Just discovered other bug preventing me to use
                            // infinity here. Will fix the bug
  const auto ttlOther{1};
  const auto nodeName{"test-node"};
  auto config = getTestKvConf(nodeName);
  config.key_ttl_ms() = ttlMe;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
  auto* store = createKvStore(
      std::move(config),
      {kTestingAreaName},
      std::nullopt,
      kvRequestQueue_.getReader());
  store->run();

  const std::string key{"key"};
  const std::string value{"val"};
  const auto version{1};

  // Store originates key, with default version=1
  kvRequestQueue_.push(PersistKeyValueRequest(kTestingAreaName, key, value));
  waitForKeyInStoreWithTimeout(store, kTestingAreaName, key);

  // Receives a pub of the same key from "others", that is expiring soon
  const thrift::Value thriftVal = createThriftValue(
      version /* version */,
      nodeName /* originatorId */,
      value /* value */,
      ttlOther /* ttl */,
      version /* ttl version */);

  store->setKey(kTestingAreaName, key, thriftVal, {});

  const thrift::Value thriftValExp = createThriftValue(
      version /* version */,
      nodeName /* originatorId */,
      value /* value */,
      ttlMe /* ttl */,
      version /* ttl version */);

  OpenrEventBase evb;
  int scheduleAt{0};

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += ttlOther + 3000), [&]() noexcept {
        // Wait 2 seconds
        {
          auto allKeyVals = store->dumpAll(kTestingAreaName);
          // Not deleted
          EXPECT_EQ(1, allKeyVals.size());
          const auto& val = allKeyVals[key];
          validateThriftValue(val, thriftValExp);
        }
        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/*
 * Verify if an inconsistent update (a ttl update with a diff originator) is
 * received, kvStores will resync and reach eventual consistency.
 *
 * Topology:
 *
 * [originator]   [inconsistency detector]          [rest of peers]
 *      |                   |                              |
 * A(key, 1) ---- B(key, 1, diff originator) ---- C(key, diff originator)
 *
 * Setup:
 *  - A(inconsistent store) originates (key, 1) and keeps in sync with B.
 *  - Force B to set key with a different originator and prevent {A} receiving
 *    flooding update by marking they are "visited" nodeIds.
 *  - A(inconsisten store) sends a TTL update at some point to B.
 *  - B's mergeKeyVals call will trigger a resync with:
 *    - A detects B has a higher originatorId after full-sync.
 *    - A advertises version + 1 to override originatorId back.
 *    - {A, B, C} will be in eventual consistency again.
 */
TEST_F(KvStoreTestFixture, ResyncUponTtlUpdateWithInconsistentOriginator) {
  const auto ttl{1000};
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
  auto config = getTestKvConf("storeA");
  config.key_ttl_ms() = ttl; // set ttl to trigger ttl update later
  auto* inconsistentStore = createKvStore(
      std::move(config),
      {kTestingAreaName},
      std::nullopt,
      kvRequestQueue_.getReader());
  auto* storeB = createKvStore(getTestKvConf("storeB"));
  auto* storeC = createKvStore(getTestKvConf("storeC"));
  inconsistentStore->run();
  storeB->run();
  storeC->run();

  // A - B
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName,
      inconsistentStore->getNodeId(),
      inconsistentStore->getPeerSpec()));
  EXPECT_TRUE(inconsistentStore->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  // B - C
  EXPECT_TRUE(storeB->addPeer(
      kTestingAreaName, storeC->getNodeId(), storeC->getPeerSpec()));
  EXPECT_TRUE(storeC->addPeer(
      kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()));

  waitForAllPeersInitialized();

  const std::string key{"key"};
  const std::string value{"val"};
  const auto version{1};

  // Inconsistent store originates key, with default version=1
  // B and C will automatically get the update of (key, 1) in their stores
  kvRequestQueue_.push(PersistKeyValueRequest(kTestingAreaName, key, value));

  // make sure storeA and storeB received the update
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, key);
  waitForKeyInStoreWithTimeout(storeC, kTestingAreaName, key);

  // Force B to have a different originatorId. Store C is not updated(By setting
  // the `nodeIds` to mark C already received the updates)
  const thrift::Value thriftVal = createThriftValue(
      version /* version */,
      storeB->getNodeId() /* diff originatorId */,
      value /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */);

  storeB->setKey(
      kTestingAreaName,
      key,
      thriftVal,
      std::optional<std::vector<std::string>>({"storeA"})
      /* set nodeIds to prevent update */);

  OpenrEventBase evb;
  int scheduleAt{0};
  // Check both store to be in sync after a TTL update from A. With higher
  // version

  // wait until a TTL update is send and full resync is done between A and B
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += ttl), [&]() noexcept {
        // All kvstores are in sync by converging to originator: storeB
        const auto expValue = createThriftValue(
            version + 1 /* version will be overridden by originator */,
            inconsistentStore->getNodeId() /* originatorId */,
            value /* value */);
        {
          auto allKeyVals = inconsistentStore->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }
        {
          auto allKeyVals = storeB->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }
        {
          auto allKeyVals = storeC->dumpAll(kTestingAreaName);
          EXPECT_EQ(1, allKeyVals.size());
          validateThriftValue(allKeyVals[key], expValue);
        }

        auto cmpPeers = storeB->getPeers(kTestingAreaName);
        EXPECT_LT(*cmpPeers["storeA"].stateElapsedTimeMs(), ttl);
        EXPECT_EQ(*cmpPeers["storeA"].flaps(), 2);
        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

/**
 * Start single testable store and set key-val. Verify content of KvStore by
 * querying it.
 */
TEST_F(KvStoreTestFixture, BasicSetKey) {
  // clean up counters before testing
  const std::string& key{"key1"};
  fb303::fbData->resetAllData();

  auto kvStore = createKvStore(getTestKvConf("node1"));
  kvStore->run();

  // Set a key in KvStore
  const thrift::Value thriftVal = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      std::string("value1") /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(
          1, "node1", thrift::Value().value() = std::string("value1")));
  kvStore->setKey(kTestingAreaName, key, thriftVal);

  // check stat was updated
  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters.at("kvstore.cmd_key_set.count"));
  EXPECT_EQ(1, counters.at("kvstore.received_publications.count"));

  // check key was added correctly
  auto recVal = kvStore->getKey(kTestingAreaName, key);
  ASSERT_TRUE(recVal.has_value());
  EXPECT_EQ(
      ComparisonResult::TIED, openr::compareValues(thriftVal, recVal.value()));

  // check only this key exists in kvstore
  folly::F14FastMap<std::string, thrift::Value> expectedKeyVals;
  expectedKeyVals[key] = thriftVal;
  auto allKeyVals = kvStore->dumpAll(kTestingAreaName);
  EXPECT_EQ(1, allKeyVals.size());
  EXPECT_EQ(expectedKeyVals, allKeyVals);

  // set the same key with new value
  auto thriftVal2 = createThriftValue(
      2 /* version */,
      "node1" /* originatorId */,
      std::string("value2") /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(
          2, "node1", thrift::Value().value() = std::string("value2")));
  kvStore->setKey(kTestingAreaName, key, thriftVal2);

  // check merge occurred correctly -- value overwritten
  auto recVal2 = kvStore->getKey(kTestingAreaName, key);
  ASSERT_TRUE(recVal2.has_value());
  EXPECT_EQ(
      ComparisonResult::TIED,
      openr::compareValues(thriftVal2, recVal2.value()));

  // check merge occurred correctly -- no duplicate key
  expectedKeyVals[key] = thriftVal2;
  allKeyVals = kvStore->dumpAll(kTestingAreaName);
  EXPECT_EQ(1, allKeyVals.size());
  EXPECT_EQ(expectedKeyVals, allKeyVals);

  // check stat was updated
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters.at("kvstore.cmd_key_set.count"));
  EXPECT_EQ(2, counters.at("kvstore.received_publications.count"));
}

/**
 * KvStore stamps three PerfEvents on the chain that rides into Decision via
 * the local update queue (RECV_PUB → KVSTORE_MERGED → KVSTORE_HANDOFF).
 * PEER_FLOOD_COMPLETE is stamped on the local-only copy after the peer
 * for-loop; receive-to-advertise latency (RECV_PUB → now) is published as
 * the kvstore.recv_to_advertise_ms fb303 stat.
 */
TEST_F(KvStoreTestFixture, ConvergenceProfilerPerfEventsStamping) {
  fb303::fbData->resetAllData();

  const std::string nodeId = "node-cp";
  const std::string key{"key-cp"};
  auto kvStore = createKvStore(getTestKvConf(nodeId));
  kvStore->run();

  const auto thriftVal = createThriftValue(
      1 /* version */,
      nodeId /* originatorId */,
      std::string("v") /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(1, nodeId, thrift::Value().value() = std::string("v")));
  EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftVal));

  auto publication = kvStore->recvPublication();
  ASSERT_TRUE(publication.perfEvents().has_value());
  const auto& events = *publication.perfEvents()->events();
  ASSERT_EQ(3, events.size());

  const std::vector<std::string> expectedDescriptions{
      "RECV_PUB", "KVSTORE_MERGED", "KVSTORE_HANDOFF"};
  for (size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(expectedDescriptions[i], *events[i].eventDescr());
    EXPECT_EQ(nodeId, *events[i].nodeName());
  }

  const auto counters = fb303::fbData->getCounters();
  EXPECT_TRUE(counters.contains("kvstore.recv_to_advertise_ms.avg"));
  // Max is published as a plain counter (not a stat), so the bare key is
  // present and non-negative after at least one observation.
  ASSERT_TRUE(counters.contains("kvstore.recv_to_advertise_max_ms"));
  EXPECT_GE(counters.at("kvstore.recv_to_advertise_max_ms"), 0);

  // Reset drops the sticky max back to zero.
  resetRecvToAdvertiseMaxMs();
  EXPECT_EQ(
      0, fb303::fbData->getCounters().at("kvstore.recv_to_advertise_max_ms"));
}

//
// Test counter reporting
//
TEST_F(KvStoreTestFixture, CounterReport) {
  // clean up counters before testing
  const std::string& area = kTestingAreaName;
  fb303::fbData->resetAllData();

  auto kvStore = createKvStore(getTestKvConf("node1"));
  kvStore->run();

  /** Verify NO redundant publications **/
  // Set key in KvStore with loop
  const std::vector<std::string> nodeIds{"node2", "node3", "node1", "node4"};
  kvStore->setKey(kTestingAreaName, "test-key", thrift::Value(), nodeIds);
  // Set same key with different path
  const std::vector<std::string> nodeIds2{"node5"};
  kvStore->setKey(kTestingAreaName, "test-key", thrift::Value(), nodeIds2);

  /** Verify key update **/
  // Set a key in KvStore
  thrift::Value thriftVal1 = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      std::string("value1") /* value */,
      counterUpdateWaitTime.count() * 2 /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  kvStore->setKey(kTestingAreaName, "test-key2", thriftVal1);

  // Set same key with different value
  thrift::Value thriftVal2 = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      std::string("value2") /* value */,
      counterUpdateWaitTime.count() * 2 /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  kvStore->setKey(kTestingAreaName, "test-key2", thriftVal2);

  // Wait till counters updated
  std::this_thread::sleep_for(std::chrono::milliseconds(counterUpdateWaitTime));
  auto timeAfterCounterUpdate = getUnixTimeStampMs();
  auto counters = fb303::fbData->getCounters();

  // Verify the counter keys exist
  ASSERT_TRUE(counters.contains("kvstore.num_peers"));
  ASSERT_TRUE(counters.contains("kvstore.cmd_peer_dump.count"));
  ASSERT_TRUE(counters.contains("kvstore.cmd_peer_add.count"));
  ASSERT_TRUE(counters.contains("kvstore.cmd_per_del.count"));
  ASSERT_TRUE(counters.contains("kvstore.expired_key_vals.sum"));
  ASSERT_TRUE(counters.contains("kvstore.thrift.flood_pub_duration_ms.avg"));
  ASSERT_TRUE(counters.contains("kvstore.thrift.full_sync_duration_ms.avg"));
  ASSERT_TRUE(
      counters.contains("kvstore.thrift.finalized_sync_duration_ms.avg"));
  ASSERT_TRUE(counters.contains("kvstore.rate_limit_keys.avg"));
  ASSERT_TRUE(counters.contains("kvstore.rate_limit_suppress.count"));
  ASSERT_TRUE(counters.contains("kvstore.cmd_hash_dump.count"));
  ASSERT_TRUE(counters.contains("kvstore.cmd_self_originated_key_dump.count"));
  ASSERT_TRUE(counters.contains("kvstore.cmd_key_dump.count"));
  ASSERT_TRUE(counters.contains("kvstore.cmd_key_get.count"));
  ASSERT_TRUE(counters.contains("kvstore.updated_key_vals." + area + ".sum"));
  ASSERT_TRUE(counters.contains("kvstore.received_key_vals." + area + ".sum"));
  ASSERT_TRUE(counters.contains("kvstore.last_update.avg"));
  ASSERT_TRUE(
      counters.contains("kvstore.received_publications." + area + ".count"));
  ASSERT_TRUE(counters.contains("kvstore.num_flood_peers"));
  ASSERT_TRUE(counters.contains("kvstore.num_flood_peers." + area + ".sum"));
  ASSERT_TRUE(counters.contains("kvstore.num_expiring_keys"));
  ASSERT_TRUE(counters.contains("kvstore.num_expiring_keys." + area + ".sum"));

  // Verify the value of counter keys
  EXPECT_EQ(0, counters.at("kvstore.num_peers"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_peer_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_peer_add.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_per_del.count"));
  EXPECT_EQ(0, counters.at("kvstore.expired_key_vals.sum"));
  EXPECT_EQ(0, counters.at("kvstore.thrift.flood_pub_duration_ms.avg"));
  EXPECT_EQ(0, counters.at("kvstore.thrift.full_sync_duration_ms.avg"));
  EXPECT_EQ(0, counters.at("kvstore.thrift.finalized_sync_duration_ms.avg"));
  EXPECT_EQ(0, counters.at("kvstore.rate_limit_keys.avg"));
  EXPECT_EQ(0, counters.at("kvstore.rate_limit_suppress.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_hash_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_self_originated_key_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_key_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_key_get.count"));
  EXPECT_EQ(0, counters.at("kvstore.num_flood_peers"));
  EXPECT_EQ(0, counters.at("kvstore.num_flood_peers." + area + ".sum"));
  EXPECT_EQ(0, counters.at("kvstore.num_expiring_keys"));
  EXPECT_EQ(0, counters.at("kvstore.num_expiring_keys." + area + ".sum"));

  // Verify four keys were set
  ASSERT_TRUE(counters.contains("kvstore.cmd_key_set.count"));
  EXPECT_EQ(4, counters.at("kvstore.cmd_key_set.count"));
  ASSERT_TRUE(counters.contains("kvstore.received_key_vals.sum"));
  EXPECT_EQ(3, counters.at("kvstore.received_key_vals.sum"));
  ASSERT_TRUE(counters.contains("kvstore.received_key_vals." + area + ".sum"));
  EXPECT_EQ(3, counters.at("kvstore.received_key_vals." + area + ".sum"));

  // Verify the ttl countdown queue size counter is populated
  // NOTE: counter is 1. We call setKey() 4 times, but the first 2 don't have a
  // ttl and the last 2 have the same (key, originator) combination.
  ASSERT_TRUE(counters.contains("kvstore.ttl_countdown_queue_size." + area));
  EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_queue_size." + area));

  ASSERT_TRUE(
      counters.contains("kvstore.ttl_countdown_handle_map_size." + area));
  EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_handle_map_size." + area));

  // Verify the key and the number of key
  ASSERT_TRUE(kvStore->getKey(kTestingAreaName, "test-key2").has_value());
  ASSERT_EQ(1, counters.count("kvstore.num_keys"));
  EXPECT_EQ(1, counters.at("kvstore.num_keys"));

  // Verify the number key update
  ASSERT_EQ(1, counters.count("kvstore.updated_key_vals.sum"));
  EXPECT_EQ(2, counters.at("kvstore.updated_key_vals.sum"));
  ASSERT_EQ(1, counters.count("kvstore.updated_key_vals." + area + ".sum"));
  EXPECT_EQ(2, counters.at("kvstore.updated_key_vals." + area + ".sum"));
  ASSERT_EQ(1, counters.count("kvstore.last_update.avg"));
  EXPECT_GT(timeAfterCounterUpdate, counters.at("kvstore.last_update.avg"));

  // Verify publication counter
  ASSERT_EQ(1, counters.count("kvstore.looped_publications.count"));
  EXPECT_EQ(1, counters.at("kvstore.looped_publications.count"));
  ASSERT_EQ(1, counters.count("kvstore.received_publications.count"));
  EXPECT_EQ(3, counters.at("kvstore.received_publications.count"));
  ASSERT_EQ(
      1, counters.count("kvstore.received_publications." + area + ".count"));
  EXPECT_EQ(3, counters.at("kvstore.received_publications." + area + ".count"));

  // Verify redundant publication counter
  ASSERT_EQ(1, counters.count("kvstore.received_redundant_publications.count"));
  EXPECT_EQ(1, counters.at("kvstore.received_redundant_publications.count"));

  // Wait for counter update again
  std::this_thread::sleep_for(std::chrono::milliseconds(counterUpdateWaitTime));
  // Verify the num_keys counter is the same
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters.at("kvstore.num_keys"));

  LOG(INFO) << "Counters received, yo";
}

/**
 * Test following with single KvStore.
 * - TTL propagation is carried out correctly
 * - Correct TTL reflects back in GET/KEY_DUMP/KEY_HASH
 * - Applying ttl updates reflects properly
 * - Size of TTL queue and TTL handle map is as expected
 */
CO_TEST_F(KvStoreTestFixture, TtlVerification) {
  const std::string key{"dummyKey"};
  const auto value = createThriftValue(
      5, /* version */
      "node1", /* node id */
      "dummyValue",
      0, /* ttl */
      5 /* ttl version */,
      0 /* hash */);

  auto kvStore = createKvStore(getTestKvConf("test"));
  kvStore->run();
  const std::string& area = kTestingAreaName;

  //
  // 1. Advertise key-value with 1ms rtt
  // - This will get added to local KvStore but will never be published
  //   to other nodes or doesn't show up in GET request
  {
    auto thriftValue = value;
    thriftValue.ttl() = 1;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));
    EXPECT_FALSE(kvStore->getKey(kTestingAreaName, key).has_value());
    EXPECT_EQ(0, kvStore->dumpAll(kTestingAreaName).size());
    EXPECT_EQ(0, kvStore->dumpAll(kTestingAreaName).size());

    // We will receive key-expiry publication but no key-advertisement
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(0, publication.keyVals()->size());
    CO_ASSERT_EQ(1, publication.expiredKeys()->size());
    EXPECT_EQ(key, publication.expiredKeys()->at(0));

    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(0, counters.at("kvstore.ttl_countdown_queue_size." + area));
    EXPECT_EQ(0, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
  }

  //
  // 2. Advertise key with long enough ttl, so that it doesn't expire
  // - Ensure we receive publication over pub socket
  // - Ensure we receive key-value via GET request
  //
  {
    auto thriftValue = value;
    thriftValue.ttl() = 50000;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    auto getRes = kvStore->getKey(kTestingAreaName, key);
    CO_ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl(), *getRes->ttl() + 1);
    getRes->ttl() = *thriftValue.ttl();
    getRes->hash() = 0;
    EXPECT_EQ(thriftValue, getRes.value());

    // dump keys
    auto dumpRes = kvStore->dumpAll(kTestingAreaName);
    EXPECT_EQ(1, dumpRes.size());
    CO_ASSERT_EQ(1, dumpRes.count(key));
    auto& dumpResValue = dumpRes.at(key);
    EXPECT_GE(*thriftValue.ttl(), *dumpResValue.ttl() + 1);
    dumpResValue.ttl() = *thriftValue.ttl();
    dumpResValue.hash() = 0;
    EXPECT_EQ(thriftValue, dumpResValue);

    // dump hashes
    auto hashRes = co_await kvStore->dumpHashes(kTestingAreaName);
    EXPECT_EQ(1, hashRes.size());
    CO_ASSERT_EQ(1, hashRes.count(key));
    auto& hashResValue = hashRes.at(key);
    EXPECT_GE(*thriftValue.ttl(), *hashResValue.ttl() + 1);
    hashResValue.ttl() = *thriftValue.ttl();
    hashResValue.hash() = 0;
    hashResValue.value().copy_from(thriftValue.value());
    EXPECT_EQ(thriftValue, hashResValue);

    // We will receive key-advertisement
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals()->size());
    CO_ASSERT_EQ(0, publication.expiredKeys()->size());
    CO_ASSERT_EQ(1, publication.keyVals()->count(key));
    auto& pubValue = publication.keyVals()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_GE(*thriftValue.ttl(), *pubValue.ttl() + 1);
    pubValue.ttl() = *thriftValue.ttl();
    pubValue.hash() = 0;
    EXPECT_EQ(thriftValue, pubValue);

    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_queue_size." + area));
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
  }

  //
  // 3. Advertise ttl-update to set it to new value
  //
  {
    auto thriftValue = value;
    thriftValue.value().reset();
    thriftValue.ttl() = 30000;
    thriftValue.ttlVersion() = *thriftValue.ttlVersion() + 1;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    auto getRes = kvStore->getKey(kTestingAreaName, key);
    CO_ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl(), *getRes->ttl() + 1);
    EXPECT_EQ(*thriftValue.version(), *getRes->version());
    EXPECT_EQ(*thriftValue.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
    EXPECT_EQ(value.value(), getRes->value());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals()->size());
    CO_ASSERT_EQ(0, publication.expiredKeys()->size());
    CO_ASSERT_EQ(1, publication.keyVals()->count(key));
    auto& pubValue = publication.keyVals()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_FALSE(pubValue.value().has_value());
    EXPECT_GE(*thriftValue.ttl(), *pubValue.ttl() + 1);
    EXPECT_EQ(*thriftValue.version(), *pubValue.version());
    EXPECT_EQ(*thriftValue.originatorId(), *pubValue.originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *pubValue.ttlVersion());

    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_queue_size." + area));
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
  }

  //
  // 4. Set ttl of key to INFINITE
  //
  {
    auto thriftValue = value;
    thriftValue.value().reset();
    thriftValue.ttl() = Constants::kTtlInfinity;
    thriftValue.ttlVersion() = *thriftValue.ttlVersion() + 2;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    // ttl should remain infinite
    auto getRes = kvStore->getKey(kTestingAreaName, key);
    CO_ASSERT_TRUE(getRes.has_value());
    EXPECT_EQ(Constants::kTtlInfinity, *getRes->ttl());
    EXPECT_EQ(*thriftValue.version(), *getRes->version());
    EXPECT_EQ(*thriftValue.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
    EXPECT_EQ(value.value(), getRes->value());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals()->size());
    CO_ASSERT_EQ(0, publication.expiredKeys()->size());
    CO_ASSERT_EQ(1, publication.keyVals()->count(key));
    auto& pubValue = publication.keyVals()->at(key);
    // TTL should remain infinite
    EXPECT_FALSE(pubValue.value().has_value());
    EXPECT_EQ(Constants::kTtlInfinity, *pubValue.ttl());
    EXPECT_EQ(*thriftValue.version(), *pubValue.version());
    EXPECT_EQ(*thriftValue.originatorId(), *pubValue.originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *pubValue.ttlVersion());

    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_queue_size." + area));
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
  }

  //
  // 5. Set ttl of key back to a fixed value
  //
  {
    auto thriftValue = value;
    thriftValue.value().reset();
    thriftValue.ttl() = 20000;
    thriftValue.ttlVersion() = *thriftValue.ttlVersion() + 3;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    auto getRes = kvStore->getKey(kTestingAreaName, key);
    CO_ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl(), *getRes->ttl() + 1);
    EXPECT_EQ(*thriftValue.version(), *getRes->version());
    EXPECT_EQ(*thriftValue.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
    EXPECT_EQ(value.value(), getRes->value());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals()->size());
    CO_ASSERT_EQ(0, publication.expiredKeys()->size());
    CO_ASSERT_EQ(1, publication.keyVals()->count(key));
    auto& pubValue = publication.keyVals()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_FALSE(pubValue.value().has_value());
    EXPECT_GE(*thriftValue.ttl(), *pubValue.ttl() + 1);
    EXPECT_EQ(*thriftValue.version(), *pubValue.version());
    EXPECT_EQ(*thriftValue.originatorId(), *pubValue.originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *pubValue.ttlVersion());

    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_queue_size." + area));
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
  }

  //
  // 6. Apply old ttl update and see no effect
  //
  {
    auto thriftValue = value;
    thriftValue.value().reset();
    thriftValue.ttl() = 10000;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    auto getRes = kvStore->getKey(kTestingAreaName, key);
    CO_ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(20000, *getRes->ttl()); // Previous ttl was set to 20s
    EXPECT_LE(10000, *getRes->ttl());
    EXPECT_EQ(*value.version(), *getRes->version());
    EXPECT_EQ(*value.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*value.ttlVersion() + 3, *getRes->ttlVersion());
    EXPECT_EQ(value.value(), getRes->value());

    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_queue_size." + area));
    EXPECT_EQ(1, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
  }

  //
  // 7. Add new (key, originatorId) combinations and verify queue size
  //  - ensure that when we re-set a (key, originatorId) combination with a new
  //  ttlVersion, size of queue does not increase
  //
  {
    // Set new key test-key2
    thrift::Value thriftVal2 = createThriftValue(
        1 /* version */,
        "node1" /* originatorId */,
        std::string("value2") /* value */,
        100000 /* ttl */,
        1 /* ttl version */,
        0 /* hash */);
    kvStore->setKey(kTestingAreaName, "test-key2", thriftVal2);

    // Set same key test-key2 with a different originatorId
    thrift::Value thriftVal3 = createThriftValue(
        1 /* version */,
        "node2" /* originatorId */,
        std::string("value2") /* value */,
        100000 /* ttl */,
        1 /* ttl version */,
        0 /* hash */);
    kvStore->setKey(kTestingAreaName, "test-key2", thriftVal3);

    // Set same key test-key2 with same originatorId but different ttl version
    thrift::Value thriftVal4 = createThriftValue(
        1 /* version */,
        "node2" /* originatorId */,
        std::string("value2") /* value */,
        10000 /* ttl */,
        2 /* ttl version */,
        0 /* hash */);
    kvStore->setKey(kTestingAreaName, "test-key2", thriftVal4);
    auto getRes = kvStore->getKey(kTestingAreaName, "test-key2");
    CO_ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(10000, *getRes->ttl()); // Previous ttl was set to 10s
    EXPECT_LE(5000, *getRes->ttl());
    EXPECT_EQ(*thriftVal4.version(), *getRes->version());
    EXPECT_EQ(*thriftVal4.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*thriftVal4.ttlVersion(), *getRes->ttlVersion());

    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(3, counters.at("kvstore.ttl_countdown_queue_size." + area));
    EXPECT_EQ(3, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
  }
}

/**
 * When we first set a key with a finite TTL, then set the same key with a
 * infinite TTL. ttlCountdownQueue_ will only store an entry for finite TTLs. We
 * want to verify that once the finite TTL gets erased, this does not affect
 * kvStore_
 * Test Scenario:
 * 1. Create a key with a short finite TTL
 * 2. Set the same key to be infinite TTL
 * 3. Wait until finite TTL key should have expired
 * 4. Verify key has not been improperly removed
 * 4.5. Verify ttlCountdownQueue_ is empty
 */
TEST_F(KvStoreTestFixture, TtlInfiniteExpiry) {
  const std::string key{"dummyKey"};
  const auto value = createThriftValue(
      1, /* version */
      "node1", /* node id */
      "dummyValue",
      2000, /* ttl */
      1 /* ttl version */,
      0 /* hash */);

  auto kvStore = createKvStore(getTestKvConf("test"));
  kvStore->run();

  // Step 1: Create key with short finite TTL
  {
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, value));
    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(2000, *getRes->ttl());
  }
  // Step 2: Set same key with infinite TTL
  {
    auto thriftValue = value;
    thriftValue.ttl() = Constants::kTtlInfinity;
    thriftValue.ttlVersion() = *thriftValue.ttlVersion() + 1;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));
    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_EQ(Constants::kTtlInfinity, *getRes->ttl());
    EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
  }
  folly::EventBase testEvb;
  // Step 3: Wait until finite TTL key should have expired
  testEvb.scheduleAt(
      [&]() noexcept {
        // Step 4: Verify key has not been improperly removed
        auto getRes = kvStore->getKey(kTestingAreaName, key);
        ASSERT_TRUE(getRes.has_value());
        EXPECT_EQ(Constants::kTtlInfinity, *getRes->ttl());
        EXPECT_EQ(*value.ttlVersion() + 1, *getRes->ttlVersion());

        // Step 4.5: Verify ttlCountdownQueue_ is empty
        auto counters = fb303::fbData->getCounters();
        const std::string& area = kTestingAreaName;
        EXPECT_EQ(0, counters.at("kvstore.ttl_countdown_queue_size." + area));
        EXPECT_EQ(
            0, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
      },
      std::chrono::steady_clock::now() + std::chrono::seconds(3));
  testEvb.loop();
}

/**
 * T226400553: Set a key with a long finite TTL, then set the same key with a
 * small finite TTL. We want to verify that once the update key with the small
 * ttl expires, the key is erased rather than replaced with the old long finite
 * ttl.
 * Test Scenario:
 * 1. Create a key with a long finite TTL
 * 2. Set the same key to be short finite TTL
 * 3. Wait until the updated finite TTL key expires
 * 4. Verify key has been deleted and not updated to the stale key
 * 5. Add the same key with a TTL and verify contents
 */
TEST_F(KvStoreTestFixture, TtlEraseExpiry) {
  const std::string key{"staleKey"};
  const auto value = createThriftValue(
      1, /* version */
      "node1", /* node id */
      "dummyValue",
      2592000000, /* ttl of 30 days */
      1 /* ttl version */,
      0 /* hash */);

  auto kvStore = createKvStore(getTestKvConf("test"));
  kvStore->run();

  // Step 1: Create key with long finite TTL
  {
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, value));
    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(2592000000, *getRes->ttl());
  }
  // Step 2: Set same key with short finite TTL
  {
    auto thriftValue = value;
    thriftValue.ttl() = 1000;
    thriftValue.ttlVersion() = *thriftValue.ttlVersion() + 1;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));
    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(1000, *getRes->ttl());
    EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
  }
  folly::EventBase testEvb;
  // Step 3: Wait until short finite TTL key should have expired
  testEvb.scheduleAt(
      [&]() noexcept {
        // Step 4: Verify key has been removed
        auto getRes = kvStore->getKey(kTestingAreaName, key);
        ASSERT_FALSE(getRes.has_value());
        auto counters = fb303::fbData->getCounters();
        const std::string& area = kTestingAreaName;
        EXPECT_EQ(0, counters.at("kvstore.ttl_countdown_queue_size." + area));
        EXPECT_EQ(
            0, counters.at("kvstore.ttl_countdown_handle_map_size." + area));
      },
      std::chrono::steady_clock::now() + std::chrono::seconds(2));

  // Step 5: Readd key with ttl and verify contents
  testEvb.scheduleAt(
      [&]() noexcept {
        auto thriftValue = value;
        thriftValue.ttl() = 100000;
        thriftValue.ttlVersion() = *thriftValue.ttlVersion() + 2;
        EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));
        auto getRes = kvStore->getKey(kTestingAreaName, key);
        ASSERT_TRUE(getRes.has_value());
        EXPECT_GE(100000, *getRes->ttl());
        EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
      },
      std::chrono::steady_clock::now() + std::chrono::seconds(4));
  testEvb.loop();
}

/**
 * Test
 * -  when KvStore peers are synced, TTL of keys are sent with remaining
 * time to expire,
 * - when keys are received with some TTL, TTL for
 * existing local keys is not updated.
 *
 * Test Scenario:
 * 1. Start store0,
 * 2. Add two keys to store0,
 * 3. Sleep for 200msec,
 * 4. Start store1 and add one of the keys
 * 5  Sync with keys from store0
 * 6. Wait for KvStores to sync.
 * 7. Check store1 adds a new key with TTL equal to [default value - 200msec]
 * 8. Check TTL for existing key in store1 does not get updated
 */
TEST_F(KvStoreTestFixture, PeerSyncTtlExpiry) {
  auto store0 = createKvStore(getTestKvConf("store0"));
  auto store1 = createKvStore(getTestKvConf("store1"));
  store0->run();
  store1->run();

  auto thriftVal1 = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      "value1" /* value */,
      kTtlMs /* ttl */,
      0 /* ttl version */,
      0 /* hash */);

  thriftVal1.hash() = generateHash(
      *thriftVal1.version(), *thriftVal1.originatorId(), thriftVal1.value());

  auto thriftVal2 = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      "value2" /* value */,
      kTtlMs /* ttl */,
      0 /* ttl version */,
      0 /* hash */);

  thriftVal2.hash() = generateHash(
      *thriftVal2.version(), *thriftVal2.originatorId(), thriftVal2.value());

  EXPECT_TRUE(store0->setKey(kTestingAreaName, "test1", thriftVal1));
  auto maybeThriftVal = store0->getKey(kTestingAreaName, "test1");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs, *maybeThriftVal->ttl());
  maybeThriftVal->ttl() = kTtlMs;
  EXPECT_EQ(thriftVal1, *maybeThriftVal);

  EXPECT_TRUE(store0->setKey(kTestingAreaName, "test2", thriftVal2));
  maybeThriftVal = store0->getKey(kTestingAreaName, "test2");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs, *maybeThriftVal->ttl());
  maybeThriftVal->ttl() = kTtlMs;
  EXPECT_EQ(thriftVal2, *maybeThriftVal);

  EXPECT_TRUE(store1->setKey(kTestingAreaName, "test2", thriftVal2));
  maybeThriftVal = store1->getKey(kTestingAreaName, "test2");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs, *maybeThriftVal->ttl());
  maybeThriftVal->ttl() = kTtlMs;
  EXPECT_EQ(thriftVal2, *maybeThriftVal);
  // sleep override
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(store1->addPeer(
      kTestingAreaName, store0->getNodeId(), store0->getPeerSpec()));

  waitForAllPeersInitialized();

  // key 'test1' should be added with remaining TTL
  maybeThriftVal = store1->getKey(kTestingAreaName, "test1");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs - 200, *maybeThriftVal.value().ttl());

  // key 'test2' should not be updated, it should have kTtlMs
  maybeThriftVal = store1->getKey(kTestingAreaName, "test2");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs, *maybeThriftVal.value().ttl());
}

/**
 * Test to verify PEER_ADD/PEER_DEL and verify that keys are synchronized
 * to the neighbor.
 *
 * Topology:
 *
 *      store0
 *        ^
 *   _____|_____
 *   |         |
 * store1   store2
 *
 * 1. Advertise keys in store1 and store2;
 * 2. Verify that k-v appear in store0(happen via flooding);
 * 3. Update keys in store0;
 * 4. Verify that k-v NOT showing up in neither store1 and store2
 * 5. Update store1's peer definition for store0
 * 6. Verify that k-v injected in 3) shows up in store1(
 *    i.e. can only happen via full-sync)
 * 7. Verify PEER_DEL API
 */
TEST_F(KvStoreTestFixture, PeerAddUpdateRemove) {
  // Start stores in their respective threads.
  auto store0 = createKvStore(getTestKvConf("store0"));
  auto store1 = createKvStore(getTestKvConf("store1"));
  auto store2 = createKvStore(getTestKvConf("store2"));
  const std::string key{"key"};

  store0->run();
  store1->run();
  store2->run();

  EXPECT_TRUE(store1->addPeer(
      kTestingAreaName, store0->getNodeId(), store0->getPeerSpec()));
  EXPECT_TRUE(store2->addPeer(
      kTestingAreaName, store0->getNodeId(), store0->getPeerSpec()));

  // wait for full-sync
  waitForAllPeersInitialized();

  // map of peers we expect and dump peers to expect the results.
  auto store0NodeId = store0->getNodeId();
  auto peerSpec0 = store0->getPeerSpec(thrift::KvStorePeerState::INITIALIZED);
  folly::F14FastMap<std::string, thrift::PeerSpec> expectedPeers = {
      {store0NodeId, peerSpec0},
  };

  auto cmpPeers = store1->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());

  cmpPeers = store2->getPeers(kTestingAreaName);
  EXPECT_EQ(1, cmpPeers.size());

  EXPECT_EQ(*cmpPeers[store0NodeId].peerAddr(), *peerSpec0.peerAddr());
  EXPECT_EQ(*cmpPeers[store0NodeId].ctrlPort(), *peerSpec0.ctrlPort());
  EXPECT_EQ(*cmpPeers[store0NodeId].state(), *peerSpec0.state());
  EXPECT_LT(*cmpPeers[store0NodeId].stateElapsedTimeMs(), 5000);
  EXPECT_EQ(*cmpPeers[store0NodeId].flaps(), 0);

  //
  // Step 1) and 2): advertise key from store1/store2 and verify
  //
  {
    auto thriftVal = createThriftValue(
        1 /* version */, "1.2.3.4" /* originatorId */, "value1" /* value */
    );
    EXPECT_TRUE(store1->setKey(kTestingAreaName, key, thriftVal));
    // Update hash
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());

    // Receive publication from store0 for new key-update
    auto pub = store0->recvPublication();
    EXPECT_EQ(1, pub.keyVals()->size());
    EXPECT_EQ(thriftVal, pub.keyVals()[key]);
  }

  // Now play the same trick with the other store
  {
    auto thriftVal = createThriftValue(
        2 /* version */, "1.2.3.4" /* originatorId */, "value2" /* value */
    );
    EXPECT_TRUE(store2->setKey(kTestingAreaName, key, thriftVal));
    // Update hash
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());

    // Receive publication from store0 for new key-update
    auto pub = store0->recvPublication();
    EXPECT_EQ(1, pub.keyVals()->size());
    EXPECT_EQ(thriftVal, pub.keyVals()[key]);
  }

  //
  // Step 3) and 4): advertise from store0 and verify
  //
  {
    auto thriftVal = createThriftValue(
        3 /* version */, "1.2.3.4" /* originatorId */, "value3" /* value */
    );
    EXPECT_TRUE(store0->setKey(kTestingAreaName, key, thriftVal));
    // Update hash
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());

    // store1/store2 should NOT have the key since no peer to flood
    auto maybeVal1 = store1->getKey(kTestingAreaName, key);
    CHECK(maybeVal1.has_value());
    auto maybeVal2 = store2->getKey(kTestingAreaName, key);
    CHECK(maybeVal2.has_value());
    EXPECT_NE(3, *maybeVal1.value().version());
    EXPECT_NE(3, *maybeVal2.value().version());
  }

  //
  // Step 5) and 6): update store1 with same peer spec of store0
  //
  // TODO: test failed under OSS build env when thrift client is
  // desctructed and recreated with the SAME (address, port)
  //
  // T101564784 to track and investigate
  {
    /*
  EXPECT_TRUE(store1->addPeer(
    kTestingAreaName, store0->getNodeId(), store0->getPeerSpec()));

  // wait for full-sync
  waitForAllPeersInitialized();
  EXPECT_EQ(expectedPeers, store1->getPeers(kTestingAreaName));

  // store1 should have key update(full-sync with peer_spec change)
  auto maybeVal1 = store1->getKey(kTestingAreaName, key);
  CHECK(maybeVal1.has_value());
  EXPECT_EQ(3, *maybeVal1.value().version());

  // store2 still NOT updated since there is no full-sync
  auto maybeVal = store2->getKey(kTestingAreaName, key);
  CHECK(maybeVal.has_value());
  EXPECT_NE(3, *maybeVal.value().version());
    */
  }

  // Remove store0 and verify
  {
    expectedPeers.clear();
    store1->delPeer(kTestingAreaName, store0->getNodeId());
    store2->delPeer(kTestingAreaName, store0->getNodeId());

    EXPECT_EQ(expectedPeers, store1->getPeers(kTestingAreaName));
    EXPECT_EQ(expectedPeers, store2->getPeers(kTestingAreaName));
  }
}

/**
 * Start single testable store, and make it sync with N other stores. We only
 * rely on pub-sub and sync logic on a single store to do all the work.
 *
 * Also verify behavior of new flooding.
 */
TEST_F(KvStoreTestFixture, BasicSync) {
  const std::string kOriginBase = "peer-store-";
  const unsigned int kNumStores = 16;

  // Create and start peer-stores
  std::vector<KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>*>
      peerStores;
  for (unsigned int j = 0; j < kNumStores; ++j) {
    auto nodeId = getNodeId(kOriginBase, j);
    auto store = createKvStore(getTestKvConf(nodeId));
    store->run();
    peerStores.push_back(store);
  }

  // Submit initial value set into all peerStores
  folly::F14FastMap<std::string, thrift::Value> expectedKeyVals;
  LOG(INFO) << "Submitting initial key-value pairs into peer stores.";
  for (auto& store : peerStores) {
    auto key = fmt::format("test-key-{}", store->getNodeId());
    auto thriftVal = createThriftValue(
        1 /* version */,
        "gotham_city" /* originatorId */,
        fmt::format("test-value-{}", store->getNodeId()),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);

    // Submit the key-value to store
    store->setKey(kTestingAreaName, key, thriftVal);
    // Update hash
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());

    // Store this in expectedKeyVals
    expectedKeyVals[key] = thriftVal;
  }

  LOG(INFO) << "Starting store under test";

  // set up the store that we'll be testing
  messaging::ReplicateQueue<PeerEvent> myPeerUpdatesQueue;
  auto myStore = createKvStore(
      getTestKvConf(getNodeId(kOriginBase, kNumStores)),
      {kTestingAreaName} /* areas */,
      myPeerUpdatesQueue.getReader());
  myStore->run();

  // NOTE: It is important to add peers after starting our store to avoid
  // race condition where certain updates are lost over PUB/SUB channel
  thrift::PeersMap myPeers;
  for (auto& store : peerStores) {
    myPeers.emplace(store->getNodeId(), store->getPeerSpec());
    store->addPeer(
        kTestingAreaName, myStore->getNodeId(), myStore->getPeerSpec());
  }

  // Push peer event to myPeerUpdatesQueue.
  PeerEvent myPeerEvent{
      {kTestingAreaName, AreaPeerEvent(myPeers, {} /*peersToDel*/)}};
  myPeerUpdatesQueue.push(myPeerEvent);

  // Wait for full-sync to complete. Full-sync is complete when all of our
  // neighbors receive all the keys and we must receive `kNumStores`
  // key-value updates from each store over PUB socket.
  LOG(INFO) << "Waiting for full sync to complete.";
  for (auto& store : stores_) {
    folly::F14FastSet<std::string> keys;
    XLOGF(DBG3, "Store {} received keys.", store->getNodeId());
    while (keys.size() < kNumStores) {
      auto publication = store->recvPublication();
      for (auto const& [key, val] : *publication.keyVals()) {
        XLOGF(DBG3, "\tkey: {}, value: {}", key, val.value().value());
        keys.insert(key);
      }
    }
  }

  // Expect myStore publishing KVSTORE_SYNCED after initial KvStoreDb sync with
  // peers.
  myStore->recvKvStoreSyncedSignal();

  // Verify myStore database
  EXPECT_EQ(expectedKeyVals, myStore->dumpAll(kTestingAreaName));

  //
  // Submit another range of values
  //
  LOG(INFO) << "Submitting the second round of key-values...";
  for (auto& store : peerStores) {
    auto key = fmt::format("test-key-{}", store->getNodeId());
    auto thriftVal = createThriftValue(
        2 /* version */,
        "gotham_city" /* originatorId */,
        fmt::format("test-value-new-{}", store->getNodeId()),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);

    // Submit the key-value to store
    store->setKey(kTestingAreaName, key, thriftVal);
    // Update hash
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());

    // Store this in expectedKeyVals
    expectedKeyVals[key] = thriftVal;
  }

  // Wait again for the full sync to complete. Full-sync is complete when all
  // of our neighbors receive all the keys and we must receive `kNumStores`
  // key-value updates from each store over PUB socket.
  LOG(INFO) << "waiting for another full sync to complete...";
  // Receive 16 updates from each store
  for (auto& store : stores_) {
    folly::F14FastSet<std::string> keys;
    XLOGF(DBG3, "Store {} received keys.", store->getNodeId());
    while (keys.size() < kNumStores) {
      auto publication = store->recvPublication();
      for (auto const& [key, val] : *publication.keyVals()) {
        XLOGF(DBG3, "\tkey: {}, value: {}", key, val.value().value());
        keys.insert(key);
      }
    }
  }

  // Verify our database and all neighbor database
  EXPECT_EQ(expectedKeyVals, myStore->dumpAll(kTestingAreaName));

  //
  // Update key in peerStore[0] and verify flooding behavior
  // Invariant => Sent publication to a neighbor never reflects back
  // - Only one publication and key_vals is received in all stores
  // - Only one publication and key_vals is updated in all stores
  // - Only one publication, key_vals is sent out of peerStore[0]
  // - Exactly 15 publications, key_vals is sent out of myStore
  //   (15 peers except originator)
  // - No publication or key_vals is sent out of peerStores except peerStore[0]
  //
  LOG(INFO) << "Testing flooding behavior";

  // Get current counters
  auto oldCounters = fb303::fbData->getCounters();

  // Set new key
  {
    auto& store = peerStores[0];
    auto key = fmt::format("flood-test-key-{}", store->getNodeId());
    auto thriftVal = createThriftValue(
        2 /* version */,
        "gotham_city" /* originatorId */,
        fmt::format("flood-test-value-{}", store->getNodeId()),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);

    // Submit the key-value to store
    LOG(INFO) << "Setting key in peerStores[0]";
    store->setKey(kTestingAreaName, key, thriftVal);
  }

  // let kvstore sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // Receive publication from each store as one update is atleast expected
  {
    for (auto& store : stores_) {
      XLOGF(DBG2, "Receiving publication from {}", store->getNodeId());
      store->recvPublication();
    }
  }

  // Get new counters
  LOG(INFO) << "Getting counters snapshot";
  auto newCounters = fb303::fbData->getCounters();

  // Verify counters
  LOG(INFO) << "Verifying global counters for 16 stores";
  EXPECT_LE(
      oldCounters["kvstore.received_publications.count"] + 17,
      newCounters["kvstore.received_publications.count"]);
  EXPECT_LE(
      oldCounters["kvstore.received_key_vals.sum"] + 17,
      newCounters["kvstore.received_key_vals.sum"]);
  EXPECT_EQ(
      oldCounters["kvstore.updated_key_vals.sum"] + 17,
      newCounters["kvstore.updated_key_vals.sum"]);
  EXPECT_LE(
      oldCounters["kvstore.thrift.num_flood_pub.count"] + 16,
      newCounters["kvstore.thrift.num_flood_pub.count"]);
  EXPECT_LE(
      oldCounters["kvstore.thrift.num_flood_key_vals.sum"] + 16,
      newCounters["kvstore.thrift.num_flood_key_vals.sum"]);
}

/**
 * Make two stores race for the same key, and make sure tie-breaking is working
 * as expected. We do this by connecting N stores in a chain, and then
 * submitting different values at each end of the chain, with same version
 * numbers. We also try injecting lower version number to make sure it does not
 * overwrite anything.
 *
 * Also verify the publication propagation via nodeIds attribute
 */
TEST_F(KvStoreTestFixture, TieBreaking) {
  const std::string kOriginBase = "store";
  const unsigned int kNumStores = 16;
  const std::string kKeyName = "test-key";

  //
  // Start the intermediate stores in string topology
  //
  LOG(INFO) << "Preparing and starting stores.";
  std::vector<KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>*>
      stores;
  std::vector<std::string> nodeIdsSeq;
  for (unsigned int i = 0; i < kNumStores; ++i) {
    auto nodeId = getNodeId(kOriginBase, i);
    auto store = createKvStore(getTestKvConf(nodeId));
    LOG(INFO) << "Preparing store " << nodeId;
    store->run();
    stores.push_back(store);
    nodeIdsSeq.emplace_back(nodeId);
  }

  // Add neighbors to the nodes.
  LOG(INFO) << "Adding neighbors in chain topology.";
  for (unsigned int i = 0; i < kNumStores; ++i) {
    auto& store = stores[i];
    if (i > 0) {
      auto& peerStore = stores[i - 1];
      EXPECT_TRUE(store->addPeer(
          kTestingAreaName, peerStore->getNodeId(), peerStore->getPeerSpec()));
    }
    if (i < kNumStores - 1) {
      auto& peerStore = stores[i + 1];
      EXPECT_TRUE(store->addPeer(
          kTestingAreaName, peerStore->getNodeId(), peerStore->getPeerSpec()));
    }
    store->recvKvStoreSyncedSignal();
  }

  // need to wait on this for the list of nodeIds to be as expected.
  waitForAllPeersInitialized();

  //
  // Submit same key in store 0 and store N-1, use same version
  // but different values
  //
  LOG(INFO) << "Submitting key-values from first and last store";

  // set a key from first store
  auto thriftValFirst = createThriftValue(
      10 /* version */,
      "1" /* originatorId */,
      "test-value-1",
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  EXPECT_TRUE(stores[0]->setKey(kTestingAreaName, kKeyName, thriftValFirst));
  // Update hash
  thriftValFirst.hash() = generateHash(
      *thriftValFirst.version(),
      *thriftValFirst.originatorId(),
      thriftValFirst.value());

  // set a key from the store on the other end of the chain
  auto thriftValLast = createThriftValue(
      10 /* version */,
      "2" /* originatorId */,
      "test-value-2",
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  EXPECT_TRUE(
      stores[kNumStores - 1]->setKey(
          kTestingAreaName, kKeyName, thriftValLast));
  // Update hash
  thriftValLast.hash() = generateHash(
      *thriftValLast.version(),
      *thriftValLast.originatorId(),
      thriftValLast.value());

  //
  // We expect test-value-2 because "2" > "1" in tie-breaking
  //
  LOG(INFO) << "Pulling values from every store";

  // We have to wait until we see two updates on the first node and verify them.
  {
    auto pub1 = stores[0]->recvPublication();
    auto pub2 = stores[0]->recvPublication();
    ASSERT_EQ(1, pub1.keyVals()->count(kKeyName));
    ASSERT_EQ(1, pub2.keyVals()->count(kKeyName));
    EXPECT_EQ(thriftValFirst, pub1.keyVals()->at(kKeyName));
    EXPECT_EQ(thriftValLast, pub2.keyVals()->at(kKeyName));

    // Verify nodeIds attribute of publication
    ASSERT_TRUE(pub1.nodeIds().has_value());
    ASSERT_TRUE(pub2.nodeIds().has_value());
    EXPECT_EQ(
        std::vector<std::string>{stores[0]->getNodeId()},
        pub1.nodeIds().value());
    auto expectedNodeIds = nodeIdsSeq;
    std::reverse(std::begin(expectedNodeIds), std::end(expectedNodeIds));
    EXPECT_EQ(expectedNodeIds, pub2.nodeIds().value());
  }

  for (auto& store : stores) {
    LOG(INFO) << "Pulling state from " << store->getNodeId();
    auto maybeThriftVal = store->getKey(kTestingAreaName, kKeyName);
    ASSERT_TRUE(maybeThriftVal.has_value());
    EXPECT_EQ(thriftValLast, *maybeThriftVal);
  }

  //
  // Now submit the same key with LOWER version number
  //
  LOG(INFO) << "Submitting key-value from first server with lower version";

  // set a key from first store - notice we bumped originator to "9", but
  // it should not have any effect, since version is lower. It is sufficient
  // to verify changes on only first node.
  {
    auto thriftVal = createThriftValue(
        9 /* version */,
        "9" /* originatorId */,
        "test-value-1",
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
    EXPECT_TRUE(stores[0]->setKey(kTestingAreaName, kKeyName, thriftVal));
    // Update hash
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());

    // Make sure the old value still exists
    EXPECT_EQ(thriftValLast, stores[0]->getKey(kTestingAreaName, kKeyName));
  }
}

TEST_F(KvStoreTestFixture, DumpPrefix) {
  const std::string kOriginBase = "peer-store-";
  const unsigned int kNumStores = 16;

  // Create and start peer-stores
  std::vector<KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>*>
      peerStores;
  for (unsigned int j = 0; j < kNumStores; ++j) {
    auto store = createKvStore(getTestKvConf(getNodeId(kOriginBase, j)));
    store->run();
    peerStores.emplace_back(store);
  }

  // Submit initial value set into all peerStores
  LOG(INFO) << "Submitting initial key-value pairs into peer stores.";

  folly::F14FastMap<std::string, thrift::Value> expectedKeyVals;
  int index = 0;
  for (auto& store : peerStores) {
    auto key = fmt::format("{}-test-key-{}", index % 2, store->getNodeId());
    auto thriftVal = createThriftValue(
        1 /* version */,
        "gotham_city" /* originatorId */,
        fmt::format("test-value-{}", store->getNodeId()),
        Constants::kTtlInfinity /* ttl */);

    // Submit the key-value to store
    store->setKey(kTestingAreaName, key, thriftVal);

    // Update hash
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());

    // Store this in expectedKeyVals
    if (index % 2 == 0) {
      expectedKeyVals[key] = thriftVal;
    }
    ++index;
  }

  LOG(INFO) << "Starting store under test";

  // set up the extra KvStore that we'll be testing
  auto myStore =
      createKvStore(getTestKvConf(getNodeId(kOriginBase, kNumStores)));
  myStore->run();

  // NOTE: It is important to add peers after starting our store to avoid
  // race condition.
  for (auto& store : peerStores) {
    myStore->addPeer(
        kTestingAreaName, store->getNodeId(), store->getPeerSpec());
  }

  // Wait for full-sync to complete. Full-sync is complete when all of our
  // neighbors receive all the keys and we must receive `kNumStores`
  // key-value updates from each store over PUB socket.
  LOG(INFO) << "Waiting for full sync to complete.";
  {
    XLOGF(DBG3, "Store {} received keys.", myStore->getNodeId());

    folly::F14FastSet<std::string> keys;
    while (keys.size() < kNumStores) {
      auto publication = myStore->recvPublication();
      for (auto const& [key, val] : *publication.keyVals()) {
        XLOGF(DBG3, "\tkey: {}, value: {}", key, val.value().value());
        keys.insert(key);
      }
    }
  }

  // Verify myStore database. we only want keys with "0" prefix
  std::optional<KvStoreFilters> kvFilters{KvStoreFilters({"0"}, {})};
  EXPECT_EQ(
      expectedKeyVals,
      myStore->dumpAll(kTestingAreaName, std::move(kvFilters)));
}

/**
 * Start single testable store, and set key values.
 * Try to request for KEY_DUMP with a few keyValHashes.
 * We only supposed to see a dump of those keyVals on which either key is not
 * present in provided keyValHashes or hash differs.
 */
TEST_F(KvStoreTestFixture, DumpDifference) {
  // set up the store that we'll be testing
  auto myStore = createKvStore(getTestKvConf("test-node"));
  myStore->run();

  folly::F14FastMap<std::string, thrift::Value> expectedKeyVals;
  thrift::KeyVals peerKeyVals;
  folly::F14FastMap<std::string, thrift::Value> diffKeyVals;
  const folly::F14FastMap<std::string, thrift::Value> emptyKeyVals;
  for (int i = 0; i < 3; ++i) {
    const auto key = fmt::format("test-key-{}", i);
    auto thriftVal = createThriftValue(
        1 /* version */,
        "gotham_city" /* originatorId */,
        fmt::format("test-value-{}", myStore->getNodeId()),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);

    // Submit the key-value to myStore
    myStore->setKey(kTestingAreaName, key, thriftVal);

    // Update hash
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());

    // Store keyVals
    expectedKeyVals[key] = thriftVal;
    if (i == 0) {
      diffKeyVals[key] = thriftVal;
    } else {
      peerKeyVals[key] = thriftVal;
    }
  }

  // 0. Expect all keys
  EXPECT_EQ(expectedKeyVals, myStore->dumpAll(kTestingAreaName));

  // 1. Query missing keys (test-key-0 will be returned)
  EXPECT_EQ(diffKeyVals, myStore->syncKeyVals(kTestingAreaName, peerKeyVals));

  // Add missing key, test-key-0, into peerKeyVals
  const auto key = "test-key-0";
  const auto strVal = fmt::format("test-value-{}", myStore->getNodeId());
  const auto thriftVal = createThriftValue(
      1 /* version */,
      "gotham_city" /* originatorId */,
      strVal /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(1, "gotham_city", thrift::Value().value() = strVal));
  peerKeyVals[key] = thriftVal;

  // 2. Query with same snapshot. Expect no changes
  {
    EXPECT_EQ(
        emptyKeyVals, myStore->syncKeyVals(kTestingAreaName, peerKeyVals));
  }

  // 3. Query with different value (change value/hash of test-key-0)
  {
    auto newThriftVal = thriftVal;
    newThriftVal.value() = "why-so-serious";
    newThriftVal.hash() = generateHash(
        *newThriftVal.version(),
        *newThriftVal.originatorId(),
        newThriftVal.value());
    peerKeyVals[key] = newThriftVal; // extra key in local
    EXPECT_EQ(
        emptyKeyVals, myStore->syncKeyVals(kTestingAreaName, peerKeyVals));
  }

  // 3. Query with different originatorID (change originatorID of test-key-0)
  {
    auto newThriftVal = thriftVal;
    *newThriftVal.originatorId() = "gotham_city_1";
    peerKeyVals[key] = newThriftVal; // better orginatorId in local
    EXPECT_EQ(
        emptyKeyVals, myStore->syncKeyVals(kTestingAreaName, peerKeyVals));
  }

  // 4. Query with different ttlVersion (change ttlVersion of test-key-1)
  {
    auto newThriftVal = thriftVal;
    newThriftVal.ttlVersion() = 0xb007;
    peerKeyVals[key] = newThriftVal; // better ttlVersion in local
    EXPECT_EQ(
        emptyKeyVals, myStore->syncKeyVals(kTestingAreaName, peerKeyVals));
  }
}

/*
 * check key value is decremented with the TTL decrement value provided,
 * and is not synced if remaining TTL is < TTL decrement value provided
 */
TEST_F(KvStoreTestFixture, TtlDecrementValue) {
  auto store1Conf = getTestKvConf("store1");
  store1Conf.ttl_decrement_ms() = 300;

  auto store0 = createKvStore(getTestKvConf("store0"));
  auto store1 = createKvStore(store1Conf);
  store0->run();
  store1->run();

  store0->addPeer(kTestingAreaName, store1->getNodeId(), store1->getPeerSpec());
  store1->addPeer(kTestingAreaName, store0->getNodeId(), store0->getPeerSpec());
  store0->recvKvStoreSyncedSignal();
  store1->recvKvStoreSyncedSignal();

  /**
   * check sync works fine, add a key with TTL > ttlDecr in store1,
   * verify key is synced to store0
   */
  int64_t ttl1 = 6000;
  auto thriftVal1 = createThriftValue(
      1 /* version */,
      "utest" /* originatorId */,
      "value" /* value */,
      ttl1 /* ttl */,
      1 /* ttl version */,
      0 /* hash */);
  thriftVal1.hash() = generateHash(
      *thriftVal1.version(), *thriftVal1.originatorId(), thriftVal1.value());
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "key1", thriftVal1));
  {
    /* check key is in store1 */
    auto getRes1 = store1->getKey(kTestingAreaName, "key1");
    ASSERT_TRUE(getRes1.has_value());

    /* check key synced from store1 has ttl that is reduced by ttlDecr. */
    auto getPub0 = store0->recvPublication();
    ASSERT_EQ(1, getPub0.keyVals()->count("key1"));
    EXPECT_LE(
        *getPub0.keyVals()->at("key1").ttl(),
        ttl1 - *store1Conf.ttl_decrement_ms());
  }

  /* Add another key with TTL < ttlDecr, and check it's not synced */
  int64_t ttl2 = *store1Conf.ttl_decrement_ms() - 1;
  auto thriftVal2 = createThriftValue(
      1 /* version */,
      "utest" /* originatorId */,
      "value" /* value */,
      ttl2 /* ttl */,
      1 /* ttl version */,
      0 /* hash */);
  thriftVal2.hash() = generateHash(
      *thriftVal2.version(), *thriftVal2.originatorId(), thriftVal2.value());
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "key2", thriftVal2));

  {
    /* check key get returns false from both store0 and store1 */
    auto getRes0 = store0->getKey(kTestingAreaName, "key2");
    ASSERT_FALSE(getRes0.has_value());
    auto getRes1 = store1->getKey(kTestingAreaName, "key2");
    ASSERT_FALSE(getRes1.has_value());
  }
}

/**
 * Test kvstore-consistency with flooding rate-limiter enabled
 * linear topology, intentionlly increate db-sync interval from 1s -> 60s so
 * we can check kvstore is synced without replying on periodic peer-sync.
 * s0 -- s1 (rate-limited) -- s2
 * let s0 set ONLY one key, while s2 sets thousands of keys within 5 seconds.
 * Make sure all stores have same amount of keys at the end
 */
TEST_F(KvStoreTestFixture, RateLimiterFlood) {
  auto rateLimitConf = getTestKvConf("store1");
  const size_t messageRate{10}, burstSize{50};
  auto floodRate = createKvStoreFloodRate(
      messageRate /*flood_msg_per_sec*/, burstSize /*flood_msg_burst_size*/);
  rateLimitConf.flood_rate() = floodRate;

  auto store0 = createKvStore(getTestKvConf("store0"));
  auto store1 = createKvStore(rateLimitConf);
  auto store2 = createKvStore(getTestKvConf("store2"));

  store0->run();
  store1->run();
  store2->run();

  store0->addPeer(kTestingAreaName, store1->getNodeId(), store1->getPeerSpec());
  store1->addPeer(kTestingAreaName, store0->getNodeId(), store0->getPeerSpec());

  store1->addPeer(kTestingAreaName, store2->getNodeId(), store2->getPeerSpec());
  store2->addPeer(kTestingAreaName, store1->getNodeId(), store1->getPeerSpec());

  auto startTime1 = steady_clock::now();
  const int duration1 = 5; // in seconds
  int expectNumKeys{0};
  uint64_t elapsedTime1{0};
  do {
    auto thriftVal = createThriftValue(
        1 /* version */,
        "store1" /* originatorId */,
        "value" /* value */,
        300000 /* ttl */,
        1 /* ttl version */,
        0 /* hash */);
    std::string key = fmt::format("key{}", ++expectNumKeys);
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());
    if (expectNumKeys == 10) {
      // we should be able to set thousands of keys wihtin 5 seconds,
      // pick one of them and let it be set by store0, all others set by store2
      *thriftVal.originatorId() = "store0";
      EXPECT_TRUE(store0->setKey(kTestingAreaName, key, thriftVal));
    } else {
      *thriftVal.originatorId() = "store2";
      EXPECT_TRUE(store2->setKey(kTestingAreaName, key, thriftVal));
    }

    elapsedTime1 =
        duration_cast<seconds>(steady_clock::now() - startTime1).count();
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (elapsedTime1 < duration1);

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(5));
  auto kv0 = store0->dumpAll(kTestingAreaName);
  auto kv1 = store1->dumpAll(kTestingAreaName);
  auto kv2 = store2->dumpAll(kTestingAreaName);

  EXPECT_TRUE(kv0.count("key10"));
  EXPECT_TRUE(kv1.count("key10"));
  EXPECT_TRUE(kv2.count("key10"));
  EXPECT_EQ(expectNumKeys, kv0.size());
  EXPECT_EQ(expectNumKeys, kv1.size());
  EXPECT_EQ(expectNumKeys, kv2.size());
}

TEST_F(KvStoreTestFixture, RateLimiter) {
  fb303::fbData->resetAllData();

  const size_t messageRate{10}, burstSize{50};
  auto rateLimitConf = getTestKvConf("store1");
  auto floodRate = createKvStoreFloodRate(
      messageRate /*flood_msg_per_sec*/, burstSize /*flood_msg_burst_size*/);
  rateLimitConf.flood_rate() = floodRate;

  auto store0 = createKvStore(getTestKvConf("store0"));
  auto store1 = createKvStore(rateLimitConf);
  store0->run();
  store1->run();

  store0->addPeer(kTestingAreaName, store1->getNodeId(), store1->getPeerSpec());
  store1->addPeer(kTestingAreaName, store0->getNodeId(), store0->getPeerSpec());

  /**
   * TEST1: install several keys in store0 which is not rate limited
   * Check number of sent publications should be at least number of
   * key updates set
   */
  auto startTime1 = steady_clock::now();
  const int duration1 = 5; // in seconds
  int i1{0};
  uint64_t elapsedTime1{0};
  do {
    auto thriftVal = createThriftValue(
        1 /* version */,
        "store1" /* originatorId */,
        "value" /* value */,
        300000 /* ttl */,
        ++i1 /* ttl version */,
        0 /* hash */);
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());
    EXPECT_TRUE(store0->setKey(kTestingAreaName, "key1", thriftVal));
    elapsedTime1 =
        duration_cast<seconds>(steady_clock::now() - startTime1).count();
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (elapsedTime1 < duration1);

  // sleep to get tokens replenished since store1 also floods keys it receives
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(5));

  auto s0PubSent1 =
      fb303::fbData->getCounters()["kvstore.thrift.num_flood_pub.count"];

  // store0 is not rate limited, so it floods updates to store1. Under flood
  // memory pressure the per-area byte budget may coalesce/defer flooding, so
  // the number of flood publications is not necessarily one-per-update. Assert
  // that flooding happened and that the latest value converged to store1.
  EXPECT_GE(s0PubSent1, 1);
  auto s1Key1 = store1->getKey(kTestingAreaName, "key1");
  ASSERT_TRUE(s1Key1.has_value());
  EXPECT_EQ(i1, *s1Key1->ttlVersion());
  /**
   * TEST2: install several keys in store1 which is rate limited. Number of
   * pulications sent should be (duration * messageRate). e.g. if duration
   * is 5 secs, and message Rate is 20 msgs/sec, max number of publications
   * sent should be 5*20 = 100 msgs.
   *
   * Also verify the last key set was sent to store0 by checking ttl version
   */
  auto startTime2 = steady_clock::now();
  const int duration2 = 5; // in seconds
  const int wait = 5; // in seconds
  int i2{0};
  uint64_t elapsedTime2{0};
  fb303::fbData->resetAllData();
  do {
    auto thriftVal = createThriftValue(
        1 /* version */,
        "store1" /* originatorId */,
        "value" /* value */,
        300000 /* ttl */,
        ++i2 /* ttl version */,
        0 /* hash */);
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());
    EXPECT_TRUE(store1->setKey(kTestingAreaName, "key2", thriftVal));
    elapsedTime2 =
        duration_cast<seconds>(steady_clock::now() - startTime2).count();
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (elapsedTime2 < duration2);

  // wait pending updates
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(wait));
  // check in store0 the ttl version, this should be the same as latest version
  auto getRes = store0->getKey(kTestingAreaName, "key2");
  ASSERT_TRUE(getRes.has_value());
  EXPECT_EQ(i2, *getRes->ttlVersion());

  auto allCounters = fb303::fbData->getCounters();
  auto s1PubSent2 = allCounters["kvstore.thrift.num_flood_pub.count"];
  auto s0KeyNum2 = store0->dumpAll(kTestingAreaName).size();

  // number of messages sent must be around duration * messageRate
  // +3 as some messages could have been sent after the counter
  EXPECT_LT(s1PubSent2, (duration2 + wait + 3) * messageRate);

  /**
   * TEST3: similar to TEST2, except instead of key ttl version, new keys
   * are inserted. Some updates will be supressed and merged into a single
   * publication. Verify that all keys changes are published.
   */
  auto startTime3 = steady_clock::now();
  const int duration3 = 5; // in seconds
  int i3{0};
  uint64_t elapsedTime3{0};
  do {
    auto key = fmt::format("key3{}", ++i3);
    auto thriftVal = createThriftValue(
        1 /* version */,
        "store1" /* originatorId */,
        "value" /* value */,
        300000 /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());
    EXPECT_TRUE(store1->setKey(kTestingAreaName, key, thriftVal));
    elapsedTime3 =
        duration_cast<seconds>(steady_clock::now() - startTime3).count();

    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (elapsedTime3 < duration3);

  // wait pending updates
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(wait));

  allCounters = fb303::fbData->getCounters();
  auto s1PubSent3 = allCounters["kvstore.thrift.num_flood_pub.count"];
  auto s1Supressed3 = allCounters["kvstore.rate_limit_suppress.count"];

  // number of messages sent must be around duration * messageRate
  // +3 as some messages could have been sent after the counter
  EXPECT_LE(s1PubSent3 - s1PubSent2, (duration3 + wait + 3) * messageRate);

  // check for number of keys in store0 should be equal to number of keys
  // added in store1.
  auto s0KeyNum3 = store0->dumpAll(kTestingAreaName).size();
  EXPECT_EQ(s0KeyNum3 - s0KeyNum2, i3);

  /*
   * TEST4: Keys expiry test. Add new keys with low ttl, that are
   * subjected to rate limit. Verify all keys are expired
   */
  auto startTime4 = steady_clock::now();
  const int duration4 = 1; // in seconds
  int i4{0};
  uint64_t elapsedTime4{0};
  int64_t ttlLow = 50; // in msec
  do {
    auto key = fmt::format("key4{}", ++i4);
    auto thriftVal = createThriftValue(
        1 /* version */,
        "store1" /* originatorId */,
        "value" /* value */,
        ttlLow /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());
    EXPECT_TRUE(store1->setKey(kTestingAreaName, key, thriftVal));
    elapsedTime4 =
        duration_cast<seconds>(steady_clock::now() - startTime4).count();

    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (elapsedTime4 < duration4);

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(2 * ttlLow));

  allCounters = fb303::fbData->getCounters();
  auto s1Supressed4 = allCounters["kvstore.rate_limit_suppress.count"];
  // expired keys are not sent (or received). Just check expired keys
  // were also supressed
  EXPECT_GE(s1Supressed4 - s1Supressed3, 1);
}

/**
 * this is to verify correctness of 3-way full-sync
 * tuple represents (key, value-version, value)
 * storeA has (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 1, a)
 * storeB has             (k1, 1, a), (k2, 1, b), (k3, 9, b), (k4, 6, b)
 * Let A do init a full-sync with B
 * we expect both storeA and storeB have:
 *           (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 9, b), (k4, 6, b)
 */
TEST_F(KvStoreTestFixture, FullSync) {
  auto storeA = createKvStore(getTestKvConf("storeA"));
  auto storeB = createKvStore(getTestKvConf("storeB"));
  storeA->run();
  storeB->run();

  const std::string k0{"key0"};
  const std::string k1{"key1"};
  const std::string k2{"key2"};
  const std::string k3{"key3"};
  const std::string k4{"key4"};
  std::vector<std::string> allKeys = {k0, k1, k2, k3, k4};
  std::vector<std::pair<std::string, int>> keyVersionAs = {
      {k0, 5}, {k1, 1}, {k2, 9}, {k3, 1}};
  std::vector<std::pair<std::string, int>> keyVersionBs = {
      {k1, 1}, {k2, 1}, {k3, 9}, {k4, 6}};

  // set key vals in storeA
  for (const auto& [key, version] : keyVersionAs) {
    thrift::Value val = createThriftValue(
        version /* version */,
        "storeA" /* originatorId */,
        "a" /* value */,
        30000 /* ttl */,
        99 /* ttl version */,
        0 /* hash*/
    );
    val.hash() = generateHash(*val.version(), *val.originatorId(), val.value());
    EXPECT_TRUE(storeA->setKey(kTestingAreaName, key, val));
  }

  // set key vals in storeB
  for (const auto& [key, version] : keyVersionBs) {
    thrift::Value val = createThriftValue(
        version /* version */,
        "storeB" /* originatorId */,
        "b" /* value */,
        30000 /* ttl */,
        99 /* ttl version */,
        0 /* hash*/
    );
    if (key == k1) {
      val.value() = "a"; // set same value for k1
    }
    val.hash() = generateHash(*val.version(), *val.originatorId(), val.value());
    EXPECT_TRUE(storeB->setKey(kTestingAreaName, key, val));
  }

  OpenrEventBase evb;
  folly::Baton waitBaton;
  int scheduleAt{0};
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // storeA has (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 1, a)
        // storeB has             (k1, 1, a), (k2, 1, b), (k3, 9, b), (k4, 6, b)
        // let A sends a full sync request to B and wait for completion
        storeA->addPeer(kTestingAreaName, "storeB", storeB->getPeerSpec());
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 1000), [&]() noexcept {
        // after full-sync, we expect both A and B have:
        // (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 9, b), (k4, 6, b)
        for (const auto& key : allKeys) {
          auto valA = storeA->getKey(kTestingAreaName, key);
          auto valB = storeB->getKey(kTestingAreaName, key);
          EXPECT_TRUE(valA.has_value());
          EXPECT_TRUE(valB.has_value());
          EXPECT_EQ(valA->value().value(), valB->value().value());
          EXPECT_EQ(*valA->version(), *valB->version());
        }
        auto v0 = storeA->getKey(kTestingAreaName, k0);
        EXPECT_EQ(*v0->version(), 5);
        EXPECT_EQ(v0->value().value(), "a");
        auto v1 = storeA->getKey(kTestingAreaName, k1);
        EXPECT_EQ(*v1->version(), 1);
        EXPECT_EQ(v1->value().value(), "a");
        auto v2 = storeA->getKey(kTestingAreaName, k2);
        EXPECT_EQ(*v2->version(), 9);
        EXPECT_EQ(v2->value().value(), "a");
        auto v3 = storeA->getKey(kTestingAreaName, k3);
        EXPECT_EQ(*v3->version(), 9);
        EXPECT_EQ(v3->value().value(), "b");
        auto v4 = storeA->getKey(kTestingAreaName, k4);
        EXPECT_EQ(*v4->version(), 6);
        EXPECT_EQ(v4->value().value(), "b");
        // Synchronization primitive
        waitBaton.post();
      });

  // Start the event loop and wait until it is finished execution.
  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  evb.stop();
  evb.waitUntilStopped();
  evbThread.join();
}

/*
 * Verify kvStore flooding is containted within an area.
 * Add a key in one area and verify that key is not flooded into the other.
 *
 * Topology:
 *
 * StoreA (pod-area)  StoreC (plane-area)
 *        \                /
 *         \              /
 *          \            /
 *           \         /
 *  (pod-area) StoreB (plane-area)
 */
CO_TEST_F(KvStoreTestFixture, KeySyncMultipleArea) {
  thrift::AreaConfig pod, plane;
  pod.area_id() = "pod-area";
  pod.neighbor_regexes()->emplace_back(".*");
  plane.area_id() = "plane-area";
  plane.neighbor_regexes()->emplace_back(".*");
  AreaId podAreaId{*pod.area_id()};
  AreaId planeAreaId{*plane.area_id()};

  auto storeA = createKvStore(getTestKvConf("storeA"), {*pod.area_id()});
  auto storeB = createKvStore(
      getTestKvConf("storeB"), {*pod.area_id(), *plane.area_id()});
  auto storeC = createKvStore(getTestKvConf("storeC"), {*plane.area_id()});

  folly::F14FastMap<std::string, thrift::Value> expectedKeyValsPod{};
  folly::F14FastMap<std::string, thrift::Value> expectedKeyValsPlane{};

  size_t keyVal0Size, keyVal1Size, keyVal2Size, keyVal3Size;

  const std::string k0{"pod-area-0"};
  const std::string k1{"pod-area-1"};
  const std::string k2{"plane-area-0"};
  const std::string k3{"plane-area-1"};

  // to aid in keyVal sizes below, calculate total of struct members with
  // fixed size once at the beginning
  size_t fixed_size = (sizeof(std::string) + sizeof(thrift::Value));

  thrift::Value thriftVal0 = createThriftValue(
      1 /* version */,
      "storeA" /* originatorId */,
      "valueA" /* value */,
      Constants::kTtlInfinity /* ttl */);
  thriftVal0.hash() = generateHash(
      *thriftVal0.version(), *thriftVal0.originatorId(), thriftVal0.value());

  keyVal0Size = k0.size() + thriftVal0.originatorId()->size() +
      thriftVal0.value()->size() + fixed_size;

  thrift::Value thriftVal1 = createThriftValue(
      1 /* version */,
      "storeB" /* originatorId */,
      "valueB" /* value */,
      Constants::kTtlInfinity /* ttl */);
  thriftVal1.hash() = generateHash(
      *thriftVal1.version(), *thriftVal1.originatorId(), thriftVal1.value());

  keyVal1Size = k1.size() + thriftVal1.originatorId()->size() +
      thriftVal1.value()->size() + fixed_size;

  thrift::Value thriftVal2 = createThriftValue(
      1 /* version */,
      "storeC" /* originatorId */,
      std::string("valueC") /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  thriftVal2.hash() = generateHash(
      *thriftVal2.version(), *thriftVal2.originatorId(), thriftVal2.value());

  keyVal2Size = k2.size() + thriftVal2.originatorId()->size() +
      thriftVal2.value()->size() + fixed_size;

  thrift::Value thriftVal3 = createThriftValue(
      1 /* version */,
      "storeC" /* originatorId */,
      "valueC" /* value */,
      Constants::kTtlInfinity /* ttl */);
  thriftVal3.hash() = generateHash(
      *thriftVal3.version(), *thriftVal3.originatorId(), thriftVal3.value());

  keyVal3Size = k3.size() + thriftVal3.originatorId()->size() +
      thriftVal3.value()->size() + fixed_size;

  {
    storeA->run();
    storeB->run();
    storeC->run();

    storeA->addPeer(podAreaId, "storeB", storeB->getPeerSpec());
    storeB->addPeer(podAreaId, "storeA", storeA->getPeerSpec());
    storeB->addPeer(planeAreaId, "storeC", storeC->getPeerSpec());
    storeC->addPeer(planeAreaId, "storeB", storeB->getPeerSpec());
    // verify get peers command
    auto storeANodeId = storeA->getNodeId();
    auto podPeerSpec =
        storeA->getPeerSpec(thrift::KvStorePeerState::INITIALIZED);
    folly::F14FastMap<std::string, thrift::PeerSpec> expectedPeersPod = {
        {storeANodeId, podPeerSpec},
    };

    auto storeCNodeId = storeC->getNodeId();
    auto planePeerSpec =
        storeC->getPeerSpec(thrift::KvStorePeerState::INITIALIZED);
    folly::F14FastMap<std::string, thrift::PeerSpec> expectedPeersPlane = {
        {storeCNodeId, planePeerSpec},
    };

    waitForAllPeersInitialized();

    auto cmpPeers = storeB->getPeers(podAreaId);
    EXPECT_EQ(*cmpPeers[storeANodeId].peerAddr(), *podPeerSpec.peerAddr());
    EXPECT_EQ(*cmpPeers[storeANodeId].ctrlPort(), *podPeerSpec.ctrlPort());
    EXPECT_EQ(*cmpPeers[storeANodeId].state(), *podPeerSpec.state());
    EXPECT_LT(*cmpPeers[storeANodeId].stateElapsedTimeMs(), 5000);
    EXPECT_EQ(*cmpPeers[storeANodeId].flaps(), 0);

    cmpPeers = storeB->getPeers(planeAreaId);
    EXPECT_EQ(*cmpPeers[storeCNodeId].peerAddr(), *planePeerSpec.peerAddr());
    EXPECT_EQ(*cmpPeers[storeCNodeId].ctrlPort(), *planePeerSpec.ctrlPort());
    EXPECT_EQ(*cmpPeers[storeCNodeId].state(), *planePeerSpec.state());
    EXPECT_LT(*cmpPeers[storeCNodeId].stateElapsedTimeMs(), 5000);
    EXPECT_EQ(*cmpPeers[storeCNodeId].flaps(), 0);
  }

  {
    // set key in default area, but storeA does not have default area, this
    // should fail
    EXPECT_FALSE(storeA->setKey(kTestingAreaName, k0, thriftVal0));
    // set key in the correct area
    EXPECT_TRUE(storeA->setKey(podAreaId, k0, thriftVal0));
    // store A should not have the key in default area
    EXPECT_FALSE(storeA->getKey(kTestingAreaName, k0).has_value());
    // store A should have the key in pod-area
    EXPECT_TRUE(storeA->getKey(podAreaId, k0).has_value());
    // store B should have the key in pod-area
    waitForKeyInStoreWithTimeout(storeB, podAreaId, k0);
    // store B should NOT have the key in plane-area
    EXPECT_FALSE(storeB->getKey(planeAreaId, k0).has_value());
  }

  {
    // set key in store C and verify it's present in plane area in store B
    // and not present in POD area in storeB and storeA set key in the
    // correct area
    EXPECT_TRUE(storeC->setKey(planeAreaId, k2, thriftVal2));
    // store C should have the key in plane.area_id
    EXPECT_TRUE(storeC->getKey(planeAreaId, k2).has_value());
    // store B should have the key in plane.area_id
    waitForKeyInStoreWithTimeout(storeB, planeAreaId, k2);
    // store B should NOT have the key in pod.area_id
    EXPECT_FALSE(storeB->getKey(podAreaId, k2).has_value());
    // store A should NOT have the key in pod.area_id
    EXPECT_FALSE(storeA->getKey(podAreaId, k2).has_value());
  }

  {
    // add another key in both plane and pod area
    EXPECT_TRUE(storeB->setKey(podAreaId, k1, thriftVal1));
    EXPECT_TRUE(storeC->setKey(planeAreaId, k3, thriftVal3));

    waitForKeyInStoreWithTimeout(storeA, podAreaId, k1);
    waitForKeyInStoreWithTimeout(storeB, planeAreaId, k3);

    // pod area expected key values
    expectedKeyValsPod[k0] = thriftVal0;
    expectedKeyValsPod[k1] = thriftVal1;

    // plane area expected key values
    expectedKeyValsPlane[k2] = thriftVal2;
    expectedKeyValsPlane[k3] = thriftVal3;

    // pod area
    EXPECT_EQ(expectedKeyValsPod, storeA->dumpAll(podAreaId));
    EXPECT_EQ(expectedKeyValsPod, storeB->dumpAll(podAreaId));

    // plane area
    EXPECT_EQ(expectedKeyValsPlane, storeB->dumpAll(planeAreaId));
    EXPECT_EQ(expectedKeyValsPlane, storeC->dumpAll(planeAreaId));

    // check for counters on StoreB that has 2 instances. Number of keys
    // must be the total of both areas number of keys must be 4, 2 from
    // pod.area_id and 2 from planArea number of peers at storeB must be 2 -
    // one from each area
    EXPECT_EQ(2, storeB->dumpAll(podAreaId).size());
    EXPECT_EQ(2, storeB->dumpAll(planeAreaId).size());
  }

  {
    // based on above config, with 3 kvstore nodes spanning two areas,
    // storeA and storeC will send back areaSummary vector with 1 entry
    // and storeB, which has two areas, will send back vector with 2
    // entries. each entry in the areaSummary vector will have 2 keys (per
    // above)
    std::set<std::string> areaSetAll{
        *pod.area_id(), *plane.area_id(), kTestingAreaName};
    std::set<std::string> areaSetEmpty{};
    std::map<std::string, int> storeBTest{};

    auto summary = co_await storeA->getSummary(areaSetAll);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(*summary.at(0).area(), *pod.area_id());
    EXPECT_EQ(keyVal0Size + keyVal1Size, *summary.at(0).keyValsBytes());

    summary = co_await storeA->getSummary(areaSetEmpty);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(*summary.at(0).area(), *pod.area_id());
    EXPECT_EQ(keyVal0Size + keyVal1Size, *summary.at(0).keyValsBytes());

    summary = co_await storeB->getSummary(areaSetAll);
    EXPECT_EQ(2, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(2, *summary.at(1).keyValsCount());
    // for storeB, spanning 2 areas, check that kv count for all areas add
    // up individually
    storeBTest[*summary.at(0).area()] = *summary.at(0).keyValsBytes();
    storeBTest[*summary.at(1).area()] = *summary.at(1).keyValsBytes();
    EXPECT_EQ(1, storeBTest.count(*plane.area_id()));
    EXPECT_EQ(keyVal2Size + keyVal3Size, storeBTest[*plane.area_id()]);
    EXPECT_EQ(1, storeBTest.count(*pod.area_id()));
    EXPECT_EQ(keyVal0Size + keyVal1Size, storeBTest[*pod.area_id()]);

    summary = co_await storeB->getSummary(areaSetEmpty);
    EXPECT_EQ(2, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(2, *summary.at(1).keyValsCount());

    summary = co_await storeC->getSummary(areaSetAll);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(*summary.at(0).area(), *plane.area_id());
    EXPECT_EQ(keyVal2Size + keyVal3Size, *summary.at(0).keyValsBytes());

    summary = co_await storeC->getSummary(areaSetEmpty);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(*summary.at(0).area(), *plane.area_id());
    EXPECT_EQ(keyVal2Size + keyVal3Size, *summary.at(0).keyValsBytes());
  }
}

/**
 * this is to verify correctness of 3-way full-sync between default and
 * non-default Areas. storeA is in kDefaultArea, while storeB is in areaB.
 * tuple represents (key, value-version, value)
 * storeA has (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 1, a)
 * storeB has             (k1, 1, a), (k2, 1, b), (k3, 9, b), (k4, 6, b)
 * Let A do init a full-sync with B
 * we expect both storeA and storeB have:
 *           (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 9, b), (k4, 6, b)
 */
TEST_F(KvStoreTestFixture, KeySyncWithBackwardCompatibility) {
  AreaId defaultAreaId{Constants::kDefaultArea.toString()};

  auto storeA = createKvStore(
      getTestKvConf("storeA"), {Constants::kDefaultArea.toString()});
  auto storeB = createKvStore(getTestKvConf("storeB"), {kTestingAreaName});
  storeA->run();
  storeB->run();

  const std::string k0{"key0"};
  const std::string k1{"key1"};
  const std::string k2{"key2"};
  const std::string k3{"key3"};
  const std::string k4{"key4"};
  std::vector<std::string> allKeys = {k0, k1, k2, k3, k4};
  std::vector<std::pair<std::string, int>> keyVersionAs = {
      {k0, 5}, {k1, 1}, {k2, 9}, {k3, 1}};
  std::vector<std::pair<std::string, int>> keyVersionBs = {
      {k1, 1}, {k2, 1}, {k3, 9}, {k4, 6}};

  // set key vals in storeA
  for (const auto& [key, version] : keyVersionAs) {
    thrift::Value val = createThriftValue(
        version /* version */,
        "storeA" /* originatorId */,
        "a" /* value */,
        30000 /* ttl */,
        99 /* ttl version */,
        0 /* hash*/
    );

    val.hash() = generateHash(*val.version(), *val.originatorId(), val.value());
    EXPECT_TRUE(storeA->setKey(kTestingAreaName, key, val));
  }

  // set key vals in storeB
  for (const auto& [key, version] : keyVersionBs) {
    thrift::Value val = createThriftValue(
        version /* version */,
        "storeB" /* originatorId */,
        "b" /* value */,
        30000 /* ttl */,
        99 /* ttl version */,
        0 /* hash*/);
    if (key == k1) {
      val.value() = "a"; // set same value for k1
    }
    val.hash() = generateHash(*val.version(), *val.originatorId(), val.value());
    EXPECT_TRUE(storeB->setKey(kTestingAreaName, key, val));
  }

  OpenrEventBase evb;
  folly::Baton waitBaton;
  int scheduleAt{0};
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // storeA has (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 1, a)
        // storeB has             (k1, 1, a), (k2, 1, b), (k3, 9, b), (k4, 6, b)
        // let A sends a full sync request to B and wait for completion
        storeA->addPeer(kTestingAreaName, "storeB", storeB->getPeerSpec());
        storeB->addPeer(defaultAreaId, "storeA", storeA->getPeerSpec());
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 1000), [&]() noexcept {
        // after full-sync, we expect both A and B have:
        // (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 9, b), (k4, 6, b)
        for (const auto& key : allKeys) {
          auto valA = storeA->getKey(kTestingAreaName, key);
          auto valB = storeB->getKey(kTestingAreaName, key);
          EXPECT_TRUE(valA.has_value());
          EXPECT_TRUE(valB.has_value());
          EXPECT_EQ(valA->value().value(), valB->value().value());
          EXPECT_EQ(*valA->version(), *valB->version());
        }
        auto v0 = storeA->getKey(kTestingAreaName, k0);
        EXPECT_EQ(*v0->version(), 5);
        EXPECT_EQ(v0->value().value(), "a");
        auto v1 = storeA->getKey(kTestingAreaName, k1);
        EXPECT_EQ(*v1->version(), 1);
        EXPECT_EQ(v1->value().value(), "a");
        auto v2 = storeA->getKey(kTestingAreaName, k2);
        EXPECT_EQ(*v2->version(), 9);
        EXPECT_EQ(v2->value().value(), "a");
        auto v3 = storeA->getKey(kTestingAreaName, k3);
        EXPECT_EQ(*v3->version(), 9);
        EXPECT_EQ(v3->value().value(), "b");
        auto v4 = storeA->getKey(kTestingAreaName, k4);
        EXPECT_EQ(*v4->version(), 6);
        EXPECT_EQ(v4->value().value(), "b");
        // Synchronization primitive
        waitBaton.post();
      });

  // Start the event loop and wait until it is finished execution.
  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  evb.stop();
  evb.waitUntilStopped();
  evbThread.join();
}

/**
 * Validate client
 */
// TEST_F(KvStoreTestFixture, SecureClientTest) {
//   AreaId defaultAreaId{Constants::kDefaultArea.toString()};

//   auto storeA = createKvStore(
//       getTestKvConf("storeA"), {Constants::kDefaultArea.toString()});
//   auto storeB = createKvStore(getTestKvConf("storeB"), {kTestingAreaName});
//   storeA->run();
//   storeB->run();
//   EXPECT_TRUE(
//       storeA->addPeer(kTestingAreaName, "storeB", storeB->getPeerSpec()));
//   EXPECT_TRUE(storeB->addPeer(defaultAreaId, "storeA",
//   storeA->getPeerSpec()));
// }

/**
 * Verify that fabric-internal keys (adj/prefix/drainStatus keys for fabric
 * nodes) are flooded only to fabric peers, while non-fabric keys are flooded to
 * all peers.
 *
 * Setup:
 *  - Node A (fabric node "eb01-ld002.dfw1") with fabricConfig — publisher
 *  - Node B (fabric peer "eb01-sp002.dfw1") — matches spine regex
 *  - Node C (non-fabric peer "external-node") — no regex match
 *
 * Expected behavior:
 *  - Fabric adj/prefix/drainStatus keys set on A → flood to B only
 *  - Non-fabric key set on A → flood to both B and C
 */
TEST_F(KvStoreTestFixture, FloodPublicationFabricScope) {
  // Build FabricConfig with leaf/spine regexes
  thrift::FabricConfig thriftFabricConfig;
  thriftFabricConfig.fabric_name() = "bbf01.dfw1";
  thriftFabricConfig.fabric_prefixes() = {"1::1/128"};
  thriftFabricConfig.fabric_leaf_regexes() = {"eb01-ld\\d{3}\\.dfw1"};
  thriftFabricConfig.fabric_spine_regexes() = {"eb01-sp\\d{3}\\.dfw1"};
  FabricConfig fabricConfig(thriftFabricConfig);

  const std::string nodeAId = "eb01-ld002.dfw1";
  const std::string nodeBId = "eb01-sp002.dfw1";
  const std::string nodeCId = "external-node";

  // Node A: fabric node (publisher) with fabricConfig
  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* storeA =
      createKvStore(
          getTestKvConf(nodeAId),
          {kTestingAreaName.t},
          std::nullopt,
          std::nullopt,
          fabricConfig);
  // Node B: fabric peer (spine name matches spine regex)
  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* storeB =
      createKvStore(getTestKvConf(nodeBId));
  // Node C: non-fabric peer (name does not match any regex)
  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* storeC =
      createKvStore(getTestKvConf(nodeCId));

  storeA->run();
  storeB->run();
  storeC->run();

  // Establish bidirectional peering: A↔B
  EXPECT_THAT(
      storeA->addPeer(
          kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      storeB->addPeer(
          kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()),
      IsTrue());

  // Establish bidirectional peering: A↔C
  EXPECT_THAT(
      storeA->addPeer(
          kTestingAreaName, storeC->getNodeId(), storeC->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      storeC->addPeer(
          kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()),
      IsTrue());

  waitForAllPeersInitialized();

  // Keys to set on Node A:
  //  - fabricAdjKey: adj key for a fabric leaf node → fabric-internal
  //  - fabricPrefixKey: prefix key for a fabric spine node → fabric-internal
  //  - fabricDrainStatusKey: drainStatus key for this fabric → fabric-internal
  //  - lagEbFaIfStatusKey: LAG EB/FA if-status key for a BBF node → NOT
  //    fabric-internal (must still flood to non-fabric peers)
  //  - nonFabricKey: adj key for external node → NOT fabric-internal
  const std::string fabricAdjKey = "adj:eb01-ld002.dfw1";
  const std::string fabricPrefixKey = "prefix:eb01-sp002.dfw1:[10.0.0.0/8]";
  const std::string fabricDrainStatusKey = "drainStatus:bbf01.dfw1";
  const std::string lagEbFaIfStatusKey = "lagEbFaIfStatus:bbf01.dfw1";
  const std::string nonFabricKey = "adj:external-node";

  const auto thriftVal = [&](const std::string& val) {
    return createThriftValue(
        1 /* version */,
        nodeAId /* originatorId */,
        val /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(1, nodeAId, thrift::Value().value() = std::string(val)));
  };

  EXPECT_THAT(
      storeA->setKey(kTestingAreaName, fabricAdjKey, thriftVal("fab-adj")),
      IsTrue());
  EXPECT_THAT(
      storeA->setKey(
          kTestingAreaName, fabricPrefixKey, thriftVal("fab-prefix")),
      IsTrue());
  EXPECT_THAT(
      storeA->setKey(
          kTestingAreaName, fabricDrainStatusKey, thriftVal("fab-drain")),
      IsTrue());
  EXPECT_THAT(
      storeA->setKey(
          kTestingAreaName, lagEbFaIfStatusKey, thriftVal("lag-status")),
      IsTrue());
  EXPECT_THAT(
      storeA->setKey(kTestingAreaName, nonFabricKey, thriftVal("non-fab")),
      IsTrue());

  // Wait for the non-fabric keys to propagate to both peers (they should always
  // reach both B and C). lagEbFaIfStatus is a BBF key but NOT fabric-internal,
  // so it must also reach the non-fabric peer C.
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, nonFabricKey);
  waitForKeyInStoreWithTimeout(storeC, kTestingAreaName, nonFabricKey);
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, lagEbFaIfStatusKey);
  waitForKeyInStoreWithTimeout(storeC, kTestingAreaName, lagEbFaIfStatusKey);

  // Also wait for fabric keys to arrive at B
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, fabricAdjKey);
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, fabricPrefixKey);
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, fabricDrainStatusKey);

  // Node B (fabric peer): should have ALL keys
  folly::F14FastMap<std::string, thrift::Value> dumpB =
      storeB->dumpAll(kTestingAreaName);
  EXPECT_THAT(dumpB.count(fabricAdjKey), Eq(1));
  EXPECT_THAT(dumpB.count(fabricPrefixKey), Eq(1));
  EXPECT_THAT(dumpB.count(fabricDrainStatusKey), Eq(1));
  EXPECT_THAT(dumpB.count(lagEbFaIfStatusKey), Eq(1));
  EXPECT_THAT(dumpB.count(nonFabricKey), Eq(1));

  // Node C (non-fabric peer): should have the non-fabric keys
  // (adj:external-node and lagEbFaIfStatus:*) but none of the fabric-internal
  // keys.
  folly::F14FastMap<std::string, thrift::Value> dumpC =
      storeC->dumpAll(kTestingAreaName);
  EXPECT_THAT(dumpC.count(nonFabricKey), Eq(1));
  EXPECT_THAT(dumpC.count(fabricAdjKey), Eq(0))
      << "Fabric adj key should NOT be flooded to non-fabric peer";
  EXPECT_THAT(dumpC.count(fabricPrefixKey), Eq(0))
      << "Fabric prefix key should NOT be flooded to non-fabric peer";
  EXPECT_THAT(dumpC.count(fabricDrainStatusKey), Eq(0))
      << "drainStatus key should NOT be flooded to non-fabric peer";
  EXPECT_THAT(dumpC.count(lagEbFaIfStatusKey), Eq(1))
      << "lagEbFaIfStatus key SHOULD be flooded to non-fabric peer";
}

namespace {

/*
 * [Flood pre-compression / memory-budget test helpers]
 *
 * The flood memory-budget workflow (byte accounting via FloodByteCharge,
 * deferral into pendingFloodKeys_, drain) is gated behind
 * enable_flood_pub_pre_compression. The tests below turn that knob on so the
 * path is actually exercised; without it floodNow is always true and the whole
 * feature is inert.
 */
thrift::KvStoreConfig
getPreCompressKvConf(const std::string& nodeId) {
  thrift::KvStoreConfig kvConf;
  kvConf.node_name() = nodeId;
  kvConf.enable_flood_pub_pre_compression() = true;
  return kvConf;
}

/*
 * Config that forces the area into backpressure immediately.
 *
 * The production budget is 128 MiB, which a unit test cannot reach -- it would
 * have to hold that much in-flight compressed payload. A 1-byte budget instead
 * makes the "already at/over budget" check true the moment any flood RPC is
 * outstanding, so deferral into pendingFloodKeys_ and the subsequent drain are
 * hit deterministically rather than by racing real memory growth.
 */
thrift::KvStoreConfig
getTinyBudgetKvConf(const std::string& nodeId) {
  auto kvConf = getPreCompressKvConf(nodeId);
  kvConf.flood_mem_budget_bytes() = 1;
  return kvConf;
}

/*
 * Read an fb303 counter, treating "absent" as 0. Counters registered via
 * addStatExportType export 0 from startup, but resetAllData() drops them until
 * the next bump, so absent and zero must be handled alike.
 */
int64_t
getCounterOrZero(const std::string& name) {
  const auto counters = fb303::fbData->getCounters();
  const auto it = counters.find(name);
  return it == counters.end() ? 0 : it->second;
}

/*
 * Assert the flood pre-compression / memory-budget path actually ran.
 *
 * Presence, not value: publishFloodBackpressureState() samples the backpressure
 * state only when enable_flood_pub_pre_compression is on, so after
 * resetAllData() the stat exists if and only if that path executed. The VALUE
 * cannot be used -- the sample is deliberately taken at the backpressure
 * decision point, before this publication is charged, so a healthy node with
 * non-overlapping floods correctly reports 0.
 */
void
expectPreCompressionPathExercised() {
  const auto counters = fb303::fbData->getCounters();
  EXPECT_TRUE(counters.contains("kvstore.flood.outstanding_bytes.avg"))
      << "flood pre-compression path did not run; budget accounting inert";
}

/*
 * Poll until `key` in `store` carries `expectedValue`. Returns false on
 * timeout. Value-aware (not just presence) so a corrupted or stale payload
 * fails rather than passing on mere arrival.
 */
bool
waitForKeyValue(
    KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* store,
    AreaId const& areaId,
    std::string const& key,
    std::string const& expectedValue,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    const auto val = store->getKey(areaId, key);
    if (val.has_value() && val->value().has_value() &&
        *val->value() == expectedValue) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

/*
 * Poll until `peerName` reaches `expectedState` in `store`. Returns false on
 * timeout.
 */
bool
waitForPeerState(
    KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* store,
    AreaId const& areaId,
    std::string const& peerName,
    thrift::KvStorePeerState expectedState,
    std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    const auto peers = store->getPeers(areaId);
    const auto it = peers.find(peerName);
    if (it != peers.end() && *it->second.state() == expectedState) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

/*
 * Build a value large and repetitive enough that zstd pre-compression actually
 * does work on it, while remaining unique per key so a cross-peer payload mixup
 * is detectable.
 */
std::string
makeFloodValue(const std::string& tag) {
  std::string value;
  value.reserve(1024);
  for (int i = 0; i < 32; ++i) {
    value += fmt::format("{}-block{:02d}-", tag, i);
  }
  return value;
}

} // namespace

/**
 * Verify that with flood pre-compression enabled a single publication is
 * serialized+compressed once and delivered intact to every peer.
 *
 * Why this matters beyond plain propagation: on this path all peers are handed
 * `serializedBuf.clone()`, which shares one refcounted payload rather than
 * copying it, and that one buffer carries a single FloodByteCharge. Thrift's
 * DefaultPayloadSerializerStrategy will serialize RPC metadata directly into a
 * data buffer's headroom when it is allowed to, which for a shared buffer would
 * mean every peer writing into the same allocation. It is safe only because
 * canSerializeMetadataIntoDataBufferHeadroom() gates on !isSharedOne(). This
 * test fans out to several peers and asserts exact payloads, so that class of
 * corruption surfaces as a value mismatch instead of passing silently.
 */
TEST_F(KvStoreTestFixture, FloodPreCompressionMultiPeerDelivery) {
  fb303::fbData->resetAllData();

  constexpr size_t kNumPeers{4};
  constexpr size_t kNumKeys{5};
  const std::string publisherId{"pre-compress-publisher"};

  auto* publisher = createKvStore(getPreCompressKvConf(publisherId));
  publisher->run();

  std::vector<KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>*>
      peers;
  for (size_t i = 0; i < kNumPeers; ++i) {
    const auto peerId = getNodeId("pre-compress-peer", i);
    auto* peer = createKvStore(getPreCompressKvConf(peerId));
    peer->run();
    peers.emplace_back(peer);

    EXPECT_THAT(
        publisher->addPeer(
            kTestingAreaName, peer->getNodeId(), peer->getPeerSpec()),
        IsTrue());
    EXPECT_THAT(
        peer->addPeer(
            kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
        IsTrue());
  }
  waitForAllPeersInitialized();

  // Set distinct, compressible values so a payload mixup is visible.
  std::map<std::string, std::string> expectedKeyVals;
  for (size_t i = 0; i < kNumKeys; ++i) {
    const auto key = fmt::format("pre-compress-key{}", i);
    const auto value = makeFloodValue(key);
    expectedKeyVals.emplace(key, value);

    auto thriftVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1, publisherId, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }

  // Every peer must receive every key with the exact payload.
  for (auto* peer : peers) {
    for (const auto& [key, value] : expectedKeyVals) {
      EXPECT_TRUE(waitForKeyValue(peer, kTestingAreaName, key, value))
          << "peer " << peer->getNodeId() << " missing/incorrect key " << key;
    }
  }

  // Positive control: flooding really happened, so the assertions above are not
  // vacuously true on a store that never flooded.
  EXPECT_GT(getCounterOrZero("kvstore.thrift.num_flood_pub.count"), 0);
  expectPreCompressionPathExercised();
}

/**
 * Verify fabric scoping still holds with flood pre-compression enabled.
 *
 * With a fabric-external peer present, one publication yields TWO distinct
 * serialized buffers -- the full set and the fabric-filtered set -- each
 * serialized, compressed and charged against the flood budget independently.
 * This covers that multi-buffer-per-publication shape, which the non-fabric
 * tests never produce.
 */
TEST_F(KvStoreTestFixture, FloodPreCompressionFabricScope) {
  fb303::fbData->resetAllData();

  thrift::FabricConfig thriftFabricConfig;
  thriftFabricConfig.fabric_name() = "bbf01.dfw";
  thriftFabricConfig.fabric_prefixes() = {"1::1/128"};
  thriftFabricConfig.fabric_leaf_regexes() = {"eb01-ld\\d{3}\\.dfw1"};
  thriftFabricConfig.fabric_spine_regexes() = {"eb01-sp\\d{3}\\.dfw1"};
  FabricConfig fabricConfig(thriftFabricConfig);

  const std::string nodeAId = "eb01-ld002.dfw1"; // fabric publisher
  const std::string nodeBId = "eb01-sp002.dfw1"; // fabric peer
  const std::string nodeCId = "external-node"; // fabric-external peer

  auto* storeA = createKvStore(
      getPreCompressKvConf(nodeAId),
      {kTestingAreaName.t},
      std::nullopt,
      std::nullopt,
      fabricConfig);
  auto* storeB = createKvStore(getPreCompressKvConf(nodeBId));
  auto* storeC = createKvStore(getPreCompressKvConf(nodeCId));

  storeA->run();
  storeB->run();
  storeC->run();

  for (auto* peer : {storeB, storeC}) {
    EXPECT_THAT(
        storeA->addPeer(
            kTestingAreaName, peer->getNodeId(), peer->getPeerSpec()),
        IsTrue());
    EXPECT_THAT(
        peer->addPeer(
            kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()),
        IsTrue());
  }
  waitForAllPeersInitialized();

  const std::string fabricAdjKey = "adj:eb01-ld002.dfw1";
  const std::string fabricPrefixKey = "prefix:eb01-sp002.dfw1:[10.0.0.0/8]";
  const std::string nonFabricKey = "adj:external-node";

  const auto setKey = [&](const std::string& key, const std::string& value) {
    auto thriftVal = createThriftValue(
        1 /* version */,
        nodeAId /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(1, nodeAId, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(storeA->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  };

  const auto fabricAdjVal = makeFloodValue("fab-adj");
  const auto fabricPrefixVal = makeFloodValue("fab-prefix");
  const auto nonFabricVal = makeFloodValue("non-fab");
  setKey(fabricAdjKey, fabricAdjVal);
  setKey(fabricPrefixKey, fabricPrefixVal);
  setKey(nonFabricKey, nonFabricVal);

  // Fabric peer receives everything, with payloads intact through the
  // compress-once path.
  EXPECT_TRUE(
      waitForKeyValue(storeB, kTestingAreaName, nonFabricKey, nonFabricVal));
  EXPECT_TRUE(
      waitForKeyValue(storeB, kTestingAreaName, fabricAdjKey, fabricAdjVal));
  EXPECT_TRUE(waitForKeyValue(
      storeB, kTestingAreaName, fabricPrefixKey, fabricPrefixVal));

  // Fabric-external peer receives only the non-fabric key.
  EXPECT_TRUE(
      waitForKeyValue(storeC, kTestingAreaName, nonFabricKey, nonFabricVal));
  // Guard: without pre-compression this degrades into the plain fabric-scope
  // test and would no longer cover the two-buffer-per-publication shape.
  expectPreCompressionPathExercised();

  const auto dumpC = storeC->dumpAll(kTestingAreaName);
  EXPECT_THAT(dumpC.count(fabricAdjKey), Eq(0))
      << "Fabric adj key should NOT be flooded to fabric-external peer";
  EXPECT_THAT(dumpC.count(fabricPrefixKey), Eq(0))
      << "Fabric prefix key should NOT be flooded to fabric-external peer";
}

/**
 * The byte watermark must capture a publication's own charge.
 *
 * The AVG gauges are sampled at the backpressure decision point, i.e. before
 * the publication is serialized and charged. If the watermark rode along with
 * that sample it would only ever see bytes contributed by *earlier* floods, so
 * a single flood with no follow-up traffic would allocate, send and complete
 * without its peak being recorded at all -- outstanding_bytes_max would read 0
 * while a multi-MiB buffer was resident. That is the same "transient is
 * invisible" failure the watermarks exist to fix.
 *
 * Deliberately one publication and nothing after it: with a burst, a later
 * flood's pre-charge sample would observe this one's bytes and hide the bug.
 * The value is highly compressible so the assertion also pins that the charge
 * is the buffer's allocated capacity rather than its compressed length.
 */
TEST_F(KvStoreTestFixture, FloodWatermarkCapturesSinglePublication) {
  fb303::fbData->resetAllData();
  resetFloodWatermarks();

  constexpr size_t kValueBytes{1 << 20};
  const std::string publisherId{"single-pub-publisher"};
  const std::string key{"single-pub-key"};
  const std::string value(kValueBytes, 'x');

  // Default budget: this must not engage backpressure -- the point is the
  // ordinary, non-deferred flood path.
  auto* publisher = createKvStore(getPreCompressKvConf(publisherId));
  auto* receiver = createKvStore(getPreCompressKvConf("single-pub-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  auto thriftVal = createThriftValue(
      1 /* version */,
      publisherId /* originatorId */,
      value /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(
          1, publisherId, thrift::Value().value() = std::string(value)));
  EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  EXPECT_TRUE(waitForKeyValue(receiver, kTestingAreaName, key, value));

  // No backpressure, so the AVG-sampled path contributed nothing; the mark can
  // only be non-zero if the charge itself recorded it.
  EXPECT_THAT(
      getCounterOrZero("kvstore.flood.backpressure_engaged.count"), Eq(0));
  EXPECT_GE(
      getCounterOrZero("kvstore.flood.outstanding_bytes_max"),
      static_cast<int64_t>(kValueBytes))
      << "isolated flood's peak was never recorded -- watermark is sampled "
         "before the charge, or charged compressed length instead of capacity";
  EXPECT_GE(
      getCounterOrZero(
          fmt::format(
              "kvstore.flood.outstanding_bytes_max.{}", kTestingAreaName.t)),
      static_cast<int64_t>(kValueBytes));
}

/**
 * Pin the invariant that the flood memory budget is a runaway guard, not an
 * operational rate limiter: a normal burst of updates must converge without
 * ever engaging backpressure or tripping wedge recovery.
 *
 * This is the regression guard for accounting drift. Every FloodByteCharge is
 * debited when a buffer is serialized and credited when the last peer's send
 * resolves; if a release were ever skipped, areaOutstandingFloodBytes_ would
 * ratchet upward instead of returning to zero and backpressure would eventually
 * engage on traffic like this, which the assertions below would catch.
 */
TEST_F(KvStoreTestFixture, FloodPreCompressionBudgetNotEngagedUnderNormalLoad) {
  fb303::fbData->resetAllData();

  constexpr size_t kNumKeys{200};
  const std::string publisherId{"budget-publisher"};

  auto* publisher = createKvStore(getPreCompressKvConf(publisherId));
  auto* receiver = createKvStore(getPreCompressKvConf("budget-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  std::string lastKey;
  std::string lastValue;
  for (size_t i = 0; i < kNumKeys; ++i) {
    lastKey = fmt::format("budget-key{}", i);
    lastValue = makeFloodValue(lastKey);
    auto thriftVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        lastValue /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1, publisherId, thrift::Value().value() = std::string(lastValue)));
    EXPECT_THAT(
        publisher->setKey(kTestingAreaName, lastKey, thriftVal), IsTrue());
  }

  // Convergence: the last key arriving implies the burst drained.
  EXPECT_TRUE(waitForKeyValue(receiver, kTestingAreaName, lastKey, lastValue));
  EXPECT_THAT(receiver->dumpAll(kTestingAreaName).size(), Eq(kNumKeys));

  // Positive control so the zero-assertions below cannot pass vacuously.
  EXPECT_GT(getCounterOrZero("kvstore.thrift.num_flood_pub.count"), 0);
  expectPreCompressionPathExercised();

  // A burst this size is orders of magnitude below the per-area budget, so
  // backpressure must never engage and the wedge-recovery path must never run.
  EXPECT_THAT(
      getCounterOrZero("kvstore.flood.backpressure_engaged.count"), Eq(0));
  EXPECT_THAT(
      getCounterOrZero("kvstore.flood.backpressure_resolved.count"), Eq(0));
  EXPECT_THAT(getCounterOrZero("kvstore.flood.stuck_reconciled.count"), Eq(0));
}

/**
 * Verify a flood RPC that fails against a dead peer still settles cleanly: the
 * peer is driven to IDLE and no wedge recovery is triggered.
 *
 * This covers the failure branch of the flood continuation. The charge is
 * dropped in a single thenTry before the success/exception split, so a failed
 * send must credit the budget exactly like a successful one; a leak here would
 * strand the area's accounting. Reaching IDLE proves the continuation ran, and
 * stuck_reconciled staying at zero proves nothing wedged behind it.
 */
TEST_F(KvStoreTestFixture, FloodPreCompressionPeerFailureSettlesCleanly) {
  fb303::fbData->resetAllData();

  const std::string publisherId{"failure-publisher"};
  auto* publisher = createKvStore(getPreCompressKvConf(publisherId));
  auto* deadPeer = createKvStore(getPreCompressKvConf("failure-peer"));
  publisher->run();
  deadPeer->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, deadPeer->getNodeId(), deadPeer->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      deadPeer->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  // Flood once while the peer is healthy. This both confirms the pre-compress
  // budget path is live (deterministic here, before the peer goes IDLE stops
  // new buffers being charged) and gives the failure below a working baseline.
  {
    const auto warmupValue = makeFloodValue("failure-warmup");
    auto warmupVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        warmupValue /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1,
            publisherId,
            thrift::Value().value() = std::string(warmupValue)));
    EXPECT_THAT(
        publisher->setKey(kTestingAreaName, "failure-warmup", warmupVal),
        IsTrue());
    EXPECT_TRUE(waitForKeyValue(
        deadPeer, kTestingAreaName, "failure-warmup", warmupValue));
    expectPreCompressionPathExercised();
  }

  // Kill the peer's thrift server; subsequent floods to it must fail. stop() is
  // idempotent, so the fixture teardown stopping it again is harmless.
  const auto deadPeerId = deadPeer->getNodeId();
  deadPeer->closeQueue();
  deadPeer->stop();

  // Flood towards the now-dead peer.
  for (size_t i = 0; i < 5; ++i) {
    const auto key = fmt::format("failure-key{}", i);
    const auto value = makeFloodValue(key);
    auto thriftVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1, publisherId, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }

  // The failed flood RPC drives the peer to IDLE via processThriftFailure,
  // which only runs from the continuation that also released the charge.
  EXPECT_TRUE(waitForPeerState(
      publisher, kTestingAreaName, deadPeerId, thrift::KvStorePeerState::IDLE))
      << "peer should transition to IDLE after flood RPC failure";

  // Accounting settled: nothing wedged the area out of flooding.
  EXPECT_THAT(getCounterOrZero("kvstore.flood.stuck_reconciled.count"), Eq(0));
}

/**
 * Drive the area over its flood memory budget and verify the full backpressure
 * cycle: publications are deferred into the area-level pending set, and the
 * budget freed by a completing flood RPC drains them so every key still lands
 * on the peer.
 *
 * A 1-byte budget makes the check trip as soon as any RPC is in flight, so this
 * exercises deferral and drain without needing 128 MiB of real payload.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureDefersAndDrains) {
  fb303::fbData->resetAllData();

  constexpr size_t kNumKeys{50};
  const std::string publisherId{"backpressure-publisher"};

  auto* publisher = createKvStore(getTinyBudgetKvConf(publisherId));
  auto* receiver = createKvStore(getPreCompressKvConf("backpressure-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  std::map<std::string, std::string> expectedKeyVals;
  for (size_t i = 0; i < kNumKeys; ++i) {
    const auto key = fmt::format("backpressure-key{}", i);
    const auto value = makeFloodValue(key);
    expectedKeyVals.emplace(key, value);

    auto thriftVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1, publisherId, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }

  // Deferral must actually have happened -- otherwise this is just the
  // no-backpressure test with a different config.
  EXPECT_GT(getCounterOrZero("kvstore.flood.backpressure_engaged.count"), 0)
      << "expected the 1-byte budget to defer at least one publication";

  // Despite deferral, every key must still reach the peer via
  // drainPendingFloods.
  for (const auto& [key, value] : expectedKeyVals) {
    EXPECT_TRUE(waitForKeyValue(receiver, kTestingAreaName, key, value))
        << "deferred key " << key << " was never drained to the peer";
  }
  EXPECT_THAT(receiver->dumpAll(kTestingAreaName).size(), Eq(kNumKeys));

  // Every backpressure episode that started must have ended: pendingFloodKeys_
  // is drained and cleared, not stranded.
  EXPECT_THAT(
      getCounterOrZero("kvstore.flood.backpressure_resolved.count"),
      Eq(getCounterOrZero("kvstore.flood.backpressure_engaged.count")));
}

/**
 * Verify the coalescing semantics of the deferred set: keys queued while over
 * budget are re-derived from kvStore_ at drain time, so the peer converges on
 * the latest value rather than replaying every superseded intermediate one.
 *
 * This is the behavior that makes deferral safe to coalesce -- buildCoalesced-
 * FloodParams looks the key up fresh instead of retaining the queued payload.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureCoalescesToLatestValue) {
  fb303::fbData->resetAllData();

  constexpr int64_t kNumUpdates{40};
  const std::string publisherId{"coalesce-publisher"};
  const std::string key{"coalesce-key"};

  auto* publisher = createKvStore(getTinyBudgetKvConf(publisherId));
  auto* receiver = createKvStore(getPreCompressKvConf("coalesce-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  // Repeatedly overwrite one key with monotonically increasing versions while
  // the area is backpressured.
  std::string finalValue;
  for (int64_t version = 1; version <= kNumUpdates; ++version) {
    finalValue = makeFloodValue(fmt::format("coalesce-v{}", version));
    auto thriftVal = createThriftValue(
        version /* version */,
        publisherId /* originatorId */,
        finalValue /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            version,
            publisherId,
            thrift::Value().value() = std::string(finalValue)));
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }

  EXPECT_GT(getCounterOrZero("kvstore.flood.backpressure_engaged.count"), 0);

  // The peer must land on the newest version, not a stale queued one.
  EXPECT_TRUE(waitForKeyValue(receiver, kTestingAreaName, key, finalValue));
  const auto received = receiver->getKey(kTestingAreaName, key);
  ASSERT_TRUE(received.has_value());
  EXPECT_THAT(*received->version(), Eq(kNumUpdates));

  /*
   * Coalescing is observable, and here it is strict: every deferral is a repeat
   * of the same key, so the drains must flush strictly fewer keys than were
   * deferred into them. This is the ratio num_deferred_keys/num_coalesced_keys
   * exists to report; a ratio of 1 would mean deferral bought nothing.
   */
  const auto deferredKeys =
      getCounterOrZero("kvstore.flood.num_deferred_keys.sum");
  const auto coalescedKeys =
      getCounterOrZero("kvstore.flood.num_coalesced_keys.sum");
  EXPECT_GT(deferredKeys, 0);
  EXPECT_GT(coalescedKeys, 0);
  EXPECT_GT(deferredKeys, coalescedKeys)
      << "repeated updates to one key were not coalesced: " << deferredKeys
      << " deferred vs " << coalescedKeys << " flushed";
}

/**
 * Verify fabric scoping is re-applied when draining deferred keys.
 *
 * The drain path does not reuse makeFabricParam: buildCoalescedFloodParams
 * re-implements the adj/prefix filtering against the fabric config. That
 * duplication can silently diverge, so this asserts the filtering still holds
 * on the drain path specifically, with the area forced into backpressure.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureDrainRespectsFabricScope) {
  fb303::fbData->resetAllData();

  thrift::FabricConfig thriftFabricConfig;
  thriftFabricConfig.fabric_name() = "bbf01.dfw";
  thriftFabricConfig.fabric_prefixes() = {"1::1/128"};
  thriftFabricConfig.fabric_leaf_regexes() = {"eb01-ld\\d{3}\\.dfw1"};
  thriftFabricConfig.fabric_spine_regexes() = {"eb01-sp\\d{3}\\.dfw1"};
  FabricConfig fabricConfig(thriftFabricConfig);

  const std::string nodeAId = "eb01-ld002.dfw1"; // fabric publisher
  const std::string nodeBId = "eb01-sp002.dfw1"; // fabric peer
  const std::string nodeCId = "external-node"; // fabric-external peer

  auto* storeA = createKvStore(
      getTinyBudgetKvConf(nodeAId),
      {kTestingAreaName.t},
      std::nullopt,
      std::nullopt,
      fabricConfig);
  auto* storeB = createKvStore(getPreCompressKvConf(nodeBId));
  auto* storeC = createKvStore(getPreCompressKvConf(nodeCId));

  storeA->run();
  storeB->run();
  storeC->run();

  for (auto* peer : {storeB, storeC}) {
    EXPECT_THAT(
        storeA->addPeer(
            kTestingAreaName, peer->getNodeId(), peer->getPeerSpec()),
        IsTrue());
    EXPECT_THAT(
        peer->addPeer(
            kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()),
        IsTrue());
  }
  waitForAllPeersInitialized();

  const std::string fabricAdjKey = "adj:eb01-ld002.dfw1";
  const std::string fabricPrefixKey = "prefix:eb01-sp002.dfw1:[10.0.0.0/8]";

  // Enough keys that the later ones are certain to be deferred and drained.
  std::map<std::string, std::string> nonFabricKeyVals;
  const auto setKey = [&](const std::string& key, const std::string& value) {
    auto thriftVal = createThriftValue(
        1 /* version */,
        nodeAId /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(1, nodeAId, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(storeA->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  };

  setKey(fabricAdjKey, makeFloodValue("fab-adj"));
  setKey(fabricPrefixKey, makeFloodValue("fab-prefix"));
  for (size_t i = 0; i < 20; ++i) {
    const auto key = fmt::format("adj:external-node{}", i);
    const auto value = makeFloodValue(key);
    nonFabricKeyVals.emplace(key, value);
    setKey(key, value);
  }

  EXPECT_GT(getCounterOrZero("kvstore.flood.backpressure_engaged.count"), 0)
      << "expected the 1-byte budget to force keys through the drain path";

  // Fabric peer converges on everything.
  for (const auto& [key, value] : nonFabricKeyVals) {
    EXPECT_TRUE(waitForKeyValue(storeB, kTestingAreaName, key, value));
  }
  EXPECT_TRUE(waitForKeyValue(
      storeB, kTestingAreaName, fabricAdjKey, makeFloodValue("fab-adj")));
  EXPECT_TRUE(waitForKeyValue(
      storeB, kTestingAreaName, fabricPrefixKey, makeFloodValue("fab-prefix")));

  // Fabric-external peer gets the non-fabric keys but never the fabric ones,
  // including for keys delivered via the drain path.
  for (const auto& [key, value] : nonFabricKeyVals) {
    EXPECT_TRUE(waitForKeyValue(storeC, kTestingAreaName, key, value));
  }
  const auto dumpC = storeC->dumpAll(kTestingAreaName);
  EXPECT_THAT(dumpC.count(fabricAdjKey), Eq(0))
      << "fabric adj key leaked to fabric-external peer via drain path";
  EXPECT_THAT(dumpC.count(fabricPrefixKey), Eq(0))
      << "fabric prefix key leaked to fabric-external peer via drain path";
}

/**
 * Verify that the smallest valid drain-reconcile threshold still allows keys
 * deferred under backpressure to converge.
 *
 * NOTE ON COVERAGE: this does NOT reach the wedge-reset branch inside
 * reconcileAndDrainPendingFloods (the one that zeroes leaked accounting and
 * bumps kvstore.flood.stuck_reconciled). That branch needs pendingFloodKeys_ to
 * still be non-empty when floodDrainTimer_ fires, and over loopback thrift a
 * flood RPC completes in well under a millisecond, so an RPC completion drains
 * the pending set long before any timer tick. Covering that branch needs a
 * flood RPC that never resolves, which requires a mock ClientType (KvStoreDb is
 * templated on it) rather than a real peer.
 */
TEST_F(
    KvStoreTestFixture, FloodBackpressureMinimumReconcileThresholdConverges) {
  fb303::fbData->resetAllData();

  constexpr size_t kNumKeys{30};
  const std::string publisherId{"reconcile-publisher"};

  auto publisherConf = getTinyBudgetKvConf(publisherId);
  // The floor Config::checkKvStoreConfig enforces: 2x the flood RPC timeout.
  publisherConf.flood_drain_reconcile_threshold_ms() =
      2 * Constants::kServiceProcTimeout.count();

  auto* publisher = createKvStore(publisherConf);
  auto* receiver = createKvStore(getPreCompressKvConf("reconcile-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  std::map<std::string, std::string> expectedKeyVals;
  for (size_t i = 0; i < kNumKeys; ++i) {
    const auto key = fmt::format("reconcile-key{}", i);
    const auto value = makeFloodValue(key);
    expectedKeyVals.emplace(key, value);

    auto thriftVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1, publisherId, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }

  EXPECT_GT(getCounterOrZero("kvstore.flood.backpressure_engaged.count"), 0);

  // No key may be lost, and the minimum valid threshold must not spuriously
  // trip wedge recovery while RPCs are legitimately in flight.
  for (const auto& [key, value] : expectedKeyVals) {
    EXPECT_TRUE(waitForKeyValue(receiver, kTestingAreaName, key, value))
        << "key " << key << " lost at the minimum reconcile threshold";
  }
  EXPECT_THAT(receiver->dumpAll(kTestingAreaName).size(), Eq(kNumKeys));
  EXPECT_THAT(getCounterOrZero("kvstore.flood.stuck_reconciled.count"), Eq(0));
}

/**
 * Guard the observability contract of the flood counters. Production data
 * showed the original shape was not operable: pending_keys read 0 across a full
 * day that contained real backpressure episodes, because an AVG stat truncates
 * a sub-sampling-interval transient to zero.
 *
 * Covers, under forced backpressure:
 *  - the sticky high-water marks record the peak the AVG stats lose;
 *  - deferral volume and coalescing are observable at all;
 *  - every flood counter is emitted area-tagged as well as globally, so an
 *    incident can be narrowed to one area of a multi-area node;
 *  - the resolved budget is exported, so outstanding_bytes is interpretable.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureCountersAreOperable) {
  fb303::fbData->resetAllData();
  resetFloodWatermarks();

  constexpr size_t kNumKeys{40};
  constexpr size_t kCapacityProbeBytes{1 << 20};
  const std::string publisherId{"counters-publisher"};
  const auto area = kTestingAreaName.t;
  const std::string capacityProbeValue(kCapacityProbeBytes, 'x');

  /*
   * Both stores get the same budget on purpose. kvstore.flood.budget_bytes is a
   * node-level setCounter, and this test process hosts two KvStore instances,
   * so the last one to initialize wins the counter. Production runs one KvStore
   * per process, where the value is unambiguous.
   */
  auto* publisher = createKvStore(getTinyBudgetKvConf(publisherId));
  auto* receiver = createKvStore(getTinyBudgetKvConf("counters-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  std::map<std::string, std::string> expectedKeyVals;
  for (size_t i = 0; i < kNumKeys; ++i) {
    const auto key = fmt::format("counters-key{}", i);
    const auto value = i == 0 ? capacityProbeValue : makeFloodValue(key);
    expectedKeyVals.emplace(key, value);
    auto thriftVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1, publisherId, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }
  for (const auto& [key, value] : expectedKeyVals) {
    EXPECT_TRUE(waitForKeyValue(receiver, kTestingAreaName, key, value));
  }

  const auto engaged =
      getCounterOrZero("kvstore.flood.backpressure_engaged.count");
  ASSERT_GT(engaged, 0) << "test did not reach backpressure";

  // Peaks survive even though the AVG stats average them away.
  EXPECT_GT(getCounterOrZero("kvstore.flood.pending_keys_max"), 0)
      << "pending-keys watermark missed an episode the AVG stat also hides";
  EXPECT_GE(
      getCounterOrZero("kvstore.flood.outstanding_bytes_max"),
      static_cast<int64_t>(capacityProbeValue.size()))
      << "resident IOBuf capacity was undercounted as compressed data length";
  EXPECT_GT(
      getCounterOrZero(fmt::format("kvstore.flood.pending_keys_max.{}", area)),
      0);

  /*
   * Deferral volume at all three granularities, not just episode count: keys
   * deferred (sum), publications deferred (count -- COUNT counts calls rather
   * than values, so one stat yields both), and distinct keys flushed on drain.
   */
  const auto deferredKeys =
      getCounterOrZero("kvstore.flood.num_deferred_keys.sum");
  const auto deferredPubs =
      getCounterOrZero("kvstore.flood.num_deferred_keys.count");
  const auto coalescedKeys =
      getCounterOrZero("kvstore.flood.num_coalesced_keys.sum");
  EXPECT_GT(deferredKeys, 0);
  EXPECT_GT(deferredPubs, 0);
  EXPECT_GT(coalescedKeys, 0);
  // A publication carries >= 1 key; an episode groups >= 1 publication.
  EXPECT_GE(deferredKeys, deferredPubs);
  EXPECT_GE(deferredPubs, engaged);
  /*
   * A drain never flushes more than was deferred into it. This test uses
   * distinct keys, so nothing is collapsed and the two are equal -- the strict
   * coalescing ratio is asserted in FloodBackpressureCoalescesToLatestValue,
   * which repeatedly overwrites a single key.
   */
  EXPECT_GE(deferredKeys, coalescedKeys);

  /*
   * Every flood counter is emitted area-tagged as well as globally with the
   * same value, so an incident on a multi-area node can be narrowed to one
   * area. engaged is separately asserted nonzero so the comparisons below
   * cannot all pass as 0 == 0; stuck_reconciled is legitimately 0 here.
   */
  EXPECT_GT(
      getCounterOrZero(
          fmt::format("kvstore.flood.backpressure_engaged.{}.count", area)),
      0)
      << "area-tagged engaged counter never incremented";
  for (const auto& stat :
       {std::string("backpressure_engaged"),
        std::string("backpressure_resolved"),
        std::string("stuck_reconciled")}) {
    const auto globalName = fmt::format("kvstore.flood.{}.count", stat);
    const auto taggedName =
        fmt::format("kvstore.flood.{}.{}.count", stat, area);
    EXPECT_THAT(getCounterOrZero(taggedName), Eq(getCounterOrZero(globalName)))
        << "area-tagged " << taggedName << " disagrees with " << globalName;
  }
  for (const auto& suffix : {std::string("sum"), std::string("count")}) {
    EXPECT_THAT(
        getCounterOrZero(
            fmt::format("kvstore.flood.num_deferred_keys.{}.{}", area, suffix)),
        Eq(getCounterOrZero(
            fmt::format("kvstore.flood.num_deferred_keys.{}", suffix))));
  }
  EXPECT_THAT(
      getCounterOrZero(
          fmt::format("kvstore.flood.num_coalesced_keys.{}.sum", area)),
      Eq(coalescedKeys));

  // The budget is exported so outstanding_bytes can be read as utilization,
  // once at node level with the configured value. It must NOT be area-tagged
  // (one node-level config applied to every area) and must NOT be summed
  // across areas, which is what routing it through getCounters() would do.
  EXPECT_THAT(getCounterOrZero("kvstore.flood.budget_bytes"), Eq(1));
  EXPECT_THAT(
      getCounterOrZero(fmt::format("kvstore.flood.budget_bytes.{}", area)),
      Eq(0))
      << "budget must not be area-tagged; it is not a per-area config";
}

/**
 * KvStoreParams is the single place the Constants fallback for the flood budget
 * knobs is applied, so verify both halves of that resolution directly rather
 * than only through end-to-end behavior.
 */
TEST_F(KvStoreTestFixture, FloodMemBudgetParamsResolution) {
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;
  messaging::ReplicateQueue<LogSample> logSampleQueue;

  // Unset in config -> Constants defaults.
  const KvStoreParams defaulted(
      getTestKvConf("params-default"), kvStoreUpdatesQueue, logSampleQueue);
  EXPECT_THAT(
      defaulted.floodMemBudgetBytes, Eq(Constants::kFloodMemBudgetBytes));
  EXPECT_THAT(
      defaulted.floodDrainReconcileThreshold,
      Eq(Constants::kFloodDrainReconcileThreshold));

  // Set in config -> config wins.
  auto overrideConf = getTestKvConf("params-override");
  overrideConf.flood_mem_budget_bytes() = 4096;
  overrideConf.flood_drain_reconcile_threshold_ms() = 10000;
  const KvStoreParams overridden(
      overrideConf, kvStoreUpdatesQueue, logSampleQueue);
  EXPECT_THAT(overridden.floodMemBudgetBytes, Eq(4096u));
  EXPECT_THAT(
      overridden.floodDrainReconcileThreshold,
      Eq(std::chrono::milliseconds(10000)));

  /*
   * KvStoreParams is constructed straight from a thrift::KvStoreConfig by
   * tests and direct embedders, bypassing Config::checkKvStoreConfig. Unsafe
   * values must not reach the members.
   *
   * A negative budget is the one that matters: the field is i64 and the member
   * size_t, so an unguarded static_cast turns -1 into SIZE_MAX and disables
   * the bound entirely instead of tightening it.
   */
  auto negBudgetConf = getTestKvConf("params-neg-budget");
  negBudgetConf.flood_mem_budget_bytes() = -1;
  const KvStoreParams negBudget(
      negBudgetConf, kvStoreUpdatesQueue, logSampleQueue);
  EXPECT_THAT(
      negBudget.floodMemBudgetBytes, Eq(Constants::kFloodMemBudgetBytes))
      << "negative budget wrapped to a huge size_t, disabling the bound";

  auto zeroBudgetConf = getTestKvConf("params-zero-budget");
  zeroBudgetConf.flood_mem_budget_bytes() = 0;
  const KvStoreParams zeroBudget(
      zeroBudgetConf, kvStoreUpdatesQueue, logSampleQueue);
  EXPECT_THAT(
      zeroBudget.floodMemBudgetBytes, Eq(Constants::kFloodMemBudgetBytes))
      << "zero budget would latch flooding off permanently";

  // Below the floor -> default, so live accounting cannot be reset under a
  // still-in-flight RPC.
  auto shortThresholdConf = getTestKvConf("params-short-threshold");
  shortThresholdConf.flood_drain_reconcile_threshold_ms() =
      Constants::kMinFloodDrainReconcileThreshold.count() - 1;
  const KvStoreParams shortThreshold(
      shortThresholdConf, kvStoreUpdatesQueue, logSampleQueue);
  EXPECT_THAT(
      shortThreshold.floodDrainReconcileThreshold,
      Eq(Constants::kFloodDrainReconcileThreshold));

  // Exactly at the floor is accepted verbatim.
  auto floorConf = getTestKvConf("params-floor-threshold");
  floorConf.flood_drain_reconcile_threshold_ms() =
      Constants::kMinFloodDrainReconcileThreshold.count();
  const KvStoreParams atFloor(floorConf, kvStoreUpdatesQueue, logSampleQueue);
  EXPECT_THAT(
      atFloor.floodDrainReconcileThreshold,
      Eq(Constants::kMinFloodDrainReconcileThreshold));

  kvStoreUpdatesQueue.close();
  logSampleQueue.close();
}

/**
 * Rollout interop: the pre-compression knob is per-node, so during a staged
 * rollout a compressing publisher will talk to a non-compressing peer and vice
 * versa. The receiver decompresses based on the frame's compression metadata,
 * not on its own config, so both directions must work.
 */
TEST_F(KvStoreTestFixture, FloodPreCompressionInteropAcrossMixedPeers) {
  fb303::fbData->resetAllData();

  const std::string compressingId{"interop-compressing"};
  const std::string legacyId{"interop-legacy"};

  auto* compressing = createKvStore(getPreCompressKvConf(compressingId));
  auto* legacy = createKvStore(getTestKvConf(legacyId)); // knob off
  compressing->run();
  legacy->run();

  EXPECT_THAT(
      compressing->addPeer(
          kTestingAreaName, legacy->getNodeId(), legacy->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      legacy->addPeer(
          kTestingAreaName,
          compressing->getNodeId(),
          compressing->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  const auto setKeyOn = [&](auto* store,
                            const std::string& originator,
                            const std::string& key,
                            const std::string& value) {
    auto thriftVal = createThriftValue(
        1 /* version */,
        originator /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1, originator, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(store->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  };

  // Compressing -> legacy.
  const auto fwdValue = makeFloodValue("interop-fwd");
  setKeyOn(compressing, compressingId, "interop-fwd-key", fwdValue);
  EXPECT_TRUE(
      waitForKeyValue(legacy, kTestingAreaName, "interop-fwd-key", fwdValue))
      << "pre-compressed flood was not decoded by a non-compressing peer";

  // Legacy -> compressing.
  const auto revValue = makeFloodValue("interop-rev");
  setKeyOn(legacy, legacyId, "interop-rev-key", revValue);
  EXPECT_TRUE(waitForKeyValue(
      compressing, kTestingAreaName, "interop-rev-key", revValue))
      << "uncompressed flood was not accepted by a compressing peer";

  expectPreCompressionPathExercised();
}

/**
 * Exercise the drain path's own fan-out. drainPendingFloods rebuilds the
 * coalesced params and caches serialized buffers per (protocol, fabric) across
 * peers, independently of the immediate flood path -- so multi-peer delivery
 * has to be asserted for drained keys specifically, not just for immediate
 * ones.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureDrainFansOutToAllPeers) {
  fb303::fbData->resetAllData();

  constexpr size_t kNumPeers{4};
  constexpr size_t kNumKeys{25};
  const std::string publisherId{"drain-fanout-publisher"};

  auto* publisher = createKvStore(getTinyBudgetKvConf(publisherId));
  publisher->run();

  std::vector<KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>*>
      peers;
  for (size_t i = 0; i < kNumPeers; ++i) {
    auto* peer =
        createKvStore(getPreCompressKvConf(getNodeId("drain-fanout-peer", i)));
    peer->run();
    peers.emplace_back(peer);
    EXPECT_THAT(
        publisher->addPeer(
            kTestingAreaName, peer->getNodeId(), peer->getPeerSpec()),
        IsTrue());
    EXPECT_THAT(
        peer->addPeer(
            kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
        IsTrue());
  }
  waitForAllPeersInitialized();

  std::map<std::string, std::string> expectedKeyVals;
  for (size_t i = 0; i < kNumKeys; ++i) {
    const auto key = fmt::format("drain-fanout-key{}", i);
    const auto value = makeFloodValue(key);
    expectedKeyVals.emplace(key, value);
    auto thriftVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            1, publisherId, thrift::Value().value() = std::string(value)));
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }

  EXPECT_GT(getCounterOrZero("kvstore.flood.backpressure_engaged.count"), 0);

  for (auto* peer : peers) {
    for (const auto& [key, value] : expectedKeyVals) {
      EXPECT_TRUE(waitForKeyValue(peer, kTestingAreaName, key, value))
          << "peer " << peer->getNodeId() << " never received drained key "
          << key;
    }
  }
}

/**
 * Leak detector for the byte accounting.
 *
 * Runs several bursts separated by quiescence. Every charge must be credited
 * back when its sends resolve; if any release were skipped,
 * areaOutstandingFloodBytes_ would ratchet permanently past the budget and both
 * gates (floodNow and drainPendingFloods) would latch closed, so a later burst
 * would never be delivered.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureRepeatedBurstsDoNotWedge) {
  fb303::fbData->resetAllData();

  constexpr size_t kNumBursts{3};
  constexpr size_t kKeysPerBurst{20};
  const std::string publisherId{"burst-publisher"};

  // 1-byte budget: every burst is guaranteed to go through defer -> drain, so
  // the wedge check below is exercised rather than depending on whether floods
  // happen to overlap.
  auto* publisher = createKvStore(getTinyBudgetKvConf(publisherId));
  auto* receiver = createKvStore(getPreCompressKvConf("burst-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  size_t totalKeys{0};
  for (size_t burst = 0; burst < kNumBursts; ++burst) {
    std::map<std::string, std::string> burstKeyVals;
    for (size_t i = 0; i < kKeysPerBurst; ++i) {
      const auto key = fmt::format("burst{}-key{}", burst, i);
      const auto value = makeFloodValue(key);
      burstKeyVals.emplace(key, value);
      auto thriftVal = createThriftValue(
          1 /* version */,
          publisherId /* originatorId */,
          value /* value */,
          Constants::kTtlInfinity /* ttl */,
          0 /* ttl version */,
          generateHash(
              1, publisherId, thrift::Value().value() = std::string(value)));
      EXPECT_THAT(
          publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
    }

    // Each burst must fully drain before the next one starts; a ratcheting
    // leak shows up as a burst that never arrives.
    for (const auto& [key, value] : burstKeyVals) {
      EXPECT_TRUE(waitForKeyValue(receiver, kTestingAreaName, key, value))
          << "burst " << burst << " key " << key
          << " never delivered -- flood budget may have leaked";
    }
    totalKeys += burstKeyVals.size();
    EXPECT_THAT(receiver->dumpAll(kTestingAreaName).size(), Eq(totalKeys));
  }

  // Every backpressure episode across every burst must have been resolved. A
  // skipped release would latch both gates closed and strand the pending set,
  // leaving engaged > resolved.
  const auto engaged =
      getCounterOrZero("kvstore.flood.backpressure_engaged.count");
  EXPECT_GT(engaged, 0);
  EXPECT_THAT(
      getCounterOrZero("kvstore.flood.backpressure_resolved.count"),
      Eq(engaged));
}

/**
 * Cover the documented echo caveat of the drain path.
 *
 * Deferred keys lose their originating senderId, so drainPendingFloods
 * re-floods the coalesced set to every peer including the node the keys came
 * from. The parent change argues this is safe and bounded: the receiver merges
 * an empty delta (counted as kvstore.received_redundant_publications), does not
 * re-flood it, and versions are monotonic so no sustained ping-pong is
 * possible. Both nodes are backpressured here so echoes flow in both
 * directions; the assertion is that traffic still converges and terminates.
 */
TEST_F(
    KvStoreTestFixture, FloodBackpressureSenderEchoConvergesWithoutPingPong) {
  fb303::fbData->resetAllData();

  constexpr size_t kNumKeysPerNode{15};
  const std::string nodeAId{"echo-node-a"};
  const std::string nodeBId{"echo-node-b"};

  auto* storeA = createKvStore(getTinyBudgetKvConf(nodeAId));
  auto* storeB = createKvStore(getTinyBudgetKvConf(nodeBId));
  storeA->run();
  storeB->run();

  EXPECT_THAT(
      storeA->addPeer(
          kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      storeB->addPeer(
          kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  std::map<std::string, std::string> expectedKeyVals;
  const auto publish = [&](auto* store,
                           const std::string& originator,
                           const std::string& prefix) {
    for (size_t i = 0; i < kNumKeysPerNode; ++i) {
      const auto key = fmt::format("{}-key{}", prefix, i);
      const auto value = makeFloodValue(key);
      expectedKeyVals.emplace(key, value);
      auto thriftVal = createThriftValue(
          1 /* version */,
          originator /* originatorId */,
          value /* value */,
          Constants::kTtlInfinity /* ttl */,
          0 /* ttl version */,
          generateHash(
              1, originator, thrift::Value().value() = std::string(value)));
      EXPECT_THAT(store->setKey(kTestingAreaName, key, thriftVal), IsTrue());
    }
  };
  publish(storeA, nodeAId, "echo-a");
  publish(storeB, nodeBId, "echo-b");

  EXPECT_GT(getCounterOrZero("kvstore.flood.backpressure_engaged.count"), 0);

  // Both nodes converge on the union despite echoes in both directions.
  for (auto* store : {storeA, storeB}) {
    for (const auto& [key, value] : expectedKeyVals) {
      EXPECT_TRUE(waitForKeyValue(store, kTestingAreaName, key, value))
          << store->getNodeId() << " missing " << key;
    }
    EXPECT_THAT(
        store->dumpAll(kTestingAreaName).size(), Eq(expectedKeyVals.size()));
  }

  // Traffic must terminate rather than ping-pong: once converged, letting the
  // stores idle produces no further flood publications.
  const auto floodsAfterConvergence =
      getCounterOrZero("kvstore.thrift.num_flood_pub.count");
  const auto idleUntil =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < idleUntil) {
    std::this_thread::yield();
  }
  EXPECT_THAT(
      getCounterOrZero("kvstore.thrift.num_flood_pub.count"),
      Eq(floodsAfterConvergence))
      << "flooding continued after convergence -- possible echo ping-pong";
}

/**
 * The budget and its accounting are per-area (areaOutstandingFloodBytes_ lives
 * on KvStoreDb, one instance per area), even though the knob is per-node.
 * Verify one area saturating its budget does not stall another area on the
 * same node.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureBudgetIsPerArea) {
  fb303::fbData->resetAllData();

  const AreaId areaOne{"flood-area-1"};
  const AreaId areaTwo{"flood-area-2"};
  const folly::F14FastSet<std::string> areaIds{areaOne.t, areaTwo.t};
  const std::string publisherId{"per-area-publisher"};

  auto* publisher = createKvStore(getTinyBudgetKvConf(publisherId), areaIds);
  auto* receiver =
      createKvStore(getPreCompressKvConf("per-area-receiver"), areaIds);
  publisher->run();
  receiver->run();

  for (const auto& area : {areaOne, areaTwo}) {
    EXPECT_THAT(
        publisher->addPeer(
            area, receiver->getNodeId(), receiver->getPeerSpec()),
        IsTrue());
    EXPECT_THAT(
        receiver->addPeer(
            area, publisher->getNodeId(), publisher->getPeerSpec()),
        IsTrue());
  }
  waitForAllPeersInitialized();

  // Drive both areas into backpressure concurrently.
  std::map<std::string, std::string> areaOneKeys;
  std::map<std::string, std::string> areaTwoKeys;
  for (size_t i = 0; i < 15; ++i) {
    for (const auto& [area, keys, prefix] :
         {std::tuple<AreaId, std::map<std::string, std::string>*, std::string>{
              areaOne, &areaOneKeys, "area1"},
          std::tuple<AreaId, std::map<std::string, std::string>*, std::string>{
              areaTwo, &areaTwoKeys, "area2"}}) {
      const auto key = fmt::format("{}-key{}", prefix, i);
      const auto value = makeFloodValue(key);
      keys->emplace(key, value);
      auto thriftVal = createThriftValue(
          1 /* version */,
          publisherId /* originatorId */,
          value /* value */,
          Constants::kTtlInfinity /* ttl */,
          0 /* ttl version */,
          generateHash(
              1, publisherId, thrift::Value().value() = std::string(value)));
      EXPECT_THAT(publisher->setKey(area, key, thriftVal), IsTrue());
    }
  }

  // Neither area may be starved by the other's backpressure.
  for (const auto& [key, value] : areaOneKeys) {
    EXPECT_TRUE(waitForKeyValue(receiver, areaOne, key, value))
        << "area-1 key " << key << " stalled";
  }
  for (const auto& [key, value] : areaTwoKeys) {
    EXPECT_TRUE(waitForKeyValue(receiver, areaTwo, key, value))
        << "area-2 key " << key << " stalled";
  }
  EXPECT_THAT(receiver->dumpAll(areaOne).size(), Eq(areaOneKeys.size()));
  EXPECT_THAT(receiver->dumpAll(areaTwo).size(), Eq(areaTwoKeys.size()));
}

/**
 * TTL-only refreshes go through the same flood path as value updates, so verify
 * they survive coalescing: the deferred set is re-derived from kvStore_, and a
 * ttl-version bump must not be lost or rolled back by a drain.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureTtlUpdatesConverge) {
  fb303::fbData->resetAllData();

  constexpr int64_t kNumTtlBumps{30};
  const std::string publisherId{"ttl-publisher"};
  const std::string key{"ttl-key"};
  const auto value = makeFloodValue("ttl-value");

  auto* publisher = createKvStore(getTinyBudgetKvConf(publisherId));
  auto* receiver = createKvStore(getPreCompressKvConf("ttl-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  for (int64_t ttlVersion = 1; ttlVersion <= kNumTtlBumps; ++ttlVersion) {
    auto thriftVal = createThriftValue(
        1 /* version */,
        publisherId /* originatorId */,
        value /* value */,
        300000 /* ttl */,
        ttlVersion /* ttl version */,
        0 /* hash */);
    thriftVal.hash() = generateHash(
        *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }

  // Converge on the newest ttlVersion, not a superseded one.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::optional<thrift::Value> received;
  while (std::chrono::steady_clock::now() < deadline) {
    received = receiver->getKey(kTestingAreaName, key);
    if (received.has_value() && *received->ttlVersion() == kNumTtlBumps) {
      break;
    }
    std::this_thread::yield();
  }
  ASSERT_TRUE(received.has_value());
  EXPECT_THAT(*received->ttlVersion(), Eq(kNumTtlBumps));
}

/**
 * The rate limiter and the flood memory budget both defer publications, but on
 * separate timers with deliberately opposite semantics:
 * pendingPublicationTimer_ debounces the rate-limiter buffer, while
 * floodDrainTimer_ is a deadline for the wedge check. Verify they compose: with
 * both active, updates are still delivered and the latest value wins.
 */
TEST_F(KvStoreTestFixture, FloodBackpressureComposesWithRateLimiter) {
  fb303::fbData->resetAllData();

  constexpr int64_t kNumUpdates{25};
  const std::string publisherId{"ratelimit-budget-publisher"};
  const std::string key{"ratelimit-budget-key"};

  auto publisherConf = getTinyBudgetKvConf(publisherId);
  publisherConf.flood_rate() =
      createKvStoreFloodRate(10 /* flood_msg_per_sec */, 5 /* burst */);

  auto* publisher = createKvStore(publisherConf);
  auto* receiver =
      createKvStore(getPreCompressKvConf("ratelimit-budget-receiver"));
  publisher->run();
  receiver->run();

  EXPECT_THAT(
      publisher->addPeer(
          kTestingAreaName, receiver->getNodeId(), receiver->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      receiver->addPeer(
          kTestingAreaName, publisher->getNodeId(), publisher->getPeerSpec()),
      IsTrue());
  waitForAllPeersInitialized();

  std::string finalValue;
  for (int64_t version = 1; version <= kNumUpdates; ++version) {
    finalValue = makeFloodValue(fmt::format("ratelimited-v{}", version));
    auto thriftVal = createThriftValue(
        version /* version */,
        publisherId /* originatorId */,
        finalValue /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            version,
            publisherId,
            thrift::Value().value() = std::string(finalValue)));
    EXPECT_THAT(publisher->setKey(kTestingAreaName, key, thriftVal), IsTrue());
  }

  EXPECT_TRUE(waitForKeyValue(receiver, kTestingAreaName, key, finalValue));
  const auto received = receiver->getKey(kTestingAreaName, key);
  ASSERT_TRUE(received.has_value());
  EXPECT_THAT(*received->version(), Eq(kNumUpdates));
}

/**
 * Verify that finalizeFullSync (the last step of the 3-way full-sync
 * handshake) filters fabric-internal keys when syncing to non-fabric peers.
 *
 * This test differs from FloodPublicationFabricScope by setting keys BEFORE
 * establishing peering, so the keys are exchanged via full-sync rather than
 * flood publication.
 *
 * Setup:
 *  - Node A (fabric node "eb01-ld002.dfw1") with fabricConfig — holds keys
 *  - Node B (fabric peer "eb01-sp002.dfw1") — matches spine regex
 *  - Node C (non-fabric peer "external-node") — no regex match
 *
 * Expected behavior after full-sync:
 *  - Node B receives all 3 keys (fabric + non-fabric)
 *  - Node C receives only the non-fabric key
 */
TEST_F(KvStoreTestFixture, FinalizeFullSyncFabricScope) {
  // Build FabricConfig with leaf/spine regexes
  thrift::FabricConfig thriftFabricConfig;
  thriftFabricConfig.fabric_name() = "bbf01.dfw";
  thriftFabricConfig.fabric_prefixes() = {"1::1/128"};
  thriftFabricConfig.fabric_leaf_regexes() = {"eb01-ld\\d{3}\\.dfw1"};
  thriftFabricConfig.fabric_spine_regexes() = {"eb01-sp\\d{3}\\.dfw1"};
  FabricConfig fabricConfig(thriftFabricConfig);

  const std::string nodeAId = "eb01-ld002.dfw1";
  const std::string nodeBId = "eb01-sp002.dfw1";
  const std::string nodeCId = "external-node";

  // Node A: fabric node (publisher) with fabricConfig
  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* storeA =
      createKvStore(
          getTestKvConf(nodeAId),
          {kTestingAreaName.t},
          std::nullopt,
          std::nullopt,
          fabricConfig);
  // Node B: fabric peer (spine name matches spine regex)
  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* storeB =
      createKvStore(getTestKvConf(nodeBId));
  // Node C: non-fabric peer (name does not match any regex)
  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* storeC =
      createKvStore(getTestKvConf(nodeCId));

  storeA->run();
  storeB->run();
  storeC->run();

  // Set keys on Node A BEFORE establishing peering so that they are
  // exchanged during the 3-way full-sync (finalizeFullSync), not via flood.
  const std::string fabricAdjKey = "adj:eb01-ld002.dfw1";
  const std::string fabricPrefixKey = "prefix:eb01-sp002.dfw1:[10.0.0.0/8]";
  const std::string nonFabricKey = "adj:external-node";

  const auto thriftVal = [&](const std::string& val) {
    return createThriftValue(
        1 /* version */,
        nodeAId /* originatorId */,
        val /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(1, nodeAId, thrift::Value().value() = std::string(val)));
  };

  EXPECT_THAT(
      storeA->setKey(kTestingAreaName, fabricAdjKey, thriftVal("fab-adj")),
      IsTrue());
  EXPECT_THAT(
      storeA->setKey(
          kTestingAreaName, fabricPrefixKey, thriftVal("fab-prefix")),
      IsTrue());
  EXPECT_THAT(
      storeA->setKey(kTestingAreaName, nonFabricKey, thriftVal("non-fab")),
      IsTrue());

  // Now establish bidirectional peering: A↔B
  EXPECT_THAT(
      storeA->addPeer(
          kTestingAreaName, storeB->getNodeId(), storeB->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      storeB->addPeer(
          kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()),
      IsTrue());

  // Establish bidirectional peering: A↔C
  EXPECT_THAT(
      storeA->addPeer(
          kTestingAreaName, storeC->getNodeId(), storeC->getPeerSpec()),
      IsTrue());
  EXPECT_THAT(
      storeC->addPeer(
          kTestingAreaName, storeA->getNodeId(), storeA->getPeerSpec()),
      IsTrue());

  // Wait for full-sync to complete
  waitForAllPeersInitialized();

  // Wait for the non-fabric key to arrive at both peers
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, nonFabricKey);
  waitForKeyInStoreWithTimeout(storeC, kTestingAreaName, nonFabricKey);

  // Wait for fabric keys to arrive at B
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, fabricAdjKey);
  waitForKeyInStoreWithTimeout(storeB, kTestingAreaName, fabricPrefixKey);

  // Node B (fabric peer): should have ALL 3 keys after full-sync
  folly::F14FastMap<std::string, thrift::Value> dumpB =
      storeB->dumpAll(kTestingAreaName);
  EXPECT_THAT(dumpB.count(fabricAdjKey), Eq(1));
  EXPECT_THAT(dumpB.count(fabricPrefixKey), Eq(1));
  EXPECT_THAT(dumpB.count(nonFabricKey), Eq(1));

  // Node C (non-fabric peer): should have ONLY the non-fabric key
  folly::F14FastMap<std::string, thrift::Value> dumpC =
      storeC->dumpAll(kTestingAreaName);
  EXPECT_THAT(dumpC.count(nonFabricKey), Eq(1));
  EXPECT_THAT(dumpC.count(fabricAdjKey), Eq(0))
      << "Fabric adj key should NOT be synced to non-fabric peer";
  EXPECT_THAT(dumpC.count(fabricPrefixKey), Eq(0))
      << "Fabric prefix key should NOT be synced to non-fabric peer";
}

/**
 * Verify that semifuture_dumpKvStoreKeys applies fabric filtering based on
 * the senderId in KeyDumpParams. When a non-fabric sender requests a dump,
 * fabric-internal keys should be excluded from the response.
 *
 * Setup:
 *  - Single fabric node ("eb01-ld002.dfw1") with fabricConfig holding 3 keys
 *
 * Scenarios:
 *  - Dump with senderId = fabric peer name → returns all 3 keys
 *  - Dump with senderId = non-fabric peer name → returns only non-fabric key
 *  - Dump with no senderId → returns all 3 keys (no filtering)
 */
TEST_F(KvStoreTestFixture, DumpKvStoreKeysFabricScope) {
  // Build FabricConfig with leaf/spine regexes
  thrift::FabricConfig thriftFabricConfig;
  thriftFabricConfig.fabric_name() = "bbf01.dfw";
  thriftFabricConfig.fabric_prefixes() = {"1::1/128"};
  thriftFabricConfig.fabric_leaf_regexes() = {"eb01-ld\\d{3}\\.dfw1"};
  thriftFabricConfig.fabric_spine_regexes() = {"eb01-sp\\d{3}\\.dfw1"};
  FabricConfig fabricConfig(thriftFabricConfig);

  const std::string nodeId = "eb01-ld002.dfw1";

  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* store =
      createKvStore(
          getTestKvConf(nodeId),
          {kTestingAreaName.t},
          std::nullopt,
          std::nullopt,
          fabricConfig);
  store->run();

  // Set fabric and non-fabric keys
  const std::string fabricAdjKey = "adj:eb01-ld002.dfw1";
  const std::string fabricPrefixKey = "prefix:eb01-sp002.dfw1:[10.0.0.0/8]";
  const std::string nonFabricKey = "adj:external-node";

  const auto thriftVal = [&](const std::string& val) {
    return createThriftValue(
        1 /* version */,
        nodeId /* originatorId */,
        val /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(1, nodeId, thrift::Value().value() = std::string(val)));
  };

  EXPECT_THAT(
      store->setKey(kTestingAreaName, fabricAdjKey, thriftVal("fab-adj")),
      IsTrue());
  EXPECT_THAT(
      store->setKey(kTestingAreaName, fabricPrefixKey, thriftVal("fab-prefix")),
      IsTrue());
  EXPECT_THAT(
      store->setKey(kTestingAreaName, nonFabricKey, thriftVal("non-fab")),
      IsTrue());

  // Helper to call semifuture_dumpKvStoreKeys and return the key-value map
  const auto dumpWithSender = [&](const std::string& senderId)
      -> folly::F14FastMap<std::string, thrift::Value> {
    thrift::KeyDumpParams params;
    if (!senderId.empty()) {
      params.senderId() = senderId;
    }
    std::vector<thrift::Publication> pubs =
        *store->getKvStore()
             ->semifuture_dumpKvStoreKeys(
                 std::move(params), {kTestingAreaName.t})
             .get();
    EXPECT_THAT(pubs.size(), Eq(1));
    const auto& kvs = *pubs.begin()->keyVals();
    return folly::F14FastMap<std::string, thrift::Value>(
        kvs.begin(), kvs.end());
  };

  // Scenario 1: fabric peer sender → all keys returned
  {
    folly::F14FastMap<std::string, thrift::Value> dump =
        dumpWithSender("eb01-sp002.dfw1");
    EXPECT_THAT(dump.count(fabricAdjKey), Eq(1));
    EXPECT_THAT(dump.count(fabricPrefixKey), Eq(1));
    EXPECT_THAT(dump.count(nonFabricKey), Eq(1));
  }

  // Scenario 2: non-fabric peer sender → only non-fabric key returned
  {
    folly::F14FastMap<std::string, thrift::Value> dump =
        dumpWithSender("external-node");
    EXPECT_THAT(dump.count(nonFabricKey), Eq(1));
    EXPECT_THAT(dump.count(fabricAdjKey), Eq(0))
        << "Fabric adj key should NOT be dumped for non-fabric sender";
    EXPECT_THAT(dump.count(fabricPrefixKey), Eq(0))
        << "Fabric prefix key should NOT be dumped for non-fabric sender";
  }

  // Scenario 3: no senderId → all keys returned (no filtering)
  {
    folly::F14FastMap<std::string, thrift::Value> dump = dumpWithSender("");
    EXPECT_THAT(dump.count(fabricAdjKey), Eq(1));
    EXPECT_THAT(dump.count(fabricPrefixKey), Eq(1));
    EXPECT_THAT(dump.count(nonFabricKey), Eq(1));
  }
}

/**
 * Verify that semifuture_dumpKvStoreSelfOriginatedKeys returns ALL
 * self-originated keys on a fabric node, including fabric-internal keys.
 * This API has no fabric filtering — the node should always be able to
 * inspect its own self-originated keys.
 *
 * Setup:
 *  - Single fabric node ("eb01-ld002.dfw1") with fabricConfig
 *  - Self-originate 3 keys via kvRequestQueue: 2 fabric + 1 non-fabric
 *
 * Expected:
 *  - dumpAllSelfOriginated returns all 3 keys
 */
TEST_F(KvStoreTestFixture, DumpSelfOriginatedKeysFabricScope) {
  // Build FabricConfig with leaf/spine regexes
  thrift::FabricConfig thriftFabricConfig;
  thriftFabricConfig.fabric_name() = "bbf01.dfw";
  thriftFabricConfig.fabric_prefixes() = {"1::1/128"};
  thriftFabricConfig.fabric_leaf_regexes() = {"eb01-ld\\d{3}\\.dfw1"};
  thriftFabricConfig.fabric_spine_regexes() = {"eb01-sp\\d{3}\\.dfw1"};
  FabricConfig fabricConfig(thriftFabricConfig);

  const std::string nodeId = "eb01-ld002.dfw1";

  // Create kvRequestQueue to push self-originated keys
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue;
  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* store =
      createKvStore(
          getTestKvConf(nodeId),
          {kTestingAreaName.t},
          std::nullopt,
          kvRequestQueue.getReader(),
          fabricConfig);
  store->run();

  // Self-originate fabric and non-fabric keys
  const std::string fabricAdjKey = "adj:eb01-ld002.dfw1";
  const std::string fabricPrefixKey = "prefix:eb01-sp002.dfw1:[10.0.0.0/8]";
  const std::string nonFabricKey = "adj:external-node";

  kvRequestQueue.push(
      PersistKeyValueRequest(kTestingAreaName, fabricAdjKey, "fab-adj"));
  kvRequestQueue.push(
      PersistKeyValueRequest(kTestingAreaName, fabricPrefixKey, "fab-prefix"));
  kvRequestQueue.push(
      PersistKeyValueRequest(kTestingAreaName, nonFabricKey, "non-fab"));

  // Wait for all keys to appear in the store
  waitForKeyInStoreWithTimeout(store, kTestingAreaName, fabricAdjKey);
  waitForKeyInStoreWithTimeout(store, kTestingAreaName, fabricPrefixKey);
  waitForKeyInStoreWithTimeout(store, kTestingAreaName, nonFabricKey);

  // dumpAllSelfOriginated should return ALL 3 keys — no fabric filtering
  SelfOriginatedKeyVals selfOriginated =
      store->dumpAllSelfOriginated(kTestingAreaName);
  EXPECT_THAT(selfOriginated.size(), Eq(3));
  EXPECT_THAT(selfOriginated.count(fabricAdjKey), Eq(1));
  EXPECT_THAT(selfOriginated.count(fabricPrefixKey), Eq(1));
  EXPECT_THAT(selfOriginated.count(nonFabricKey), Eq(1));
}

/**
 * Verify that co_dumpKvStoreHashes applies fabric filtering based on
 * the senderId in KeyDumpParams. When a non-fabric sender requests a hash
 * dump, fabric-internal key hashes should be excluded from the response.
 *
 * Setup:
 *  - Single fabric node ("eb01-ld002.dfw1") with fabricConfig holding 3 keys
 *
 * Scenarios:
 *  - Hash dump with senderId = fabric peer name → returns hashes for all 3 keys
 *  - Hash dump with senderId = non-fabric peer name → returns hash for only
 *    non-fabric key
 *  - Hash dump with no senderId → returns hashes for all 3 keys (no filtering)
 */
CO_TEST_F(KvStoreTestFixture, DumpKvStoreHashesFabricScope) {
  // Build FabricConfig with leaf/spine regexes
  thrift::FabricConfig thriftFabricConfig;
  thriftFabricConfig.fabric_name() = "bbf01.dfw";
  thriftFabricConfig.fabric_prefixes() = {"1::1/128"};
  thriftFabricConfig.fabric_leaf_regexes() = {"eb01-ld\\d{3}\\.dfw1"};
  thriftFabricConfig.fabric_spine_regexes() = {"eb01-sp\\d{3}\\.dfw1"};
  FabricConfig fabricConfig(thriftFabricConfig);

  const std::string nodeId = "eb01-ld002.dfw1";

  KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>* store =
      createKvStore(
          getTestKvConf(nodeId),
          {kTestingAreaName.t},
          std::nullopt,
          std::nullopt,
          fabricConfig);
  store->run();

  // Set fabric and non-fabric keys
  const std::string fabricAdjKey = "adj:eb01-ld002.dfw1";
  const std::string fabricPrefixKey = "prefix:eb01-sp002.dfw1:[10.0.0.0/8]";
  const std::string nonFabricKey = "adj:external-node";

  const auto thriftVal = [&](const std::string& val) {
    return createThriftValue(
        1 /* version */,
        nodeId /* originatorId */,
        val /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(1, nodeId, thrift::Value().value() = std::string(val)));
  };

  EXPECT_THAT(
      store->setKey(kTestingAreaName, fabricAdjKey, thriftVal("fab-adj")),
      IsTrue());
  EXPECT_THAT(
      store->setKey(kTestingAreaName, fabricPrefixKey, thriftVal("fab-prefix")),
      IsTrue());
  EXPECT_THAT(
      store->setKey(kTestingAreaName, nonFabricKey, thriftVal("non-fab")),
      IsTrue());

  // Helper to call co_dumpKvStoreHashes and return the key-value map
  const auto dumpHashesWithSender = [&](const std::string& senderId)
      -> folly::coro::Task<folly::F14FastMap<std::string, thrift::Value>> {
    thrift::KeyDumpParams params;
    if (!senderId.empty()) {
      params.senderId() = senderId;
    }
    auto pub = co_await store->getKvStore()->co_dumpKvStoreHashes(
        kTestingAreaName.t, std::move(params));
    const auto& kvs = *pub->keyVals();
    co_return folly::F14FastMap<std::string, thrift::Value>(
        kvs.begin(), kvs.end());
  };

  // Scenario 1: fabric peer sender → hashes for all keys returned
  {
    folly::F14FastMap<std::string, thrift::Value> dump =
        co_await dumpHashesWithSender("eb01-sp002.dfw1");
    EXPECT_THAT(dump.count(fabricAdjKey), Eq(1));
    EXPECT_THAT(dump.count(fabricPrefixKey), Eq(1));
    EXPECT_THAT(dump.count(nonFabricKey), Eq(1));
  }

  // Scenario 2: non-fabric peer sender → only non-fabric key hash returned
  {
    folly::F14FastMap<std::string, thrift::Value> dump =
        co_await dumpHashesWithSender("external-node");
    EXPECT_THAT(dump.count(nonFabricKey), Eq(1));
    EXPECT_THAT(dump.count(fabricAdjKey), Eq(0))
        << "Fabric adj key hash should NOT be dumped for non-fabric sender";
    EXPECT_THAT(dump.count(fabricPrefixKey), Eq(0))
        << "Fabric prefix key hash should NOT be dumped for non-fabric sender";
  }

  // Scenario 3: no senderId → hashes for all keys returned (no filtering)
  {
    folly::F14FastMap<std::string, thrift::Value> dump =
        co_await dumpHashesWithSender("");
    EXPECT_THAT(dump.count(fabricAdjKey), Eq(1));
    EXPECT_THAT(dump.count(fabricPrefixKey), Eq(1));
    EXPECT_THAT(dump.count(nonFabricKey), Eq(1));
  }
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
