/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <tuple>

#include <fb303/ServiceData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/Random.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;
using namespace std::chrono;

namespace fb303 = facebook::fb303;

namespace {

// TTL in ms
const int64_t kTtlMs = 1000;

// wait time before checking counter
const std::chrono::milliseconds counterUpdateWaitTime(5500);

// Timeout of checking peers in all KvStores are initialized.
const std::chrono::milliseconds kTimeoutOfAllPeersInitialized(1000);

// Timeout of checking keys are propagated in all KvStores in the same area.
const std::chrono::milliseconds kTimeoutOfKvStorePropagation(500);

thrift::KvstoreConfig
getTestKvConf() {
  thrift::KvstoreConfig kvConf;
  kvConf.enable_thrift_dual_msg_ref() = false;
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
    // nothing to do
    for (auto& store : stores_) {
      store->stop();
    }
    stores_.clear();
  }

  /**
   * Helper function to create KvStoreWrapper. The underlying stores will be
   * stopped as well as destroyed automatically when test exits.
   * Retured raw pointer of an object will be freed as well.
   */
  KvStoreWrapper*
  createKvStore(
      std::string nodeId,
      thrift::KvstoreConfig kvStoreConf = getTestKvConf(),
      const std::vector<thrift::AreaConfig>& areas = {},
      std::optional<messaging::RQueue<PeerEvent>> peerUpdatesQueue =
          std::nullopt) {
    auto tConfig = getBasicOpenrConfig(nodeId, "domain", areas);
    tConfig.kvstore_config_ref() = kvStoreConf;
    auto config = std::make_shared<Config>(tConfig);

    stores_.emplace_back(
        std::make_unique<KvStoreWrapper>(context_, config, peerUpdatesQueue));
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
    auto const start = std::chrono::steady_clock::now();
    while (not allInitialized &&
           (std::chrono::steady_clock::now() - start <
            kTimeoutOfAllPeersInitialized)) {
      std::this_thread::yield();
      allInitialized = true;
      for (auto const& store : stores_) {
        for (auto const& area : store->getAreaIds()) {
          for (auto const& [_, spec] : store->getPeers(AreaId{area})) {
            allInitialized &=
                ((spec.get_state() == thrift::KvStorePeerState::INITIALIZED)
                     ? 1
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
      KvStoreWrapper* store,
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

  // Public member variables
  fbzmq::Context context_;

  // Internal stores
  std::vector<std::unique_ptr<KvStoreWrapper>> stores_{};
};

} // namespace

/**
 * Validate retrieval of a single key-value from a node's KvStore.
 */
TEST_F(KvStoreTestFixture, BasicGetKey) {
  // Create and start KvStore.
  const std::string nodeId = "node-for-retrieval";
  auto kvStore_ = createKvStore(nodeId);
  kvStore_->run();

  const std::string key = "get-key-key";
  const std::string value = "get-key-value";

  // 1. Get key. Make sure it doesn't exist in KvStore yet.
  // 2. Set key manually using KvStoreWrapper.
  // 3. Get key. Make sure it exists and value matches.

  thrift::KeyGetParams paramsBefore;
  paramsBefore.keys_ref()->emplace_back(key);
  auto pub = *kvStore_->getKvStore()
                  ->semifuture_getKvStoreKeyVals(kTestingAreaName, paramsBefore)
                  .get();
  auto it = pub.keyVals_ref()->find(key);
  EXPECT_EQ(it, pub.keyVals_ref()->end());

  // Set a key in KvStore.
  const thrift::Value thriftVal = createThriftValue(
      1 /* version */,
      nodeId /* originatorId */,
      value /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(
          1, nodeId, thrift::Value().value_ref() = std::string(value)));
  kvStore_->setKey(kTestingAreaName, key, thriftVal);

  // Check that value retrieved is same as value that was set.
  thrift::KeyGetParams paramsAfter;
  paramsAfter.keys_ref()->emplace_back(key);
  auto pubAfter =
      *kvStore_->getKvStore()
           ->semifuture_getKvStoreKeyVals(kTestingAreaName, paramsAfter)
           .get();
  auto itAfter = pubAfter.keyVals_ref()->find(key);
  EXPECT_NE(itAfter, pubAfter.keyVals_ref()->end());
  auto& valueFromStore = itAfter->second;
  EXPECT_EQ(valueFromStore.value_ref(), value);
  EXPECT_EQ(valueFromStore.get_version(), 1);
  EXPECT_EQ(valueFromStore.get_ttlVersion(), 0);

  kvStore_->stop();
}

/**
 * Validate retrieval of all key-values matching a given prefix.
 */
TEST_F(KvStoreTestFixture, DumpKeysWithPrefix) {
  // Create and start KvStore.
  const std::string nodeId = "node-for-dump";
  auto kvStore_ = createKvStore(nodeId);
  kvStore_->run();

  const std::string prefixRegex = "10\\.0\\.0\\.";
  const std::string prefix1 = "10.0.0.96";
  const std::string prefix2 = "10.0.0.128";
  const std::string prefix3 = "192.10.0.0";
  const std::string prefix4 = "192.168.0.0";

  // 1. Dump keys with no matches.
  // 2. Set keys manully. 2 include prefix, 2 do not.
  // 3. Dump keys. Verify 2 that include prefix are in dump, others are not.
  std::optional<std::unordered_map<std::string, thrift::Value>> maybeKeyMap;
  try {
    thrift::KeyDumpParams params;
    params.prefix_ref() = prefixRegex;
    params.keys_ref() = {prefixRegex};
    auto pub = *kvStore_->getKvStore()
                    ->semifuture_dumpKvStoreKeys(
                        std::move(params), {kTestingAreaName.t})
                    .get()
                    ->begin();
    maybeKeyMap = *pub.keyVals_ref();
  } catch (const std::exception& ex) {
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
      generateHash(
          1, nodeId, thrift::Value().value_ref() = std::string(genValue)));
  kvStore_->setKey(kTestingAreaName, prefix1, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix2, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix3, thriftVal);
  kvStore_->setKey(kTestingAreaName, prefix4, thriftVal);

  // Check that keys retrieved are those with prefix "10.0.0".
  std::optional<std::unordered_map<std::string, thrift::Value>>
      maybeKeysAfterInsert;
  try {
    thrift::KeyDumpParams params;
    params.prefix_ref() = prefixRegex;
    params.keys_ref() = {prefixRegex};
    auto pub = *kvStore_->getKvStore()
                    ->semifuture_dumpKvStoreKeys(
                        std::move(params), {kTestingAreaName.t})
                    .get()
                    ->begin();
    maybeKeysAfterInsert = *pub.keyVals_ref();
  } catch (const std::exception& ex) {
    maybeKeysAfterInsert = std::nullopt;
  }
  EXPECT_TRUE(maybeKeysAfterInsert.has_value());
  auto keysFromStore = maybeKeysAfterInsert.value();
  EXPECT_EQ(keysFromStore.size(), 2);
  EXPECT_EQ(keysFromStore.count(prefix1), 1);
  EXPECT_EQ(keysFromStore.count(prefix2), 1);
  EXPECT_EQ(keysFromStore.count(prefix3), 0);
  EXPECT_EQ(keysFromStore.count(prefix4), 0);

  // Cleanup.
  kvStore_->stop();
}

/**
 * Verify KvStore publishes kvStoreSynced signal even when receiving empty peers
 * in initialization process.
 */
TEST_F(KvStoreTestFixture, PublishKvStoreSyncedForEmptyPeerEvent) {
  messaging::ReplicateQueue<PeerEvent> myPeerUpdatesQueue;
  auto myStore = createKvStore(
      "node1", getTestKvConf(), {} /*areas*/, myPeerUpdatesQueue.getReader());
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
  thrift::AreaConfig area1Config, area2Config;
  area1Config.area_id_ref() = "area1";
  area2Config.area_id_ref() = "area2";
  AreaId area1Id{area1Config.get_area_id()};

  messaging::ReplicateQueue<PeerEvent> storeAPeerUpdatesQueue;
  messaging::ReplicateQueue<PeerEvent> storeBPeerUpdatesQueue;
  auto* storeA = createKvStore(
      "storeA",
      getTestKvConf(),
      {area1Config},
      storeAPeerUpdatesQueue.getReader());
  // storeB is configured with two areas.
  auto* storeB = createKvStore(
      "storeB",
      getTestKvConf(),
      {area1Config, area2Config},
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

/**
 * Start single testable store and set key-val. Verify content of KvStore by
 * querying it.
 */
TEST_F(KvStoreTestFixture, BasicSetKey) {
  // clean up counters before testing
  const std::string& key{"key1"};
  fb303::fbData->resetAllData();

  auto kvStore = createKvStore("node1");
  kvStore->run();

  // Set a key in KvStore
  const thrift::Value thriftVal = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      std::string("value1") /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(
          1, "node1", thrift::Value().value_ref() = std::string("value1")));
  kvStore->setKey(kTestingAreaName, key, thriftVal);

  // check stat was updated
  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters.at("kvstore.cmd_key_set.count"));

  // check key was added correctly
  auto recVal = kvStore->getKey(kTestingAreaName, key);
  ASSERT_TRUE(recVal.has_value());
  EXPECT_EQ(0, openr::compareValues(thriftVal, recVal.value()));

  // check only this key exists in kvstore
  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
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
          2, "node1", thrift::Value().value_ref() = std::string("value2")));
  kvStore->setKey(kTestingAreaName, key, thriftVal2);

  // check merge occurred correctly -- value overwritten
  auto recVal2 = kvStore->getKey(kTestingAreaName, key);
  ASSERT_TRUE(recVal2.has_value());
  EXPECT_EQ(0, openr::compareValues(thriftVal2, recVal2.value()));

  // check merge occurred correctly -- no duplicate key
  expectedKeyVals[key] = thriftVal2;
  allKeyVals = kvStore->dumpAll(kTestingAreaName);
  EXPECT_EQ(1, allKeyVals.size());
  EXPECT_EQ(expectedKeyVals, allKeyVals);

  // check stat was updated
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters.at("kvstore.cmd_key_set.count"));

  kvStore->stop();
}

//
// Test counter reporting
//
TEST_F(KvStoreTestFixture, CounterReport) {
  // clean up counters before testing
  const std::string& area = kTestingAreaName;
  fb303::fbData->resetAllData();

  auto kvStore = createKvStore("node1");
  kvStore->run();

  /** Verify redundant publications **/
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
  ASSERT_TRUE(counters.count("kvstore.flood_duration_ms.avg"));
  ASSERT_TRUE(counters.count("kvstore.full_sync_duration_ms.avg"));
  ASSERT_TRUE(counters.count("kvstore.peers.bytes_received.sum"));
  ASSERT_TRUE(counters.count("kvstore.peers.bytes_sent.sum"));
  ASSERT_TRUE(counters.count("kvstore.rate_limit_keys.avg"));
  ASSERT_TRUE(counters.count("kvstore.rate_limit_suppress.count"));
  ASSERT_TRUE(counters.count("kvstore.received_dual_messages.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_hash_dump.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_self_originated_key_dump.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_key_dump.count"));
  ASSERT_TRUE(counters.count("kvstore.cmd_key_get.count"));
  ASSERT_TRUE(counters.count("kvstore.sent_key_vals.sum"));
  ASSERT_TRUE(counters.count("kvstore.sent_publications.count"));
  ASSERT_TRUE(counters.count("kvstore.sent_key_vals." + area + ".sum"));
  ASSERT_TRUE(counters.count("kvstore.sent_publications." + area + ".count"));
  ASSERT_TRUE(counters.count("kvstore.updated_key_vals." + area + ".sum"));
  ASSERT_TRUE(counters.count("kvstore.received_key_vals." + area + ".sum"));
  ASSERT_TRUE(
      counters.count("kvstore.received_publications." + area + ".count"));

  // Verify the value of counter keys
  EXPECT_EQ(0, counters.at("kvstore.num_peers"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_peer_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_peer_add.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_per_del.count"));
  EXPECT_EQ(0, counters.at("kvstore.expired_key_vals.sum"));
  EXPECT_EQ(0, counters.at("kvstore.flood_duration_ms.avg"));
  EXPECT_EQ(0, counters.at("kvstore.full_sync_duration_ms.avg"));
  EXPECT_EQ(0, counters.at("kvstore.peers.bytes_received.sum"));
  EXPECT_EQ(0, counters.at("kvstore.peers.bytes_sent.sum"));
  EXPECT_EQ(0, counters.at("kvstore.rate_limit_keys.avg"));
  EXPECT_EQ(0, counters.at("kvstore.rate_limit_suppress.count"));
  EXPECT_EQ(0, counters.at("kvstore.received_dual_messages.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_hash_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_self_originated_key_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_key_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_key_get.count"));
  EXPECT_EQ(0, counters.at("kvstore.sent_key_vals.sum"));
  EXPECT_EQ(0, counters.at("kvstore.sent_publications.count"));
  EXPECT_EQ(0, counters.at("kvstore.sent_key_vals." + area + ".sum"));
  EXPECT_EQ(0, counters.at("kvstore.sent_publications." + area + ".count"));

  // Verify four keys were set
  ASSERT_EQ(1, counters.count("kvstore.cmd_key_set.count"));
  EXPECT_EQ(4, counters.at("kvstore.cmd_key_set.count"));
  ASSERT_EQ(1, counters.count("kvstore.received_key_vals.sum"));
  EXPECT_EQ(4, counters.at("kvstore.received_key_vals.sum"));
  ASSERT_EQ(1, counters.count("kvstore.received_key_vals." + area + ".sum"));
  EXPECT_EQ(4, counters.at("kvstore.received_key_vals." + area + ".sum"));

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
  EXPECT_EQ(4, counters.at("kvstore.received_publications.count"));
  ASSERT_EQ(
      1, counters.count("kvstore.received_publications." + area + ".count"));
  EXPECT_EQ(4, counters.at("kvstore.received_publications." + area + ".count"));

  // Verify redundant publication counter
  ASSERT_EQ(1, counters.count("kvstore.received_redundant_publications.count"));
  EXPECT_EQ(1, counters.at("kvstore.received_redundant_publications.count"));

  // Wait for counter update again
  std::this_thread::sleep_for(std::chrono::milliseconds(counterUpdateWaitTime));
  // Verify the num_keys counter is the same
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(expect_num_key, counters.at("kvstore.num_keys"));

  LOG(INFO) << "Counters received, yo";
  kvStore->stop();
  LOG(INFO) << "KvStore thread finished";
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

  auto kvStore = createKvStore("test");
  kvStore->run();

  //
  // 1. Advertise key-value with 1ms rtt
  // - This will get added to local KvStore but will never be published
  //   to other nodes or doesn't show up in GET request
  {
    auto thriftValue = value;
    thriftValue.ttl_ref() = 1;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));
    EXPECT_FALSE(kvStore->getKey(kTestingAreaName, key).has_value());
    EXPECT_EQ(0, kvStore->dumpAll(kTestingAreaName).size());
    EXPECT_EQ(0, kvStore->dumpAll(kTestingAreaName).size());

    // We will receive key-expiry publication but no key-advertisement
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(0, publication.keyVals_ref()->size());
    ASSERT_EQ(1, publication.expiredKeys_ref()->size());
    EXPECT_EQ(key, publication.expiredKeys_ref()->at(0));
  }

  //
  // 2. Advertise key with long enough ttl, so that it doesn't expire
  // - Ensure we receive publication over pub socket
  // - Ensure we receive key-value via GET request
  //
  {
    auto thriftValue = value;
    thriftValue.ttl_ref() = 50000;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl_ref(), *getRes->ttl_ref() + 1);
    getRes->ttl_ref() = *thriftValue.ttl_ref();
    getRes->hash_ref() = 0;
    EXPECT_EQ(thriftValue, getRes.value());

    // dump keys
    auto dumpRes = kvStore->dumpAll(kTestingAreaName);
    EXPECT_EQ(1, dumpRes.size());
    ASSERT_EQ(1, dumpRes.count(key));
    auto& dumpResValue = dumpRes.at(key);
    EXPECT_GE(*thriftValue.ttl_ref(), *dumpResValue.ttl_ref() + 1);
    dumpResValue.ttl_ref() = *thriftValue.ttl_ref();
    dumpResValue.hash_ref() = 0;
    EXPECT_EQ(thriftValue, dumpResValue);

    // dump hashes
    auto hashRes = kvStore->dumpHashes(kTestingAreaName);
    EXPECT_EQ(1, hashRes.size());
    ASSERT_EQ(1, hashRes.count(key));
    auto& hashResValue = hashRes.at(key);
    EXPECT_GE(*thriftValue.ttl_ref(), *hashResValue.ttl_ref() + 1);
    hashResValue.ttl_ref() = *thriftValue.ttl_ref();
    hashResValue.hash_ref() = 0;
    hashResValue.value_ref().copy_from(thriftValue.value_ref());
    EXPECT_EQ(thriftValue, hashResValue);

    // We will receive key-advertisement
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals_ref()->size());
    ASSERT_EQ(0, publication.expiredKeys_ref()->size());
    ASSERT_EQ(1, publication.keyVals_ref()->count(key));
    auto& pubValue = publication.keyVals_ref()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_GE(*thriftValue.ttl_ref(), *pubValue.ttl_ref() + 1);
    pubValue.ttl_ref() = *thriftValue.ttl_ref();
    pubValue.hash_ref() = 0;
    EXPECT_EQ(thriftValue, pubValue);
  }

  //
  // 3. Advertise ttl-update to set it to new value
  //
  {
    auto thriftValue = value;
    thriftValue.value_ref().reset();
    thriftValue.ttl_ref() = 30000;
    thriftValue.ttlVersion_ref() = *thriftValue.ttlVersion_ref() + 1;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl_ref(), *getRes->ttl_ref() + 1);
    EXPECT_EQ(*thriftValue.version_ref(), *getRes->version_ref());
    EXPECT_EQ(*thriftValue.originatorId_ref(), *getRes->originatorId_ref());
    EXPECT_EQ(*thriftValue.ttlVersion_ref(), *getRes->ttlVersion_ref());
    EXPECT_EQ(value.value_ref(), getRes->value_ref());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals_ref()->size());
    ASSERT_EQ(0, publication.expiredKeys_ref()->size());
    ASSERT_EQ(1, publication.keyVals_ref()->count(key));
    auto& pubValue = publication.keyVals_ref()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_FALSE(pubValue.value_ref().has_value());
    EXPECT_GE(*thriftValue.ttl_ref(), *pubValue.ttl_ref() + 1);
    EXPECT_EQ(*thriftValue.version_ref(), *pubValue.version_ref());
    EXPECT_EQ(*thriftValue.originatorId_ref(), *pubValue.originatorId_ref());
    EXPECT_EQ(*thriftValue.ttlVersion_ref(), *pubValue.ttlVersion_ref());
  }

  //
  // 4. Set ttl of key to INFINITE
  //
  {
    auto thriftValue = value;
    thriftValue.value_ref().reset();
    thriftValue.ttl_ref() = Constants::kTtlInfinity;
    thriftValue.ttlVersion_ref() = *thriftValue.ttlVersion_ref() + 2;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    // ttl should remain infinite
    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_EQ(Constants::kTtlInfinity, *getRes->ttl_ref());
    EXPECT_EQ(*thriftValue.version_ref(), *getRes->version_ref());
    EXPECT_EQ(*thriftValue.originatorId_ref(), *getRes->originatorId_ref());
    EXPECT_EQ(*thriftValue.ttlVersion_ref(), *getRes->ttlVersion_ref());
    EXPECT_EQ(value.value_ref(), getRes->value_ref());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals_ref()->size());
    ASSERT_EQ(0, publication.expiredKeys_ref()->size());
    ASSERT_EQ(1, publication.keyVals_ref()->count(key));
    auto& pubValue = publication.keyVals_ref()->at(key);
    // TTL should remain infinite
    EXPECT_FALSE(pubValue.value_ref().has_value());
    EXPECT_EQ(Constants::kTtlInfinity, *pubValue.ttl_ref());
    EXPECT_EQ(*thriftValue.version_ref(), *pubValue.version_ref());
    EXPECT_EQ(*thriftValue.originatorId_ref(), *pubValue.originatorId_ref());
    EXPECT_EQ(*thriftValue.ttlVersion_ref(), *pubValue.ttlVersion_ref());
  }

  //
  // 5. Set ttl of key back to a fixed value
  //
  {
    auto thriftValue = value;
    thriftValue.value_ref().reset();
    thriftValue.ttl_ref() = 20000;
    thriftValue.ttlVersion_ref() = *thriftValue.ttlVersion_ref() + 3;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(*thriftValue.ttl_ref(), *getRes->ttl_ref() + 1);
    EXPECT_EQ(*thriftValue.version_ref(), *getRes->version_ref());
    EXPECT_EQ(*thriftValue.originatorId_ref(), *getRes->originatorId_ref());
    EXPECT_EQ(*thriftValue.ttlVersion_ref(), *getRes->ttlVersion_ref());
    EXPECT_EQ(value.value_ref(), getRes->value_ref());

    // We will receive update over PUB socket
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(1, publication.keyVals_ref()->size());
    ASSERT_EQ(0, publication.expiredKeys_ref()->size());
    ASSERT_EQ(1, publication.keyVals_ref()->count(key));
    auto& pubValue = publication.keyVals_ref()->at(key);
    // TTL decremented by 1 before it gets forwarded out
    EXPECT_FALSE(pubValue.value_ref().has_value());
    EXPECT_GE(*thriftValue.ttl_ref(), *pubValue.ttl_ref() + 1);
    EXPECT_EQ(*thriftValue.version_ref(), *pubValue.version_ref());
    EXPECT_EQ(*thriftValue.originatorId_ref(), *pubValue.originatorId_ref());
    EXPECT_EQ(*thriftValue.ttlVersion_ref(), *pubValue.ttlVersion_ref());
  }

  //
  // 6. Apply old ttl update and see no effect
  //
  {
    auto thriftValue = value;
    thriftValue.value_ref().reset();
    thriftValue.ttl_ref() = 10000;
    EXPECT_TRUE(kvStore->setKey(kTestingAreaName, key, thriftValue));

    auto getRes = kvStore->getKey(kTestingAreaName, key);
    ASSERT_TRUE(getRes.has_value());
    EXPECT_GE(20000, *getRes->ttl_ref()); // Previous ttl was set to 20s
    EXPECT_LE(10000, *getRes->ttl_ref());
    EXPECT_EQ(*value.version_ref(), *getRes->version_ref());
    EXPECT_EQ(*value.originatorId_ref(), *getRes->originatorId_ref());
    EXPECT_EQ(*value.ttlVersion_ref() + 3, *getRes->ttlVersion_ref());
    EXPECT_EQ(value.value_ref(), getRes->value_ref());
  }

  kvStore->stop();
}

TEST_F(KvStoreTestFixture, LeafNode) {
  auto store0Conf = getTestKvConf();
  store0Conf.set_leaf_node_ref() = true;
  store0Conf.key_prefix_filters_ref() = {"e2e"};
  store0Conf.key_originator_id_filters_ref() = {"store0"};

  auto store0 = createKvStore("store0", store0Conf);
  auto store1 = createKvStore("store1");
  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
  std::unordered_map<std::string, thrift::Value> expectedOrignatorVals;

  store0->run();
  store1->run();

  // Set key into store0 with origninator ID as "store0".
  // getKey should pass for this key.
  LOG(INFO) << "Setting value in store0 with originator id as store0...";
  auto thriftVal = createThriftValue(
      1 /* version */,
      "store0" /* originatorId */,
      "value1" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);

  thriftVal.hash_ref() = generateHash(
      *thriftVal.version_ref(),
      *thriftVal.originatorId_ref(),
      thriftVal.value_ref());
  EXPECT_TRUE(store0->setKey(kTestingAreaName, "test1", thriftVal));
  // Adding key in store1 too
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "test1", thriftVal));
  expectedKeyVals["test1"] = thriftVal;
  expectedOrignatorVals["test1"] = thriftVal;

  auto maybeThriftVal = store0->getKey(kTestingAreaName, "test1");
  ASSERT_TRUE(maybeThriftVal.has_value());

  // Set key with a different originator ID
  // This shouldn't be added to Kvstore
  LOG(INFO) << "Setting value in store0 with originator id as store1...";
  auto thriftVal2 = createThriftValue(
      1 /* version */,
      "store1" /* originatorId */,
      "value1" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  thriftVal2.hash_ref() = generateHash(
      *thriftVal2.version_ref(),
      *thriftVal2.originatorId_ref(),
      thriftVal2.value_ref());
  EXPECT_TRUE(store0->setKey(kTestingAreaName, "test2", thriftVal2));
  // Adding key in store1 too
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "test2", thriftVal2));
  expectedKeyVals["test2"] = thriftVal2;

  maybeThriftVal = store0->getKey(kTestingAreaName, "test2");
  ASSERT_FALSE(maybeThriftVal.has_value());

  std::unordered_map<std::string, thrift::Value> expectedKeyPrefixVals;
  // Set key with a different originator ID, but matching prefix
  // This should be added to Kvstore.
  LOG(INFO) << "Setting key value with a matching key prefix...";
  auto thriftVal3 = createThriftValue(
      1 /* version */,
      "store1" /* originatorId */,
      "value1" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  thriftVal3.hash_ref() = generateHash(
      *thriftVal3.version_ref(),
      *thriftVal3.originatorId_ref(),
      thriftVal3.value_ref());
  EXPECT_TRUE(store0->setKey(kTestingAreaName, "e2exyz", thriftVal3));
  // Adding key in store1 too
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "e2exyz", thriftVal3));
  expectedKeyVals["e2exyz"] = thriftVal3;
  expectedKeyPrefixVals["e2exyz"] = thriftVal3;

  maybeThriftVal = store0->getKey(kTestingAreaName, "e2exyz");
  ASSERT_TRUE(maybeThriftVal.has_value());

  // Add another matching key prefix, different originator ID
  // This should be added to Kvstore.
  LOG(INFO) << "Setting key value with a matching key prefix...";
  auto thriftVal4 = createThriftValue(
      1 /* version */,
      "store1" /* originatorId */,
      "value1" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  thriftVal4.hash_ref() = generateHash(
      *thriftVal4.version_ref(),
      *thriftVal4.originatorId_ref(),
      thriftVal4.value_ref());
  EXPECT_TRUE(store0->setKey(kTestingAreaName, "e2e", thriftVal4));
  // Adding key in store1 too
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "e2e", thriftVal4));
  expectedKeyVals["e2e"] = thriftVal4;
  expectedKeyPrefixVals["e2e"] = thriftVal4;

  maybeThriftVal = store0->getKey(kTestingAreaName, "e2e");
  ASSERT_TRUE(maybeThriftVal.has_value());

  // Add non-matching key prefix, different originator ID..
  // This shouldn't be added to Kvstore.
  LOG(INFO) << "Setting key value with a non-matching key prefix...";
  auto thriftVal5 = createThriftValue(
      1 /* version */,
      "storex" /* originatorId */,
      "value1" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  thriftVal5.hash_ref() = generateHash(
      *thriftVal5.version_ref(),
      *thriftVal5.originatorId_ref(),
      thriftVal5.value_ref());
  EXPECT_TRUE(store0->setKey(kTestingAreaName, "e2", thriftVal5));
  // Adding key in store1 too
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "e2", thriftVal5));
  expectedKeyVals["e2"] = thriftVal5;

  maybeThriftVal = store0->getKey(kTestingAreaName, "e2");
  ASSERT_FALSE(maybeThriftVal.has_value());

  // Add key in store1 with originator ID of store0
  LOG(INFO) << "Setting key with origninator ID of leaf into a non-leaf node";
  auto thriftVal6 = createThriftValue(
      1 /* version */,
      "store0" /* originatorId */,
      "value1" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  thriftVal6.hash_ref() = generateHash(
      *thriftVal6.version_ref(),
      *thriftVal6.originatorId_ref(),
      thriftVal6.value_ref());
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "test3", thriftVal6));
  expectedKeyVals["test3"] = thriftVal6;
  expectedOrignatorVals["test3"] = thriftVal6;

  // Store1 should have 6 keys
  EXPECT_EQ(expectedKeyVals, store1->dumpAll(kTestingAreaName));

  // Request dumpAll from store1 with a key prefix provided,
  // must return 2 keys
  std::optional<KvStoreFilters> kvFilters1{KvStoreFilters({"e2e"}, {})};
  EXPECT_EQ(
      expectedKeyPrefixVals,
      store1->dumpAll(kTestingAreaName, std::move(kvFilters1)));

  // Request dumpAll from store1 with a originator prefix provided,
  // must return 2 keys
  std::optional<KvStoreFilters> kvFilters2{KvStoreFilters({""}, {"store0"})};
  EXPECT_EQ(
      expectedOrignatorVals,
      store1->dumpAll(kTestingAreaName, std::move(kvFilters2)));

  expectedOrignatorVals["e2exyz"] = thriftVal2;
  expectedOrignatorVals["e2e"] = thriftVal4;

  // Request dumpAll from store1 with a key prefix and
  // originator prefix provided, must return 4 keys
  std::optional<KvStoreFilters> kvFilters3{KvStoreFilters({"e2e"}, {"store0"})};
  EXPECT_EQ(
      expectedOrignatorVals,
      store1->dumpAll(kTestingAreaName, std::move(kvFilters3)));

  // try dumpAll with multiple key and originator prefix
  expectedOrignatorVals["test3"] = thriftVal6;
  expectedOrignatorVals["e2"] = thriftVal5;
  std::optional<KvStoreFilters> kvFilters4{
      KvStoreFilters({"e2e", "test3"}, {"store0", "storex"})};
  EXPECT_EQ(
      expectedOrignatorVals,
      store1->dumpAll(kTestingAreaName, std::move(kvFilters4)));
}

/**
 * Test to verify that during peer sync TTLs are sent with remaining
 * time to expire, and new keys are added with that TTL while TTL for
 * existing keys is not updated.
 * 1. Start store0,
 * 2. Add two keys to store0
 * 3. Sleep for 200msec
 * 4. Start store1 and add one of the keys
 * 5  Sync with keys from store0
 * 6. Check store1 adds a new key with TTL equal to [default value - 200msec]
 * 7. Check TTL for existing key in store1 does not get updated
 */
TEST_F(KvStoreTestFixture, PeerSyncTtlExpiry) {
  auto store0 = createKvStore("store0");
  auto store1 = createKvStore("store1");
  store0->run();
  store1->run();

  auto thriftVal1 = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      "value1" /* value */,
      kTtlMs /* ttl */,
      0 /* ttl version */,
      0 /* hash */);

  thriftVal1.hash_ref() = generateHash(
      *thriftVal1.version_ref(),
      *thriftVal1.originatorId_ref(),
      thriftVal1.value_ref());

  auto thriftVal2 = createThriftValue(
      1 /* version */,
      "node1" /* originatorId */,
      "value2" /* value */,
      kTtlMs /* ttl */,
      0 /* ttl version */,
      0 /* hash */);

  thriftVal2.hash_ref() = generateHash(
      *thriftVal2.version_ref(),
      *thriftVal2.originatorId_ref(),
      thriftVal2.value_ref());

  EXPECT_TRUE(store0->setKey(kTestingAreaName, "test1", thriftVal1));
  auto maybeThriftVal = store0->getKey(kTestingAreaName, "test1");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs, *maybeThriftVal->ttl_ref());
  maybeThriftVal->ttl_ref() = kTtlMs;
  EXPECT_EQ(thriftVal1, *maybeThriftVal);

  EXPECT_TRUE(store0->setKey(kTestingAreaName, "test2", thriftVal2));
  maybeThriftVal = store0->getKey(kTestingAreaName, "test2");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs, *maybeThriftVal->ttl_ref());
  maybeThriftVal->ttl_ref() = kTtlMs;
  EXPECT_EQ(thriftVal2, *maybeThriftVal);

  EXPECT_TRUE(store1->setKey(kTestingAreaName, "test2", thriftVal2));
  maybeThriftVal = store1->getKey(kTestingAreaName, "test2");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs, *maybeThriftVal->ttl_ref());
  maybeThriftVal->ttl_ref() = kTtlMs;
  EXPECT_EQ(thriftVal2, *maybeThriftVal);
  // sleep override
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(store1->addPeer(
      kTestingAreaName, store0->getNodeId(), store0->getPeerSpec()));

  OpenrEventBase evb;
  folly::Baton waitBaton;
  int scheduleAt{0};
  // wait to sync kvstore. Set as 100ms sine p99 of KvStore convergece completed
  // within 99ms as of today.
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 100), [&]() noexcept {
        // std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // key 'test1' should be added with remaining TTL
        maybeThriftVal = store1->getKey(kTestingAreaName, "test1");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_GE(kTtlMs - 200, *maybeThriftVal.value().ttl_ref());

        // key 'test2' should not be updated, it should have kTtlMs
        maybeThriftVal = store1->getKey(kTestingAreaName, "test2");
        ASSERT_TRUE(maybeThriftVal.has_value());
        EXPECT_GE(kTtlMs, *maybeThriftVal.value().ttl_ref());
        evb.stop();
      });

  // Start the event loop and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
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
  auto store0 = createKvStore("store0");
  auto store1 = createKvStore("store1");
  auto store2 = createKvStore("store2");
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
  std::unordered_map<std::string, thrift::PeerSpec> expectedPeers = {
      {store0->getNodeId(),
       store0->getPeerSpec(thrift::KvStorePeerState::INITIALIZED)},
  };
  EXPECT_EQ(expectedPeers, store1->getPeers(kTestingAreaName));
  EXPECT_EQ(expectedPeers, store2->getPeers(kTestingAreaName));

  //
  // Step 1) and 2): advertise key from store1/store2 and verify
  //
  {
    auto thriftVal = createThriftValue(
        1 /* version */, "1.2.3.4" /* originatorId */, "value1" /* value */
    );
    EXPECT_TRUE(store1->setKey(kTestingAreaName, key, thriftVal));
    // Update hash
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());

    // Receive publication from store0 for new key-update
    auto pub = store0->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());
    EXPECT_EQ(thriftVal, pub.keyVals_ref()[key]);
  }

  // Now play the same trick with the other store
  {
    auto thriftVal = createThriftValue(
        2 /* version */, "1.2.3.4" /* originatorId */, "value2" /* value */
    );
    EXPECT_TRUE(store2->setKey(kTestingAreaName, key, thriftVal));
    // Update hash
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());

    // Receive publication from store0 for new key-update
    auto pub = store0->recvPublication();
    EXPECT_EQ(1, pub.keyVals_ref()->size());
    EXPECT_EQ(thriftVal, pub.keyVals_ref()[key]);
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());

    // store1/store2 should NOT have the key since no peer to flood
    auto maybeVal1 = store1->getKey(kTestingAreaName, key);
    CHECK(maybeVal1.has_value());
    auto maybeVal2 = store2->getKey(kTestingAreaName, key);
    CHECK(maybeVal2.has_value());
    EXPECT_NE(3, *maybeVal1.value().version_ref());
    EXPECT_NE(3, *maybeVal2.value().version_ref());
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
    EXPECT_EQ(3, *maybeVal1.value().version_ref());

    // store2 still NOT updated since there is no full-sync
    auto maybeVal = store2->getKey(kTestingAreaName, key);
    CHECK(maybeVal.has_value());
    EXPECT_NE(3, *maybeVal.value().version_ref());
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
 * (DUAL disabled)   (root)
 *    fsw001         fsw002
 *      |      \ /
 *      |      / \
 *    rsw001         rsw002
 *  (non-root)     (non-root)
 *
 *  This test will make sure flooding topology is NOT jeopardized
 *  even when some nodes have DUAL disabled, whereas other has DUAL
 *  enabled. DUAL-enabled node will still flood to DUAL-disabled node
 *  for KvStore's global consistency.
 */
TEST_F(KvStoreTestFixture, FloodOptimizationWithBackwardCompatibility) {
  // Disable DUAL for fsw001
  auto fsw001Conf = getTestKvConf();
  fsw001Conf.enable_flood_optimization_ref() = false;

  // Enable DUAL and set flood root for fsw002
  auto fsw002Conf = getTestKvConf();
  fsw002Conf.enable_flood_optimization_ref() = true;
  fsw002Conf.is_flood_root_ref() = true;

  // Enable DUAL and set non-root for rsw*
  auto rswConf = getTestKvConf();
  rswConf.enable_flood_optimization_ref() = true;
  rswConf.is_flood_root_ref() = false;

  auto fsw001 = createKvStore("fsw001", fsw001Conf);
  auto fsw002 = createKvStore("fsw002", fsw002Conf);
  auto rsw001 = createKvStore("rsw001", rswConf);
  auto rsw002 = createKvStore("rsw002", rswConf);

  // Start stores in their respective threads.
  fsw001->run();
  fsw002->run();
  rsw001->run();
  rsw002->run();

  // Add peers to all stores
  thrift::PeersMap rswPeers = {
      {rsw001->getNodeId(), rsw001->getPeerSpec()},
      {rsw002->getNodeId(), rsw002->getPeerSpec()}};
  EXPECT_TRUE(fsw001->addPeers(kTestingAreaName, rswPeers));
  EXPECT_TRUE(fsw002->addPeers(kTestingAreaName, rswPeers));

  thrift::PeersMap fswPeers = {
      {fsw001->getNodeId(), fsw001->getPeerSpec()},
      {fsw002->getNodeId(), fsw002->getPeerSpec()}};
  EXPECT_TRUE(rsw001->addPeers(kTestingAreaName, fswPeers));
  EXPECT_TRUE(rsw002->addPeers(kTestingAreaName, fswPeers));
  fsw001->recvKvStoreSyncedSignal();
  fsw002->recvKvStoreSyncedSignal();
  rsw001->recvKvStoreSyncedSignal();
  rsw002->recvKvStoreSyncedSignal();

  // verify flooding peers to make sure flooding topology is good
  const auto& fsw001SptInfos = fsw001->getFloodTopo(kTestingAreaName);
  const auto& fsw002SptInfos = fsw002->getFloodTopo(kTestingAreaName);
  const auto& rsw001SptInfos = rsw001->getFloodTopo(kTestingAreaName);
  const auto& rsw002SptInfos = rsw002->getFloodTopo(kTestingAreaName);

  // Verify fsw001 -> [rsw001, rsw002]
  auto& fsw001Peers = fsw001SptInfos.get_floodPeers();
  EXPECT_TRUE(fsw001Peers.count("rsw001"));
  EXPECT_TRUE(fsw001Peers.count("rsw002"));

  // Verify fsw002 -> [rsw001, rsw002]
  auto& fsw002Peers = fsw002SptInfos.get_floodPeers();
  EXPECT_TRUE(fsw002Peers.count("rsw001"));
  EXPECT_TRUE(fsw002Peers.count("rsw002"));

  // Verify rsw001 -> [fsw001, fsw002]
  auto& rsw001Peers = rsw001SptInfos.get_floodPeers();
  EXPECT_TRUE(rsw001Peers.count("fsw001"));
  EXPECT_TRUE(rsw001Peers.count("fsw002"));

  // Verify rsw002 -> [fsw001, fsw002]
  auto& rsw002Peers = rsw002SptInfos.get_floodPeers();
  EXPECT_TRUE(rsw002Peers.count("fsw001"));
  EXPECT_TRUE(rsw002Peers.count("fsw002"));
}

/**
 * 2 x 2 Fabric topology
 * r0, r1 are root nodes
 * n0, n1 are non-root nodes
 * r0    r1
 * | \   /|
 * |  \_/ |
 * | /  \ |
 * n0    n1
 * verify flooding-topology information on each node (parent, children, cost)
 */
TEST_F(KvStoreTestFixture, DualTest) {
  auto floodRootConf = getTestKvConf();
  floodRootConf.enable_flood_optimization_ref() = true;
  floodRootConf.is_flood_root_ref() = true;

  auto nonFloodRootConf = getTestKvConf();
  nonFloodRootConf.enable_flood_optimization_ref() = true;
  nonFloodRootConf.is_flood_root_ref() = false;

  auto r0 = createKvStore("r0", floodRootConf);
  auto r1 = createKvStore("r1", floodRootConf);
  auto n0 = createKvStore("n0", nonFloodRootConf);
  auto n1 = createKvStore("n1", nonFloodRootConf);

  // Start stores in their respective threads.
  r0->run();
  r1->run();
  n0->run();
  n1->run();

  // Add peers to all stores
  thrift::PeersMap nPeers = {
      {n0->getNodeId(), n0->getPeerSpec()},
      {n1->getNodeId(), n1->getPeerSpec()}};
  EXPECT_TRUE(r0->addPeers(kTestingAreaName, nPeers));
  EXPECT_TRUE(r1->addPeers(kTestingAreaName, nPeers));
  thrift::PeersMap rPeers = {
      {r0->getNodeId(), r0->getPeerSpec()},
      {r1->getNodeId(), r1->getPeerSpec()}};
  EXPECT_TRUE(n0->addPeers(kTestingAreaName, rPeers));
  EXPECT_TRUE(n1->addPeers(kTestingAreaName, rPeers));
  r0->recvKvStoreSyncedSignal();
  r1->recvKvStoreSyncedSignal();
  n0->recvKvStoreSyncedSignal();
  n1->recvKvStoreSyncedSignal();

  // let kvstore dual sync
  /* sleep override */
  // TODO: Make this test more reliable instead of hardcode waiting time
  std::this_thread::sleep_for(std::chrono::seconds(1));

  // helper function to validate all roots up case
  // everybody should pick r0 as spt root
  auto validateAllRootsUpCase = [&]() {
    std::string r0Parent;
    std::string r1Parent;

    // validate r0
    {
      const auto& sptInfos = r0->getFloodTopo(kTestingAreaName);
      EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
      EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
      EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

      // validate root r0
      const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
      EXPECT_TRUE(*sptInfo0.passive_ref());
      EXPECT_EQ(*sptInfo0.cost_ref(), 0);
      EXPECT_EQ(sptInfo0.parent_ref(), "r0");
      EXPECT_EQ(sptInfo0.children_ref()->size(), 2);
      EXPECT_EQ(sptInfo0.children_ref()->count("n0"), 1);
      EXPECT_EQ(sptInfo0.children_ref()->count("n1"), 1);

      // validate root r1
      const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
      EXPECT_TRUE(*sptInfo1.passive_ref());
      EXPECT_EQ(*sptInfo1.cost_ref(), 2);
      EXPECT_TRUE(sptInfo1.parent_ref().has_value());
      r0Parent = *sptInfo1.parent_ref();
      EXPECT_TRUE(r0Parent == "n0" or r0Parent == "n1");
      EXPECT_EQ(sptInfo1.children_ref()->size(), 0);

      // validate flooding-peer-info
      EXPECT_TRUE(sptInfos.floodRootId_ref().has_value());
      EXPECT_EQ(*sptInfos.floodRootId_ref(), "r0");
      EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 2);
      EXPECT_EQ(sptInfos.floodPeers_ref()->count("n0"), 1);
      EXPECT_EQ(sptInfos.floodPeers_ref()->count("n1"), 1);
    }

    // validate r1
    {
      const auto& sptInfos = r1->getFloodTopo(kTestingAreaName);
      EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
      EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
      EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

      // validate root r1
      const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
      EXPECT_TRUE(*sptInfo1.passive_ref());
      EXPECT_EQ(*sptInfo1.cost_ref(), 0);
      EXPECT_EQ(sptInfo1.parent_ref(), "r1");
      EXPECT_EQ(sptInfo1.children_ref()->size(), 2);
      EXPECT_EQ(sptInfo1.children_ref()->count("n0"), 1);
      EXPECT_EQ(sptInfo1.children_ref()->count("n1"), 1);

      // validate root r0
      const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
      EXPECT_TRUE(*sptInfo0.passive_ref());
      EXPECT_EQ(*sptInfo0.cost_ref(), 2);
      EXPECT_TRUE(sptInfo0.parent_ref().has_value());
      r1Parent = *sptInfo0.parent_ref();
      EXPECT_TRUE(r1Parent == "n0" or r1Parent == "n1");
      EXPECT_EQ(sptInfo0.children_ref()->size(), 0);

      // validate flooding-peer-info
      EXPECT_TRUE(sptInfos.floodRootId_ref().has_value());
      EXPECT_EQ(*sptInfos.floodRootId_ref(), "r0");
      EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 1);
      EXPECT_EQ(sptInfos.floodPeers_ref()->count(r1Parent), 1);
    }

    // validate n0
    {
      const auto& sptInfos = n0->getFloodTopo(kTestingAreaName);
      EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
      EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
      EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

      // validate root r0
      const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
      EXPECT_TRUE(*sptInfo0.passive_ref());
      EXPECT_EQ(*sptInfo0.cost_ref(), 1);
      EXPECT_EQ(sptInfo0.parent_ref(), "r0");
      if (r1Parent == "n0") {
        EXPECT_EQ(sptInfo0.children_ref()->size(), 1);
        EXPECT_EQ(sptInfo0.children_ref()->count("r1"), 1);
      } else {
        EXPECT_EQ(sptInfo0.children_ref()->size(), 0);
      }

      // validate root r1
      const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
      EXPECT_TRUE(*sptInfo1.passive_ref());
      EXPECT_EQ(*sptInfo1.cost_ref(), 1);
      EXPECT_EQ(sptInfo1.parent_ref(), "r1");
      if (r0Parent == "n0") {
        EXPECT_EQ(sptInfo1.children_ref()->size(), 1);
        EXPECT_EQ(sptInfo1.children_ref()->count("r0"), 1);
      } else {
        EXPECT_EQ(sptInfo1.children_ref()->size(), 0);
      }

      // validate flooding-peer-info
      EXPECT_EQ(*sptInfos.floodRootId_ref(), "r0");
      if (r1Parent == "n0") {
        EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 2);
        EXPECT_EQ(sptInfos.floodPeers_ref()->count("r0"), 1);
        EXPECT_EQ(sptInfos.floodPeers_ref()->count("r1"), 1);
      } else {
        EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 1);
        EXPECT_EQ(sptInfos.floodPeers_ref()->count("r0"), 1);
      }
    }

    // validate n1
    {
      const auto& sptInfos = n1->getFloodTopo(kTestingAreaName);
      EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
      EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
      EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

      // validate root r0
      const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
      EXPECT_TRUE(*sptInfo0.passive_ref());
      EXPECT_EQ(*sptInfo0.cost_ref(), 1);
      EXPECT_EQ(sptInfo0.parent_ref(), "r0");
      if (r1Parent == "n1") {
        EXPECT_EQ(sptInfo0.children_ref()->size(), 1);
        EXPECT_EQ(sptInfo0.children_ref()->count("r1"), 1);
      } else {
        EXPECT_EQ(sptInfo0.children_ref()->size(), 0);
      }

      // validate root r1
      const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
      EXPECT_TRUE(*sptInfo1.passive_ref());
      EXPECT_EQ(*sptInfo1.cost_ref(), 1);
      EXPECT_EQ(sptInfo1.parent_ref(), "r1");
      if (r0Parent == "n1") {
        EXPECT_EQ(sptInfo1.children_ref()->size(), 1);
        EXPECT_EQ(sptInfo1.children_ref()->count("r0"), 1);
      } else {
        EXPECT_EQ(sptInfo1.children_ref()->size(), 0);
      }

      // validate flooding-peer-info
      EXPECT_EQ(*sptInfos.floodRootId_ref(), "r0");
      if (r1Parent == "n1") {
        EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 2);
        EXPECT_EQ(sptInfos.floodPeers_ref()->count("r0"), 1);
        EXPECT_EQ(sptInfos.floodPeers_ref()->count("r1"), 1);
      } else {
        EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 1);
        EXPECT_EQ(sptInfos.floodPeers_ref()->count("r0"), 1);
      }
    }
  };

  LOG(INFO) << "Test 1: validate all roots are up...";
  validateAllRootsUpCase();

  // bring r0 down and everyone should choose r1 as new root
  r0->delPeer(kTestingAreaName, "n0");
  r0->delPeer(kTestingAreaName, "n1");
  n0->delPeer(kTestingAreaName, "r0");
  n1->delPeer(kTestingAreaName, "r0");

  // let kvstore dual sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

  LOG(INFO) << "Test 2: bring r0 down...";

  // validate r1
  {
    const auto& sptInfos = r1->getFloodTopo(kTestingAreaName);
    EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
    EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
    EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

    // validate root r1
    const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
    EXPECT_TRUE(*sptInfo1.passive_ref());
    EXPECT_EQ(*sptInfo1.cost_ref(), 0);
    EXPECT_EQ(sptInfo1.parent_ref(), "r1");
    EXPECT_EQ(sptInfo1.children_ref()->size(), 2);
    EXPECT_EQ(sptInfo1.children_ref()->count("n0"), 1);
    EXPECT_EQ(sptInfo1.children_ref()->count("n1"), 1);

    // validate root r0
    const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
    EXPECT_TRUE(*sptInfo0.passive_ref());
    EXPECT_EQ(*sptInfo0.cost_ref(), std::numeric_limits<int64_t>::max());
    EXPECT_EQ(sptInfo0.children_ref()->size(), 0);

    // validate flooding-peer-info
    EXPECT_EQ(*sptInfos.floodRootId_ref(), "r1");
    EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 2);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("n0"), 1);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("n1"), 1);
  }

  // validate n0
  {
    const auto& sptInfos = n0->getFloodTopo(kTestingAreaName);
    EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
    EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
    EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

    // validate root r0
    const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
    EXPECT_TRUE(*sptInfo0.passive_ref());
    EXPECT_EQ(*sptInfo0.cost_ref(), std::numeric_limits<int64_t>::max());

    // validate root r1
    const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
    EXPECT_TRUE(*sptInfo1.passive_ref());
    EXPECT_EQ(*sptInfo1.cost_ref(), 1);
    EXPECT_EQ(sptInfo1.parent_ref(), "r1");
    EXPECT_EQ(sptInfo1.children_ref()->size(), 0);

    // validate flooding-peer-info
    EXPECT_EQ(*sptInfos.floodRootId_ref(), "r1");
    EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 1);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("r1"), 1);
  }

  // validate n1
  {
    const auto& sptInfos = n1->getFloodTopo(kTestingAreaName);
    EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
    EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
    EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

    // validate root r0
    const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
    EXPECT_TRUE(*sptInfo0.passive_ref());
    EXPECT_EQ(*sptInfo0.cost_ref(), std::numeric_limits<int64_t>::max());

    // validate root r1
    const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
    EXPECT_TRUE(*sptInfo1.passive_ref());
    EXPECT_EQ(*sptInfo1.cost_ref(), 1);
    EXPECT_EQ(sptInfo1.parent_ref(), "r1");
    EXPECT_EQ(sptInfo1.children_ref()->size(), 0);

    // validate flooding-peer-info
    EXPECT_EQ(*sptInfos.floodRootId_ref(), "r1");
    EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 1);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("r1"), 1);
  }

  // bring r0 up and everyone should choose r0 as new root
  EXPECT_TRUE(
      r0->addPeer(kTestingAreaName, n0->getNodeId(), n0->getPeerSpec()));
  EXPECT_TRUE(
      r0->addPeer(kTestingAreaName, n1->getNodeId(), n1->getPeerSpec()));
  EXPECT_TRUE(
      n0->addPeer(kTestingAreaName, r0->getNodeId(), r0->getPeerSpec()));
  EXPECT_TRUE(
      n1->addPeer(kTestingAreaName, r0->getNodeId(), r0->getPeerSpec()));

  // let kvstore dual sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

  LOG(INFO) << "Test 3: bring r0 back up...";

  validateAllRootsUpCase();

  // bring link r0-n0 down and verify topology below
  // r0    r1
  //   \   /|
  //    \_/ |
  //   /  \ |
  // n0    n1
  r0->delPeer(kTestingAreaName, "n0");
  n0->delPeer(kTestingAreaName, "r0");

  // let kvstore dual sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

  LOG(INFO) << "Test 4: bring link r0-n0 down...";

  // validate r0
  {
    const auto& sptInfos = r0->getFloodTopo(kTestingAreaName);
    EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
    EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
    EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

    // validate root r0
    const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
    EXPECT_TRUE(*sptInfo0.passive_ref());
    EXPECT_EQ(*sptInfo0.cost_ref(), 0);
    EXPECT_EQ(sptInfo0.parent_ref(), "r0");
    EXPECT_EQ(sptInfo0.children_ref()->size(), 1);
    EXPECT_EQ(sptInfo0.children_ref()->count("n1"), 1);

    // validate root r1
    const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
    EXPECT_TRUE(*sptInfo1.passive_ref());
    EXPECT_EQ(*sptInfo1.cost_ref(), 2);
    EXPECT_EQ(sptInfo1.parent_ref(), "n1");
    EXPECT_EQ(sptInfo1.children_ref()->size(), 0);

    // validate flooding-peer-info
    EXPECT_TRUE(sptInfos.floodRootId_ref().has_value());
    EXPECT_EQ(*sptInfos.floodRootId_ref(), "r0");
    EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 1);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("n1"), 1);
  }

  // validate r1
  {
    const auto& sptInfos = r1->getFloodTopo(kTestingAreaName);
    EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
    EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
    EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

    // validate root r0
    const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
    EXPECT_TRUE(*sptInfo0.passive_ref());
    EXPECT_EQ(*sptInfo0.cost_ref(), 2);
    EXPECT_EQ(sptInfo0.parent_ref(), "n1");
    EXPECT_EQ(sptInfo0.children_ref()->size(), 1);
    EXPECT_EQ(sptInfo0.children_ref()->count("n0"), 1);

    // validate root r1
    const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
    EXPECT_TRUE(*sptInfo1.passive_ref());
    EXPECT_EQ(*sptInfo1.cost_ref(), 0);
    EXPECT_EQ(sptInfo1.parent_ref(), "r1");
    EXPECT_EQ(sptInfo1.children_ref()->size(), 2);
    EXPECT_EQ(sptInfo1.children_ref()->count("n0"), 1);
    EXPECT_EQ(sptInfo1.children_ref()->count("n1"), 1);

    // validate flooding-peer-info
    EXPECT_TRUE(sptInfos.floodRootId_ref().has_value());
    EXPECT_EQ(*sptInfos.floodRootId_ref(), "r0");
    EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 2);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("n0"), 1);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("n1"), 1);
  }

  // validate n0
  {
    const auto& sptInfos = n0->getFloodTopo(kTestingAreaName);
    EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
    EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
    EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

    // validate root r0
    const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
    EXPECT_TRUE(*sptInfo0.passive_ref());
    EXPECT_EQ(*sptInfo0.cost_ref(), 3);
    EXPECT_EQ(sptInfo0.parent_ref(), "r1");
    EXPECT_EQ(sptInfo0.children_ref()->size(), 0);

    // validate root r1
    const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
    EXPECT_TRUE(*sptInfo1.passive_ref());
    EXPECT_EQ(*sptInfo1.cost_ref(), 1);
    EXPECT_EQ(sptInfo1.parent_ref(), "r1");
    EXPECT_EQ(sptInfo1.children_ref()->size(), 0);

    // validate flooding-peer-info
    EXPECT_EQ(*sptInfos.floodRootId_ref(), "r0");
    EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 1);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("r1"), 1);
  }

  // validate n1
  {
    const auto& sptInfos = n1->getFloodTopo(kTestingAreaName);
    EXPECT_EQ(sptInfos.infos_ref()->size(), 2);
    EXPECT_EQ(sptInfos.infos_ref()->count("r0"), 1);
    EXPECT_EQ(sptInfos.infos_ref()->count("r1"), 1);

    // validate root r0
    const auto& sptInfo0 = sptInfos.infos_ref()->at("r0");
    EXPECT_TRUE(*sptInfo0.passive_ref());
    EXPECT_EQ(*sptInfo0.cost_ref(), 1);
    EXPECT_EQ(sptInfo0.parent_ref(), "r0");
    EXPECT_EQ(sptInfo0.children_ref()->size(), 1);
    EXPECT_EQ(sptInfo0.children_ref()->count("r1"), 1);

    // validate root r1
    const auto& sptInfo1 = sptInfos.infos_ref()->at("r1");
    EXPECT_TRUE(*sptInfo1.passive_ref());
    EXPECT_EQ(*sptInfo1.cost_ref(), 1);
    EXPECT_EQ(sptInfo1.parent_ref(), "r1");
    EXPECT_EQ(sptInfo1.children_ref()->size(), 1);
    EXPECT_EQ(sptInfo1.children_ref()->count("r0"), 1);

    // validate flooding-peer-info
    EXPECT_EQ(*sptInfos.floodRootId_ref(), "r0");
    EXPECT_EQ(sptInfos.floodPeers_ref()->size(), 2);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("r0"), 1);
    EXPECT_EQ(sptInfos.floodPeers_ref()->count("r1"), 1);
  }

  // bring r0-n0 link back up, and validate again
  EXPECT_TRUE(
      r0->addPeer(kTestingAreaName, n0->getNodeId(), n0->getPeerSpec()));
  EXPECT_TRUE(
      n0->addPeer(kTestingAreaName, r0->getNodeId(), r0->getPeerSpec()));

  // let kvstore dual sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

  LOG(INFO) << "Test 5: bring link r0-n0 up...";

  validateAllRootsUpCase();

  // mimic r0 non-graceful shutdown and restart
  // bring r0 down non-gracefully
  r0->delPeer(kTestingAreaName, "n0");
  r0->delPeer(kTestingAreaName, "n1");
  // NOTE: n0, n1 will NOT receive delPeer() command

  // wait 1 sec for r0 comes back up
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

  LOG(INFO) << "Test 6: r0 non-graceful shutdown...";

  // bring r0 back up
  EXPECT_TRUE(
      r0->addPeer(kTestingAreaName, n0->getNodeId(), n0->getPeerSpec()));
  EXPECT_TRUE(
      r0->addPeer(kTestingAreaName, n1->getNodeId(), n1->getPeerSpec()));
  EXPECT_TRUE(
      n0->addPeer(kTestingAreaName, r0->getNodeId(), r0->getPeerSpec()));
  EXPECT_TRUE(
      n1->addPeer(kTestingAreaName, r0->getNodeId(), r0->getPeerSpec()));

  // let kvstore dual sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  validateAllRootsUpCase();
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
  std::vector<KvStoreWrapper*> peerStores;
  for (unsigned int j = 0; j < kNumStores; ++j) {
    auto nodeId = getNodeId(kOriginBase, j);
    auto store = createKvStore(nodeId);
    store->run();
    peerStores.push_back(store);
  }

  // Submit initial value set into all peerStores
  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());

    // Store this in expectedKeyVals
    expectedKeyVals[key] = thriftVal;
  }

  LOG(INFO) << "Starting store under test";

  // set up the store that we'll be testing
  messaging::ReplicateQueue<PeerEvent> myPeerUpdatesQueue;
  auto myNodeId = getNodeId(kOriginBase, kNumStores);
  auto myStore = createKvStore(
      myNodeId, getTestKvConf(), {} /*areas*/, myPeerUpdatesQueue.getReader());
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
      for (auto const& [key, val] : *publication.keyVals_ref()) {
        VLOG(3) << "\tkey: " << key << ", value: " << val.value_ref().value();
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());

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
      for (auto const& [key, val] : *publication.keyVals_ref()) {
        VLOG(3) << "\tkey: " << key << ", value: " << val.value_ref().value();
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
  std::vector<KvStoreWrapper*> stores;
  std::vector<std::string> nodeIdsSeq;
  for (unsigned int i = 0; i < kNumStores; ++i) {
    auto nodeId = getNodeId(kOriginBase, i);
    auto store = createKvStore(nodeId);
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
  thriftValFirst.hash_ref() = generateHash(
      *thriftValFirst.version_ref(),
      *thriftValFirst.originatorId_ref(),
      thriftValFirst.value_ref());

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
  thriftValLast.hash_ref() = generateHash(
      *thriftValLast.version_ref(),
      *thriftValLast.originatorId_ref(),
      thriftValLast.value_ref());

  //
  // We expect test-value-2 because "2" > "1" in tie-breaking
  //
  LOG(INFO) << "Pulling values from every store";

  // We have to wait until we see two updates on the first node and verify them.
  {
    auto pub1 = stores[0]->recvPublication();
    auto pub2 = stores[0]->recvPublication();
    ASSERT_EQ(1, pub1.keyVals_ref()->count(kKeyName));
    ASSERT_EQ(1, pub2.keyVals_ref()->count(kKeyName));
    EXPECT_EQ(thriftValFirst, pub1.keyVals_ref()->at(kKeyName));
    EXPECT_EQ(thriftValLast, pub2.keyVals_ref()->at(kKeyName));

    // Verify nodeIds attribute of publication
    ASSERT_TRUE(pub1.nodeIds_ref().has_value());
    ASSERT_TRUE(pub2.nodeIds_ref().has_value());
    EXPECT_EQ(
        std::vector<std::string>{stores[0]->getNodeId()},
        pub1.nodeIds_ref().value());
    auto expectedNodeIds = nodeIdsSeq;
    std::reverse(std::begin(expectedNodeIds), std::end(expectedNodeIds));
    EXPECT_EQ(expectedNodeIds, pub2.nodeIds_ref().value());
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());

    // Make sure the old value still exists
    EXPECT_EQ(thriftValLast, stores[0]->getKey(kTestingAreaName, kKeyName));
  }
}

TEST_F(KvStoreTestFixture, DumpPrefix) {
  const std::string kOriginBase = "peer-store-";
  const unsigned int kNumStores = 16;

  // Create and start peer-stores
  std::vector<KvStoreWrapper*> peerStores;
  for (unsigned int j = 0; j < kNumStores; ++j) {
    auto nodeId = getNodeId(kOriginBase, j);
    auto store = createKvStore(nodeId);
    store->run();
    peerStores.emplace_back(store);
  }

  // Submit initial value set into all peerStores
  LOG(INFO) << "Submitting initial key-value pairs into peer stores.";

  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());

    // Store this in expectedKeyVals
    if (index % 2 == 0) {
      expectedKeyVals[key] = thriftVal;
    }
    ++index;
  }

  LOG(INFO) << "Starting store under test";

  // set up the extra KvStore that we'll be testing
  auto myNodeId = getNodeId(kOriginBase, kNumStores);
  auto myStore = createKvStore(myNodeId);
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
      for (auto const& [key, val] : *publication.keyVals_ref()) {
        VLOG(3) << "\tkey: " << key << ", value: " << val.value_ref().value();
        keys.insert(key);
      }
    }
  }

  // Verify myStore database. we only want keys with "0" prefix
  std::optional<KvStoreFilters> kvFilters{KvStoreFilters({"0"}, {})};
  EXPECT_EQ(
      expectedKeyVals,
      myStore->dumpAll(kTestingAreaName, std::move(kvFilters)));

  // cleanup
  myStore->stop();
}

/**
 * Start single testable store, and set key values.
 * Try to request for KEY_DUMP with a few keyValHashes.
 * We only supposed to see a dump of those keyVals on which either key is not
 * present in provided keyValHashes or hash differs.
 */
TEST_F(KvStoreTestFixture, DumpDifference) {
  LOG(INFO) << "Starting store under test";

  // set up the store that we'll be testing
  auto myNodeId = "test-node";
  auto myStore = createKvStore(myNodeId);
  myStore->run();

  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
  std::unordered_map<std::string, thrift::Value> peerKeyVals;
  std::unordered_map<std::string, thrift::Value> diffKeyVals;
  const std::unordered_map<std::string, thrift::Value> emptyKeyVals;
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());

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
      generateHash(1, "gotham_city", thrift::Value().value_ref() = strVal));
  peerKeyVals[key] = thriftVal;

  // 2. Query with same snapshot. Expect no changes
  {
    EXPECT_EQ(
        emptyKeyVals, myStore->syncKeyVals(kTestingAreaName, peerKeyVals));
  }

  // 3. Query with different value (change value/hash of test-key-0)
  {
    auto newThriftVal = thriftVal;
    newThriftVal.value_ref() = "why-so-serious";
    newThriftVal.hash_ref() = generateHash(
        *newThriftVal.version_ref(),
        *newThriftVal.originatorId_ref(),
        newThriftVal.value_ref());
    peerKeyVals[key] = newThriftVal; // extra key in local
    EXPECT_EQ(
        emptyKeyVals, myStore->syncKeyVals(kTestingAreaName, peerKeyVals));
  }

  // 3. Query with different originatorID (change originatorID of test-key-0)
  {
    auto newThriftVal = thriftVal;
    *newThriftVal.originatorId_ref() = "gotham_city_1";
    peerKeyVals[key] = newThriftVal; // better orginatorId in local
    EXPECT_EQ(
        emptyKeyVals, myStore->syncKeyVals(kTestingAreaName, peerKeyVals));
  }

  // 4. Query with different ttlVersion (change ttlVersion of test-key-1)
  {
    auto newThriftVal = thriftVal;
    newThriftVal.ttlVersion_ref() = 0xb007;
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
  auto store0 = createKvStore("store0");
  auto store1Conf = getTestKvConf();
  store1Conf.ttl_decrement_ms_ref() = 300;
  auto store1 = createKvStore("store1", store1Conf);
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
  thriftVal1.hash_ref() = generateHash(
      *thriftVal1.version_ref(),
      *thriftVal1.originatorId_ref(),
      thriftVal1.value_ref());
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "key1", thriftVal1));
  {
    /* check key is in store1 */
    auto getRes1 = store1->getKey(kTestingAreaName, "key1");
    ASSERT_TRUE(getRes1.has_value());

    /* check key synced from store1 has ttl that is reduced by ttlDecr. */
    auto getPub0 = store0->recvPublication();
    ASSERT_EQ(1, getPub0.keyVals_ref()->count("key1"));
    EXPECT_LE(
        *getPub0.keyVals_ref()->at("key1").ttl_ref(),
        ttl1 - *store1Conf.ttl_decrement_ms_ref());
  }

  /* Add another key with TTL < ttlDecr, and check it's not synced */
  int64_t ttl2 = *store1Conf.ttl_decrement_ms_ref() - 1;
  auto thriftVal2 = createThriftValue(
      1 /* version */,
      "utest" /* originatorId */,
      "value" /* value */,
      ttl2 /* ttl */,
      1 /* ttl version */,
      0 /* hash */);
  thriftVal2.hash_ref() = generateHash(
      *thriftVal2.version_ref(),
      *thriftVal2.originatorId_ref(),
      thriftVal2.value_ref());
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
  // use prod syncInterval 60s
  thrift::KvstoreConfig prodConf, rateLimitConf;
  const size_t messageRate{10}, burstSize{50};
  auto floodRate = createKvstoreFloodRate(
      messageRate /*flood_msg_per_sec*/, burstSize /*flood_msg_burst_size*/);
  rateLimitConf.flood_rate_ref() = floodRate;

  auto store0 = createKvStore("store0", prodConf);
  auto store1 = createKvStore("store1", rateLimitConf);
  auto store2 = createKvStore("store2", prodConf);

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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());
    if (expectNumKeys == 10) {
      // we should be able to set thousands of keys wihtin 5 seconds,
      // pick one of them and let it be set by store0, all others set by store2
      *thriftVal.originatorId_ref() = "store0";
      EXPECT_TRUE(store0->setKey(kTestingAreaName, key, thriftVal));
    } else {
      *thriftVal.originatorId_ref() = "store2";
      EXPECT_TRUE(store2->setKey(kTestingAreaName, key, thriftVal));
    }

    elapsedTime1 =
        duration_cast<seconds>(steady_clock::now() - startTime1).count();
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (elapsedTime1 < duration1);

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
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
  auto rateLimitConf = getTestKvConf();
  auto floodRate = createKvstoreFloodRate(
      messageRate /*flood_msg_per_sec*/, burstSize /*flood_msg_burst_size*/);
  rateLimitConf.flood_rate_ref() = floodRate;

  auto store0 = createKvStore("store0");
  auto store1 = createKvStore("store1", rateLimitConf);
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());
    EXPECT_TRUE(store0->setKey(kTestingAreaName, "key1", thriftVal));
    elapsedTime1 =
        duration_cast<seconds>(steady_clock::now() - startTime1).count();
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (elapsedTime1 < duration1);

  // sleep to get tokens replenished since store1 also floods keys it receives
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

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
  const int wait = 2; // in seconds
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());
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
  EXPECT_EQ(i2, *getRes->ttlVersion_ref());

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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());
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
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());
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
  auto storeA = createKvStore("storeA");
  auto storeB = createKvStore("storeB");
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
    val.hash_ref() = generateHash(
        *val.version_ref(), *val.originatorId_ref(), val.value_ref());
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
      val.value_ref() = "a"; // set same value for k1
    }
    val.hash_ref() = generateHash(
        *val.version_ref(), *val.originatorId_ref(), val.value_ref());
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
          EXPECT_EQ(valA->value_ref().value(), valB->value_ref().value());
          EXPECT_EQ(*valA->version_ref(), *valB->version_ref());
        }
        auto v0 = storeA->getKey(kTestingAreaName, k0);
        EXPECT_EQ(*v0->version_ref(), 5);
        EXPECT_EQ(v0->value_ref().value(), "a");
        auto v1 = storeA->getKey(kTestingAreaName, k1);
        EXPECT_EQ(*v1->version_ref(), 1);
        EXPECT_EQ(v1->value_ref().value(), "a");
        auto v2 = storeA->getKey(kTestingAreaName, k2);
        EXPECT_EQ(*v2->version_ref(), 9);
        EXPECT_EQ(v2->value_ref().value(), "a");
        auto v3 = storeA->getKey(kTestingAreaName, k3);
        EXPECT_EQ(*v3->version_ref(), 9);
        EXPECT_EQ(v3->value_ref().value(), "b");
        auto v4 = storeA->getKey(kTestingAreaName, k4);
        EXPECT_EQ(*v4->version_ref(), 6);
        EXPECT_EQ(v4->value_ref().value(), "b");
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
  pod.area_id_ref() = "pod-area";
  pod.neighbor_regexes_ref()->emplace_back(".*");
  plane.area_id_ref() = "plane-area";
  plane.neighbor_regexes_ref()->emplace_back(".*");
  AreaId podAreaId{pod.get_area_id()};
  AreaId planeAreaId{plane.get_area_id()};

  auto storeA = createKvStore("storeA", getTestKvConf(), {pod});
  auto storeB = createKvStore("storeB", getTestKvConf(), {pod, plane});
  auto storeC = createKvStore("storeC", getTestKvConf(), {plane});

  std::unordered_map<std::string, thrift::Value> expectedKeyValsPod{};
  std::unordered_map<std::string, thrift::Value> expectedKeyValsPlane{};

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
  thriftVal0.hash_ref() = generateHash(
      *thriftVal0.version_ref(),
      *thriftVal0.originatorId_ref(),
      thriftVal0.value_ref());

  keyVal0Size = k0.size() + thriftVal0.originatorId_ref()->size() +
      thriftVal0.value_ref()->size() + fixed_size;

  thrift::Value thriftVal1 = createThriftValue(
      1 /* version */,
      "storeB" /* originatorId */,
      "valueB" /* value */,
      Constants::kTtlInfinity /* ttl */);
  thriftVal1.hash_ref() = generateHash(
      *thriftVal1.version_ref(),
      *thriftVal1.originatorId_ref(),
      thriftVal1.value_ref());

  keyVal1Size = k1.size() + thriftVal1.originatorId_ref()->size() +
      thriftVal1.value_ref()->size() + fixed_size;

  thrift::Value thriftVal2 = createThriftValue(
      1 /* version */,
      "storeC" /* originatorId */,
      std::string("valueC") /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  thriftVal2.hash_ref() = generateHash(
      *thriftVal2.version_ref(),
      *thriftVal2.originatorId_ref(),
      thriftVal2.value_ref());

  keyVal2Size = k2.size() + thriftVal2.originatorId_ref()->size() +
      thriftVal2.value_ref()->size() + fixed_size;

  thrift::Value thriftVal3 = createThriftValue(
      1 /* version */,
      "storeC" /* originatorId */,
      "valueC" /* value */,
      Constants::kTtlInfinity /* ttl */);
  thriftVal3.hash_ref() = generateHash(
      *thriftVal3.version_ref(),
      *thriftVal3.originatorId_ref(),
      thriftVal3.value_ref());

  keyVal3Size = k3.size() + thriftVal3.originatorId_ref()->size() +
      thriftVal3.value_ref()->size() + fixed_size;

  {
    storeA->run();
    storeB->run();
    storeC->run();

    storeA->addPeer(podAreaId, "storeB", storeB->getPeerSpec());
    storeB->addPeer(podAreaId, "storeA", storeA->getPeerSpec());
    storeB->addPeer(planeAreaId, "storeC", storeC->getPeerSpec());
    storeC->addPeer(planeAreaId, "storeB", storeB->getPeerSpec());
    // verify get peers command
    std::unordered_map<std::string, thrift::PeerSpec> expectedPeersPod = {
        {storeA->getNodeId(),
         storeA->getPeerSpec(thrift::KvStorePeerState::INITIALIZED)},
    };
    std::unordered_map<std::string, thrift::PeerSpec> expectedPeersPlane = {
        {storeC->getNodeId(),
         storeC->getPeerSpec(thrift::KvStorePeerState::INITIALIZED)},
    };

    waitForAllPeersInitialized();

    EXPECT_EQ(expectedPeersPod, storeB->getPeers(podAreaId));
    EXPECT_EQ(expectedPeersPlane, storeB->getPeers(planeAreaId));
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
        pod.get_area_id(), plane.get_area_id(), kTestingAreaName};
    std::set<std::string> areaSetEmpty{};
    std::map<std::string, int> storeBTest{};

    auto summary = storeA->getSummary(areaSetAll);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, summary.at(0).get_keyValsCount());
    EXPECT_EQ(summary.at(0).get_area(), pod.get_area_id());
    EXPECT_EQ(keyVal0Size + keyVal1Size, summary.at(0).get_keyValsBytes());

    summary = storeA->getSummary(areaSetEmpty);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, summary.at(0).get_keyValsCount());
    EXPECT_EQ(summary.at(0).get_area(), pod.get_area_id());
    EXPECT_EQ(keyVal0Size + keyVal1Size, summary.at(0).get_keyValsBytes());

    summary = storeB->getSummary(areaSetAll);
    EXPECT_EQ(2, summary.size());
    EXPECT_EQ(2, summary.at(0).get_keyValsCount());
    EXPECT_EQ(2, summary.at(1).get_keyValsCount());
    // for storeB, spanning 2 areas, check that kv count for all areas add
    // up individually
    storeBTest[summary.at(0).get_area()] = summary.at(0).get_keyValsBytes();
    storeBTest[summary.at(1).get_area()] = summary.at(1).get_keyValsBytes();
    EXPECT_EQ(1, storeBTest.count(plane.get_area_id()));
    EXPECT_EQ(keyVal2Size + keyVal3Size, storeBTest[plane.get_area_id()]);
    EXPECT_EQ(1, storeBTest.count(pod.get_area_id()));
    EXPECT_EQ(keyVal0Size + keyVal1Size, storeBTest[pod.get_area_id()]);

    summary = storeB->getSummary(areaSetEmpty);
    EXPECT_EQ(2, summary.size());
    EXPECT_EQ(2, summary.at(0).get_keyValsCount());
    EXPECT_EQ(2, summary.at(1).get_keyValsCount());

    summary = storeC->getSummary(areaSetAll);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, summary.at(0).get_keyValsCount());
    EXPECT_EQ(summary.at(0).get_area(), plane.get_area_id());
    EXPECT_EQ(keyVal2Size + keyVal3Size, summary.at(0).get_keyValsBytes());

    summary = storeC->getSummary(areaSetEmpty);
    EXPECT_EQ(1, summary.size());
    EXPECT_EQ(2, summary.at(0).get_keyValsCount());
    EXPECT_EQ(summary.at(0).get_area(), plane.get_area_id());
    EXPECT_EQ(keyVal2Size + keyVal3Size, summary.at(0).get_keyValsBytes());
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
  thrift::AreaConfig defaultAreaConfig, nonDefaultAreaConfig;
  defaultAreaConfig.area_id_ref() = Constants::kDefaultArea.toString();
  defaultAreaConfig.neighbor_regexes_ref()->emplace_back(".*");
  nonDefaultAreaConfig.area_id_ref() = kTestingAreaName;
  nonDefaultAreaConfig.neighbor_regexes_ref()->emplace_back(".*");
  AreaId defaultAreaId{defaultAreaConfig.get_area_id()};

  auto storeA = createKvStore("storeA", getTestKvConf(), {defaultAreaConfig});
  auto storeB =
      createKvStore("storeB", getTestKvConf(), {nonDefaultAreaConfig});
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

    val.hash_ref() = generateHash(
        *val.version_ref(), *val.originatorId_ref(), val.value_ref());
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
      val.value_ref() = "a"; // set same value for k1
    }
    val.hash_ref() = generateHash(
        *val.version_ref(), *val.originatorId_ref(), val.value_ref());
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
          EXPECT_EQ(valA->value_ref().value(), valB->value_ref().value());
          EXPECT_EQ(*valA->version_ref(), *valB->version_ref());
        }
        auto v0 = storeA->getKey(kTestingAreaName, k0);
        EXPECT_EQ(*v0->version_ref(), 5);
        EXPECT_EQ(v0->value_ref().value(), "a");
        auto v1 = storeA->getKey(kTestingAreaName, k1);
        EXPECT_EQ(*v1->version_ref(), 1);
        EXPECT_EQ(v1->value_ref().value(), "a");
        auto v2 = storeA->getKey(kTestingAreaName, k2);
        EXPECT_EQ(*v2->version_ref(), 9);
        EXPECT_EQ(v2->value_ref().value(), "a");
        auto v3 = storeA->getKey(kTestingAreaName, k3);
        EXPECT_EQ(*v3->version_ref(), 9);
        EXPECT_EQ(v3->value_ref().value(), "b");
        auto v4 = storeA->getKey(kTestingAreaName, k4);
        EXPECT_EQ(*v4->version_ref(), 6);
        EXPECT_EQ(v4->value_ref().value(), "b");
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
