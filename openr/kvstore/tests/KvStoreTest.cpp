/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/init/Init.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;
using namespace std::chrono;

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

// [SECURE CONFIG OVERRIDE]
thrift::KvStoreConfig
getSecureTestKvConf(std::string nodeId) {
  thrift::KvStoreConfig kvConf;
  kvConf.node_name() = nodeId;
  kvConf.enable_secure_thrift_client() = true;

  kvConf.x509_cert_path() = fmt::format("{}certs/test_cert1.pem", kTestDir);
  kvConf.x509_key_path() = fmt::format("{}certs/test_key1.pem", kTestDir);
  kvConf.x509_ca_path() = fmt::format("{}certs/ca_cert.pem", kTestDir);
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
   * Retured raw pointer of an object will be freed as well.
   */
  KvStoreWrapper<thrift::KvStoreServiceAsyncClient>*
  createKvStore(
      thrift::KvStoreConfig kvStoreConf,
      const std::unordered_set<std::string>& areaIds = {kTestingAreaName},
      std::optional<messaging::RQueue<PeerEvent>> peerUpdatesQueue =
          std::nullopt,
      std::optional<messaging::RQueue<KeyValueRequest>> kvRequestQueue =
          std::nullopt) {
    stores_.emplace_back(
        std::make_unique<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            areaIds, kvStoreConf, peerUpdatesQueue, kvRequestQueue));
    return stores_.back().get();
  }

  /**
   * Utility function to create a nodeId/originId based on it's index and
   * prefix
   */
  std::string
  getNodeId(const std::string& prefix, const int index) const {
    return fmt::format("{}{}", prefix, index);
  }

  void
  waitForAllPeersInitialized() const {
    bool allInitialized = false;
    while (not allInitialized) {
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
      KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* store,
      AreaId const& areaId,
      std::string const& key) const {
    auto const start = std::chrono::steady_clock::now();
    while (not store->getKey(areaId, key).has_value() &&
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
  std::vector<
      std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>
      stores_{};
};

} // namespace

/**
 * Validate retrieval of a single key-value from a node's KvStore.
 */
TEST_F(KvStoreTestFixture, BasicGetKey) {
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
  auto pub = *kvStore_->getKvStore()
                  ->semifuture_getKvStoreKeyVals(kTestingAreaName, paramsBefore)
                  .get();
  auto it = pub.keyVals()->find(key);
  EXPECT_EQ(it, pub.keyVals()->end());

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
  auto pubAfter =
      *kvStore_->getKvStore()
           ->semifuture_getKvStoreKeyVals(kTestingAreaName, paramsAfter)
           .get();
  auto itAfter = pubAfter.keyVals()->find(key);
  EXPECT_NE(itAfter, pubAfter.keyVals()->end());
  auto& valueFromStore = itAfter->second;
  EXPECT_EQ(valueFromStore.value(), value);
  EXPECT_EQ(*valueFromStore.version(), 1);
  EXPECT_EQ(*valueFromStore.ttlVersion(), 0);
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
    auto pub = co_await kvStore_->getKvStore()->co_dumpKvStoreKeys(
        std::move(params), {kTestingAreaName.t});
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
    auto pub = co_await kvStore_->getKvStore()->co_dumpKvStoreKeys(
        std::move(params), {kTestingAreaName.t});
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
  EXPECT_GT(elapsedTime, ksyncInitialBackoff.count());
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
  EXPECT_GT(elapsedTime, ksyncInitialBackoff.count());
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
  thrift::KeyVals expectedKeyVals;
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
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  kvStore->setKey(kTestingAreaName, "test-key2", thriftVal1);

  // Set same key with different value
  thrift::Value thriftVal2 = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      std::string("value2") /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  kvStore->setKey(kTestingAreaName, "test-key2", thriftVal2);

  // Wait till counters updated
  std::this_thread::sleep_for(std::chrono::milliseconds(counterUpdateWaitTime));
  auto counters = fb303::fbData->getCounters();

  // Verify the counter keys exist
  ASSERT_TRUE(counters.count("kvstore.num_peers"));
  ASSERT_TRUE(counters.count("kvstore.cmd_peer_dump.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_peer_add.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_per_del.count"));
  ASSERT_TRUE(counters.count("kvstore.expired_key_vals.sum"));
  ASSERT_TRUE(counters.count("kvstore.thrift.flood_pub_duration_ms.avg"));
  ASSERT_TRUE(counters.count("kvstore.thrift.full_sync_duration_ms.avg"));
  ASSERT_TRUE(counters.count("kvstore.thrift.finalized_sync_duration_ms.avg"));
  ASSERT_TRUE(counters.count("kvstore.rate_limit_keys.avg"));
  ASSERT_TRUE(counters.count("kvstore.rate_limit_suppress.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_hash_dump.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_self_originated_key_dump.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_key_dump.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_key_get.count"));
  ASSERT_TRUE(counters.count("kvstore.updated_key_vals." + area + ".sum"));
  ASSERT_TRUE(counters.count("kvstore.received_key_vals." + area + ".sum"));
  ASSERT_TRUE(
      counters.count("kvstore.received_publications." + area + ".count"));
  ASSERT_TRUE(counters.count("kvstore.num_flood_peers"));
  ASSERT_TRUE(counters.count("kvstore.num_flood_peers." + area + ".sum"));
  ASSERT_TRUE(counters.count("kvstore.num_expiring_keys"));
  ASSERT_TRUE(counters.count("kvstore.num_expiring_keys." + area + ".sum"));

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
  ASSERT_EQ(1, counters.count("kvstore.cmd_key_set.count"));
  EXPECT_EQ(4, counters.at("kvstore.cmd_key_set.count"));
  ASSERT_EQ(1, counters.count("kvstore.received_key_vals.sum"));
  EXPECT_EQ(3, counters.at("kvstore.received_key_vals.sum"));
  ASSERT_EQ(1, counters.count("kvstore.received_key_vals." + area + ".sum"));
  EXPECT_EQ(3, counters.at("kvstore.received_key_vals." + area + ".sum"));

  // Verify the key and the number of key
  ASSERT_TRUE(kvStore->getKey(kTestingAreaName, "test-key2").has_value());
  ASSERT_EQ(1, counters.count("kvstore.num_keys"));
  int expect_num_key = 1;
  EXPECT_EQ(expect_num_key, counters.at("kvstore.num_keys"));

  // Verify the number key update
  ASSERT_EQ(1, counters.count("kvstore.updated_key_vals.sum"));
  EXPECT_EQ(2, counters.at("kvstore.updated_key_vals.sum"));
  ASSERT_EQ(1, counters.count("kvstore.updated_key_vals." + area + ".sum"));
  EXPECT_EQ(2, counters.at("kvstore.updated_key_vals." + area + ".sum"));

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
  EXPECT_EQ(expect_num_key, counters.at("kvstore.num_keys"));

  LOG(INFO) << "Counters received, yo";
}

/**
 * Test following with single KvStore.
 * - TTL propagation is carried out correctly
 * - Correct TTL reflects back in GET/KEY_DUMP/KEY_HASH
 * - Applying ttl updates reflects properly
 */
TEST_F(KvStoreTestFixture, TtlVerification) {
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
    ASSERT_EQ(1, publication.expiredKeys()->size());
    EXPECT_EQ(key, publication.expiredKeys()->at(0));
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
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl(), *getRes->ttl() + 1);
    getRes->ttl() = *thriftValue.ttl();
    getRes->hash() = 0;
    EXPECT_EQ(thriftValue, getRes.value());

    // dump keys
    auto dumpRes = kvStore->dumpAll(kTestingAreaName);
    EXPECT_EQ(1, dumpRes.size());
    ASSERT_EQ(1, dumpRes.count(key));
    auto& dumpResValue = dumpRes.at(key);
    EXPECT_GE(*thriftValue.ttl(), *dumpResValue.ttl() + 1);
    dumpResValue.ttl() = *thriftValue.ttl();
    dumpResValue.hash() = 0;
    EXPECT_EQ(thriftValue, dumpResValue);

    // dump hashes
    auto hashRes = kvStore->dumpHashes(kTestingAreaName);
    EXPECT_EQ(1, hashRes.size());
    ASSERT_EQ(1, hashRes.count(key));
    auto& hashResValue = hashRes.at(key);
    EXPECT_GE(*thriftValue.ttl(), *hashResValue.ttl() + 1);
    hashResValue.ttl() = *thriftValue.ttl();
    hashResValue.hash() = 0;
    hashResValue.value().copy_from(thriftValue.value());
    EXPECT_EQ(thriftValue, hashResValue);

    // We will receive key-advertisement
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals()->size());
    ASSERT_EQ(0, publication.expiredKeys()->size());
    ASSERT_EQ(1, publication.keyVals()->count(key));
    auto& pubValue = publication.keyVals()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_GE(*thriftValue.ttl(), *pubValue.ttl() + 1);
    pubValue.ttl() = *thriftValue.ttl();
    pubValue.hash() = 0;
    EXPECT_EQ(thriftValue, pubValue);
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
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl(), *getRes->ttl() + 1);
    EXPECT_EQ(*thriftValue.version(), *getRes->version());
    EXPECT_EQ(*thriftValue.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
    EXPECT_EQ(value.value(), getRes->value());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals()->size());
    ASSERT_EQ(0, publication.expiredKeys()->size());
    ASSERT_EQ(1, publication.keyVals()->count(key));
    auto& pubValue = publication.keyVals()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_FALSE(pubValue.value().has_value());
    EXPECT_GE(*thriftValue.ttl(), *pubValue.ttl() + 1);
    EXPECT_EQ(*thriftValue.version(), *pubValue.version());
    EXPECT_EQ(*thriftValue.originatorId(), *pubValue.originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *pubValue.ttlVersion());
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
    ASSERT_TRUE(getRes.has_value());
    EXPECT_EQ(Constants::kTtlInfinity, *getRes->ttl());
    EXPECT_EQ(*thriftValue.version(), *getRes->version());
    EXPECT_EQ(*thriftValue.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
    EXPECT_EQ(value.value(), getRes->value());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals()->size());
    ASSERT_EQ(0, publication.expiredKeys()->size());
    ASSERT_EQ(1, publication.keyVals()->count(key));
    auto& pubValue = publication.keyVals()->at(key);
    // TTL should remain infinite
    EXPECT_FALSE(pubValue.value().has_value());
    EXPECT_EQ(Constants::kTtlInfinity, *pubValue.ttl());
    EXPECT_EQ(*thriftValue.version(), *pubValue.version());
    EXPECT_EQ(*thriftValue.originatorId(), *pubValue.originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *pubValue.ttlVersion());
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
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl(), *getRes->ttl() + 1);
    EXPECT_EQ(*thriftValue.version(), *getRes->version());
    EXPECT_EQ(*thriftValue.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *getRes->ttlVersion());
    EXPECT_EQ(value.value(), getRes->value());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals()->size());
    ASSERT_EQ(0, publication.expiredKeys()->size());
    ASSERT_EQ(1, publication.keyVals()->count(key));
    auto& pubValue = publication.keyVals()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_FALSE(pubValue.value().has_value());
    EXPECT_GE(*thriftValue.ttl(), *pubValue.ttl() + 1);
    EXPECT_EQ(*thriftValue.version(), *pubValue.version());
    EXPECT_EQ(*thriftValue.originatorId(), *pubValue.originatorId());
    EXPECT_EQ(*thriftValue.ttlVersion(), *pubValue.ttlVersion());
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
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(20000, *getRes->ttl()); // Previous ttl was set to 20s
    EXPECT_LE(10000, *getRes->ttl());
    EXPECT_EQ(*value.version(), *getRes->version());
    EXPECT_EQ(*value.originatorId(), *getRes->originatorId());
    EXPECT_EQ(*value.ttlVersion() + 3, *getRes->ttlVersion());
    EXPECT_EQ(value.value(), getRes->value());
  }
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
  std::unordered_map<std::string, thrift::PeerSpec> expectedPeers = {
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
  std::vector<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>*> peerStores;
  for (unsigned int j = 0; j < kNumStores; ++j) {
    auto nodeId = getNodeId(kOriginBase, j);
    auto store = createKvStore(getTestKvConf(nodeId));
    store->run();
    peerStores.push_back(store);
  }

  // Submit initial value set into all peerStores
  thrift::KeyVals expectedKeyVals;
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
    std::unordered_set<std::string> keys;
    VLOG(3) << "Store " << store->getNodeId() << " received keys.";
    while (keys.size() < kNumStores) {
      auto publication = store->recvPublication();
      for (auto const& [key, val] : *publication.keyVals()) {
        VLOG(3) << "\tkey: " << key << ", value: " << val.value().value();
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
    std::unordered_set<std::string> keys;
    VLOG(3) << "Store " << store->getNodeId() << " received keys.";
    while (keys.size() < kNumStores) {
      auto publication = store->recvPublication();
      for (auto const& [key, val] : *publication.keyVals()) {
        VLOG(3) << "\tkey: " << key << ", value: " << val.value().value();
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
      VLOG(2) << "Receiving publication from " << store->getNodeId();
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
  std::vector<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>*> stores;
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
  EXPECT_TRUE(stores[kNumStores - 1]->setKey(
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
  std::vector<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>*> peerStores;
  for (unsigned int j = 0; j < kNumStores; ++j) {
    auto store = createKvStore(getTestKvConf(getNodeId(kOriginBase, j)));
    store->run();
    peerStores.emplace_back(store);
  }

  // Submit initial value set into all peerStores
  LOG(INFO) << "Submitting initial key-value pairs into peer stores.";

  thrift::KeyVals expectedKeyVals;
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
    VLOG(3) << "Store " << myStore->getNodeId() << " received keys.";

    std::unordered_set<std::string> keys;
    while (keys.size() < kNumStores) {
      auto publication = myStore->recvPublication();
      for (auto const& [key, val] : *publication.keyVals()) {
        VLOG(3) << "\tkey: " << key << ", value: " << val.value().value();
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

  thrift::KeyVals expectedKeyVals;
  thrift::KeyVals peerKeyVals;
  thrift::KeyVals diffKeyVals;
  const thrift::KeyVals emptyKeyVals;
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

  // check number of sent publications should be at least number of keys set
  EXPECT_GE(s0PubSent1 - i1, 0);
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
TEST_F(KvStoreTestFixture, KeySyncMultipleArea) {
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

  thrift::KeyVals expectedKeyValsPod{};
  thrift::KeyVals expectedKeyValsPlane{};

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
    std::unordered_map<std::string, thrift::PeerSpec> expectedPeersPod = {
        {storeANodeId, podPeerSpec},
    };

    auto storeCNodeId = storeC->getNodeId();
    auto planePeerSpec =
        storeC->getPeerSpec(thrift::KvStorePeerState::INITIALIZED);
    std::unordered_map<std::string, thrift::PeerSpec> expectedPeersPlane = {
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

    auto summary = storeA->getSummary(areaSetAll);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(*summary.at(0).area(), *pod.area_id());
    EXPECT_EQ(keyVal0Size + keyVal1Size, *summary.at(0).keyValsBytes());

    summary = storeA->getSummary(areaSetEmpty);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(*summary.at(0).area(), *pod.area_id());
    EXPECT_EQ(keyVal0Size + keyVal1Size, *summary.at(0).keyValsBytes());

    summary = storeB->getSummary(areaSetAll);
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

    summary = storeB->getSummary(areaSetEmpty);
    EXPECT_EQ(2, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(2, *summary.at(1).keyValsCount());

    summary = storeC->getSummary(areaSetAll);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, *summary.at(0).keyValsCount());
    EXPECT_EQ(*summary.at(0).area(), *plane.area_id());
    EXPECT_EQ(keyVal2Size + keyVal3Size, *summary.at(0).keyValsBytes());

    summary = storeC->getSummary(areaSetEmpty);
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

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
