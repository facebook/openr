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
#include <openr/config/tests/Utils.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;
using apache::thrift::CompactSerializer;
using namespace std::chrono;

namespace fb303 = facebook::fb303;

namespace {
// the size of the value string
const uint32_t kValueStrSize = 64;

// interval for periodic syncs
const std::chrono::seconds kDbSyncInterval(1);

// wait time before checking counter
const std::chrono::milliseconds counterUpdateWaitTime(5500);

// Timeout of checking peers in all KvStores are initialized.
const std::chrono::milliseconds kTimeoutOfAllPeersInitialized(1000);

// Timeout of checking keys are propagated in all KvStores in the same area.
const std::chrono::milliseconds kTimeoutOfKvStorePropagation(500);

// TTL in ms
const int64_t kTtlMs = 1000;

/**
 * produce a random string of given length - for value generation
 */
std::string
genRandomStr(const int len) {
  std::string s;
  s.resize(len);

  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

  for (int i = 0; i < len; ++i) {
    s[i] = alphanum[folly::Random::rand32() % (sizeof(alphanum) - 1)];
  }

  return s;
}

thrift::KvstoreConfig
getTestKvConf() {
  thrift::KvstoreConfig kvConf;
  kvConf.sync_interval_s_ref() = kDbSyncInterval.count();
  return kvConf;
}

/**
 * Fixture for abstracting out common functionality for unittests.
 */
class KvStoreTestFixture : public ::testing::TestWithParam<bool> {
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
      const std::vector<thrift::AreaConfig>& areas = {}) {
    auto tConfig = getBasicOpenrConfig(nodeId, "domain", areas);
    *tConfig.kvstore_config_ref() = kvStoreConf;
    config_ = std::make_shared<Config>(tConfig);

    stores_.emplace_back(std::make_unique<KvStoreWrapper>(context, config_));
    return stores_.back().get();
  }

  /**
   * Utility function to create a nodeId/originId based on it's index and
   * prefix
   */
  std::string
  getNodeId(const std::string& prefix, const int index) const {
    return folly::sformat("{}{}", prefix, index);
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
  fbzmq::Context context;

  // Internal stores
  std::vector<std::unique_ptr<KvStoreWrapper>> stores_{};

  // config
  std::shared_ptr<Config> config_;
};

class KvStoreTestTtlFixture : public KvStoreTestFixture {
 public:
  /**
   * Helper function to perform basic KvStore synchronization tests. We generate
   * random topologies and perform same test on all of them.
   *
   * @param adjacencyList: index => set<neighbor-node index>
   * @param kOriginBase the base name for the kv servers
   * @param kNumStores how many stores/syncers to create
   * @param kNumIter number of full swing iteration of key generation to do
   * @param kNumKeys how many key/values to create
   */
  void
  performKvStoreSyncTest(
      std::vector<std::unordered_set<int>> adjacencyList,
      const std::string kOriginBase,
      const unsigned int kNumStores = 16,
      const unsigned int kNumIter = 17,
      const unsigned int kNumKeys = 8) {
    // Some sanity checks
    CHECK_EQ(kNumStores, adjacencyList.size()) << "Incomplete adjacencies.";

    // Create and start stores
    VLOG(1) << "Creating and starting stores ...";
    std::vector<KvStoreWrapper*> stores;
    for (unsigned int i = 0; i < kNumStores; ++i) {
      const auto nodeId = getNodeId(kOriginBase, i);
      LOG(INFO) << "Creating store " << nodeId;
      stores.emplace_back(createKvStore(nodeId));
      stores.back()->run();
    }

    // Add neighbors
    LOG(INFO) << "Adding neighbors ...";
    for (unsigned int i = 0; i < kNumStores; ++i) {
      const auto nodeId = getNodeId(kOriginBase, i);
      for (const auto j : adjacencyList[i]) {
        const auto neighborId = getNodeId(kOriginBase, j);
        LOG(INFO) << "Adding neighbor " << neighborId << " to store " << nodeId;
        EXPECT_TRUE(stores[i]->addPeer(
            kTestingAreaName,
            stores[j]->getNodeId(),
            stores[j]->getPeerSpec()));
      }
    }

    // Expected global Key-Value database
    std::unordered_map<std::string, thrift::Value> expectedGlobalKeyVals;

    // For each `key`, generate random `value` and submit it to any random
    // store. After submission of all keys, make sure all KvStore are in
    // consistent state. We perform the same thing `kNumIter` times.
    int64_t version = 1;
    CHECK(kNumIter > kNumStores) << "Number of iterations must be > num stores";
    for (unsigned int i = 0; i < kNumIter; ++i, ++version) {
      LOG(INFO) << "KeyValue Synchronization Test. Iteration# " << i;
      auto startTime = steady_clock::now();

      // we'll save expected keys and values which will be updated in all
      // KvStores in this iteration.
      std::map<std::string, thrift::Value> expectedKeyVals;

      // Submit a bunch of keys/values to a random store.
      for (unsigned int j = 0; j < kNumKeys; ++j) {
        // Create new key-value pair.
        const std::string key = folly::sformat("key-{}-{}", i, j);
        const auto value = genRandomStr(kValueStrSize);
        thrift::Value thriftVal(
            apache::thrift::FRAGILE,
            version,
            "pluto" /* originatorId */,
            value,
            ttl /* ttl */,
            0 /* ttl version */,
            0 /* hash*/);

        // Submit this to a random store.
        auto& store = stores[folly::Random::rand32() % stores.size()];
        EXPECT_TRUE(store->setKey(kTestingAreaName, key, thriftVal));
        const auto dump = store->dumpAll(kTestingAreaName);
        EXPECT_FALSE(dump.empty());
        // Verify 1. hash is updated in KvStore
        // 2. dumpHashes request returns key values as expected
        const auto hashDump = store->dumpHashes(kTestingAreaName);
        for (const auto& [key, value] : dump) {
          EXPECT_TRUE(value.hash_ref().value() != 0);
          EXPECT_TRUE(hashDump.count(key) != 0);
          if (!hashDump.count(key)) {
            continue;
          }
          EXPECT_EQ(
              value.hash_ref().value(), hashDump.at(key).hash_ref().value());
        }

        // Update hash
        thriftVal.hash_ref() = generateHash(
            *thriftVal.version_ref(),
            *thriftVal.originatorId_ref(),
            thriftVal.value_ref());
        // Add this to expected key/vals
        expectedKeyVals.emplace(key, thriftVal);
        expectedGlobalKeyVals.emplace(key, thriftVal);
      } // for `j < kNumKeys`
      LOG(INFO) << "Done submitting key-value pairs. Iteration# " << i;

      // We just generated kNumKeys `new` keys-vals with random values and
      // submitted each one in a different Publication. So we must receive
      // exactly kNumKeys key-value updates from all KvStores.
      // NOTE: It is not necessary to receive kNumKeys publications. Just one
      // publication can be published for all changes and depends on internal
      // implementation of KvStore (we do not rely on it).
      if (not checkTtl) {
        LOG(INFO) << "Expecting publications from stores. Iteration# " << i;
        for (unsigned int j = 0; j < kNumStores; ++j) {
          auto& store = stores[j];

          // Receive all expected number of updates.
          std::map<std::string, thrift::Value> receivedKeyVals;
          while (receivedKeyVals.size() < kNumKeys) {
            auto publication = store->recvPublication();
            for (auto const& kv : *publication.keyVals_ref()) {
              receivedKeyVals.emplace(kv);
            }
          }

          // Verify expectations for kv-store updates
          EXPECT_EQ(kNumKeys, receivedKeyVals.size());
          EXPECT_EQ(expectedKeyVals, receivedKeyVals);

          // Print for debugging.
          VLOG(4) << "Store " << store->getNodeId() << " received keys.";
          for (auto const& kv : receivedKeyVals) {
            VLOG(4) << "\tkey: " << kv.first
                    << ", value: " << kv.second.value_ref().value()
                    << ", version: " << *kv.second.version_ref();
          }
        } // for `j < kNumStores`

        // Verify the global key-value database from all nodes
        for (auto& store : stores) {
          const auto dump = store->dumpAll(kTestingAreaName);
          const auto hashDump = store->dumpHashes(kTestingAreaName);
          EXPECT_EQ(expectedGlobalKeyVals, dump);
          for (const auto& [key, value] : dump) {
            EXPECT_TRUE(value.hash_ref().value() != 0);
            EXPECT_TRUE(hashDump.count(key) != 0);
            EXPECT_EQ(
                value.hash_ref().value(), hashDump.at(key).hash_ref().value());
          }
        }
      } else {
        // wait for all keys to expire
        size_t iter{0};
        while (true) {
          LOG(INFO) << "Checking for empty stores. Iter#" << ++iter;
          bool allStoreEmpty = true;
          for (auto& store : stores) {
            auto keyVals = store->dumpAll(kTestingAreaName);
            if (not keyVals.empty()) {
              VLOG(2) << store->getNodeId() << " still has " << keyVals.size()
                      << " keys remaining";
              for (auto& kv : keyVals) {
                VLOG(2) << "  " << kv.first
                        << ", ttl: " << *kv.second.ttl_ref();
              }
              allStoreEmpty = false;
              break;
            }
          }
          if (allStoreEmpty) {
            break;
          }
          std::this_thread::yield();
        }
        // must expire after ttl
        const int64_t errorMargin = 5; // 5ms for error margin
        const auto elapsedTime =
            duration_cast<milliseconds>(steady_clock::now() - startTime)
                .count();
        LOG(INFO) << "ttl " << ttl << " vs elapsedTime " << elapsedTime;
        EXPECT_LE(
            ttl - Constants::kTtlDecrement.count() - errorMargin, elapsedTime);
      }
    } // for `i < kNumIter`
  }

  // enable TTL check or not
  const bool checkTtl = GetParam();
  const int64_t ttl = checkTtl ? kTtlMs : Constants::kTtlInfinity;
};

INSTANTIATE_TEST_CASE_P(
    KvStoreTestInstance, KvStoreTestTtlFixture, ::testing::Bool());

} // namespace

//
// Test counter reporting
//
TEST_F(KvStoreTestFixture, CounterReport) {
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
  ASSERT_EQ(1, counters.count("kvstore.num_peers"));
  ASSERT_EQ(1, counters.count("kvstore.cmd_peer_dump.count"));
  ASSERT_EQ(1, counters.count("kvstore.cmd_peer_add.count"));
  ASSERT_EQ(1, counters.count("kvstore.cmd_per_del.count"));
  ASSERT_EQ(1, counters.count("kvstore.expired_key_vals.sum"));
  ASSERT_EQ(1, counters.count("kvstore.flood_duration_ms.avg"));
  ASSERT_EQ(1, counters.count("kvstore.full_sync_duration_ms.avg"));
  ASSERT_EQ(1, counters.count("kvstore.peers.bytes_received.sum"));
  ASSERT_EQ(1, counters.count("kvstore.peers.bytes_sent.sum"));
  ASSERT_EQ(1, counters.count("kvstore.rate_limit_keys.avg"));
  ASSERT_EQ(1, counters.count("kvstore.rate_limit_suppress.count"));
  ASSERT_EQ(1, counters.count("kvstore.received_dual_messages.count"));
  ASSERT_EQ(1, counters.count("kvstore.cmd_hash_dump.count"));
  ASSERT_EQ(1, counters.count("kvstore.cmd_key_dump.count"));
  ASSERT_EQ(1, counters.count("kvstore.cmd_key_get.count"));
  ASSERT_EQ(1, counters.count("kvstore.sent_key_vals.sum"));
  ASSERT_EQ(1, counters.count("kvstore.sent_publications.count"));
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
  EXPECT_EQ(0, counters.at("kvstore.cmd_key_dump.count"));
  EXPECT_EQ(0, counters.at("kvstore.cmd_key_get.count"));
  EXPECT_EQ(0, counters.at("kvstore.sent_key_vals.sum"));
  EXPECT_EQ(0, counters.at("kvstore.sent_publications.count"));

  // Verify four keys were set
  ASSERT_EQ(1, counters.count("kvstore.cmd_key_set.count"));
  EXPECT_EQ(4, counters.at("kvstore.cmd_key_set.count"));
  ASSERT_EQ(1, counters.count("kvstore.received_key_vals.sum"));
  EXPECT_EQ(4, counters.at("kvstore.received_key_vals.sum"));

  // Verify the key and the number of key
  ASSERT_TRUE(kvStore->getKey(kTestingAreaName, "test-key2").has_value());
  ASSERT_EQ(1, counters.count("kvstore.num_keys"));
  int expect_num_key = 1;
  EXPECT_EQ(expect_num_key, counters.at("kvstore.num_keys"));

  // Verify the number key update
  ASSERT_EQ(1, counters.count("kvstore.updated_key_vals.sum"));
  EXPECT_EQ(2, counters.at("kvstore.updated_key_vals.sum"));

  // Verify publication counter
  ASSERT_EQ(1, counters.count("kvstore.looped_publications.count"));
  EXPECT_EQ(1, counters.at("kvstore.looped_publications.count"));
  ASSERT_EQ(1, counters.count("kvstore.received_publications.count"));
  EXPECT_EQ(4, counters.at("kvstore.received_publications.count"));

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
  const thrift::Value value(
      apache::thrift::FRAGILE,
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
  // 2. Advertise key-value with just below kTtlThreshold.
  // - Ensure we don't receive it over publication but do in GET request
  //
  {
    auto thriftValue = value;
    thriftValue.ttl_ref() = Constants::kTtlThreshold.count() - 1;
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

    // We will receive key-expiry publication but no key-advertisement
    auto publication = kvStore->recvPublication();
    EXPECT_EQ(0, publication.keyVals_ref()->size());
    ASSERT_EQ(1, publication.expiredKeys_ref()->size());
    EXPECT_EQ(key, publication.expiredKeys_ref()->at(0));
  }

  //
  // 3. Advertise key with long enough ttl, so that it doesn't expire
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
  // 4. Advertise ttl-update to set it to new value
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
  // 5. Set ttl of key to INFINITE
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
  thrift::Value thriftVal(
      apache::thrift::FRAGILE,
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
  thrift::Value thriftVal2(
      apache::thrift::FRAGILE,
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
  thrift::Value thriftVal3(
      apache::thrift::FRAGILE,
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
  thrift::Value thriftVal4(
      apache::thrift::FRAGILE,
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
  thrift::Value thriftVal5(
      apache::thrift::FRAGILE,
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
  thrift::Value thriftVal6(
      apache::thrift::FRAGILE,
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

  thrift::Value thriftVal1(
      apache::thrift::FRAGILE,
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

  thrift::Value thriftVal2(
      apache::thrift::FRAGILE,
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
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(store1->addPeer(
      kTestingAreaName, store0->getNodeId(), store0->getPeerSpec()));
  // wait to sync kvstore
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  // key 'test1' should be added with remaining TTL
  maybeThriftVal = store1->getKey(kTestingAreaName, "test1");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs - 200, *maybeThriftVal.value().ttl_ref());

  // key 'test2' should not be updated, it should have kTtlMs
  maybeThriftVal = store1->getKey(kTestingAreaName, "test2");
  ASSERT_TRUE(maybeThriftVal.has_value());
  EXPECT_GE(kTtlMs, *maybeThriftVal.value().ttl_ref());
}

/**
 * Test to verify PEER_ADD/PEER_DEL and verify that keys are synchronized
 * to the neighbor.
 * 1. Start store0, store1 and store2
 * 2. Add store1 and store2 to store0
 * 3. Advertise keys in store1 and store2
 * 4. Verify that they appear in store0
 * 5. Update store1 peer definitions in store0
 * 6. Update keys in store1
 * 7. Verify that they appear in store0 (can only happen via full-sync)
 * 8. Verify PEER_DEL API
 */
TEST_F(KvStoreTestFixture, PeerAddUpdateRemove) {
  auto store0 = createKvStore("store0");
  auto store1 = createKvStore("store1");
  auto store2 = createKvStore("store2");

  // Start stores in their respective threads.
  store0->run();
  store1->run();
  store2->run();

  // Add peers to store0
  EXPECT_TRUE(store0->addPeer(
      kTestingAreaName, store1->getNodeId(), store1->getPeerSpec()));
  EXPECT_TRUE(store0->addPeer(
      kTestingAreaName, store2->getNodeId(), store2->getPeerSpec()));

  waitForAllPeersInitialized();
  // map of peers we expect and dump peers to expect the results.
  std::unordered_map<std::string, thrift::PeerSpec> expectedPeers = {
      {store1->getNodeId(),
       store1->getPeerSpec(thrift::KvStorePeerState::INITIALIZED)},
      {store2->getNodeId(),
       store2->getPeerSpec(thrift::KvStorePeerState::INITIALIZED)},
  };
  EXPECT_EQ(expectedPeers, store0->getPeers(kTestingAreaName));

  // Set key into store1 and make sure it appears in store0
  LOG(INFO) << "Setting value in store1...";
  thrift::Value thriftVal(
      apache::thrift::FRAGILE,
      1 /* version */,
      "1.2.3.4" /* originatorId */,
      "value1" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "test", thriftVal));
  // Update hash
  thriftVal.hash_ref() = generateHash(
      *thriftVal.version_ref(),
      *thriftVal.originatorId_ref(),
      thriftVal.value_ref());

  // Receive publication from store0 for new key-update
  LOG(INFO) << "Waiting for message from store0 on pub socket ...";
  auto pub = store0->recvPublication();
  EXPECT_EQ(1, pub.keyVals_ref()->size());
  EXPECT_EQ(thriftVal, pub.keyVals_ref()["test"]);

  // Now play the same trick with the other store
  LOG(INFO) << "Setting value in store2...";
  thriftVal = thrift::Value(
      apache::thrift::FRAGILE,
      2 /* version */,
      "1.2.3.4" /* originatorId */,
      "value2" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  EXPECT_TRUE(store2->setKey(kTestingAreaName, "test", thriftVal));
  // Update hash
  thriftVal.hash_ref() = generateHash(
      *thriftVal.version_ref(),
      *thriftVal.originatorId_ref(),
      thriftVal.value_ref());

  // Receive publication from store0 for new key-update
  LOG(INFO) << "Waiting for message from store0...";
  pub = store0->recvPublication();
  EXPECT_EQ(1, pub.keyVals_ref()->size());
  EXPECT_EQ(thriftVal, pub.keyVals_ref()["test"]);

  // Update store1 with same peer spec
  EXPECT_TRUE(store0->addPeer(
      kTestingAreaName, store1->getNodeId(), store1->getPeerSpec()));
  waitForAllPeersInitialized();
  EXPECT_EQ(expectedPeers, store0->getPeers(kTestingAreaName));

  // Update key
  // Set key into store1 and make sure it appears in store0
  LOG(INFO) << "Updating value in store1 again ...";
  thriftVal = thrift::Value(
      apache::thrift::FRAGILE,
      3 /* version */,
      "1.2.3.4" /* originatorId */,
      "value3" /* value */,
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  EXPECT_TRUE(store1->setKey(kTestingAreaName, "test", thriftVal));
  // Update hash
  thriftVal.hash_ref() = generateHash(
      *thriftVal.version_ref(),
      *thriftVal.originatorId_ref(),
      thriftVal.value_ref());
  // Receive publication from store0 for new key-update
  LOG(INFO) << "Waiting for message from store0 on pub socket ...";
  pub = store0->recvPublication();
  EXPECT_EQ(1, pub.keyVals_ref()->size());
  EXPECT_EQ(thriftVal, pub.keyVals_ref()["test"]);

  // Remove store1 and verify
  LOG(INFO) << "Deleting the peers for store0...";
  store0->delPeer(kTestingAreaName, "store1");
  expectedPeers.clear();
  expectedPeers[store2->getNodeId()] =
      store2->getPeerSpec(thrift::KvStorePeerState::INITIALIZED);
  EXPECT_EQ(expectedPeers, store0->getPeers(kTestingAreaName));

  // Remove store2 and verify that there are no more peers
  store0->delPeer(kTestingAreaName, "store2");
  const std::unordered_map<std::string, thrift::PeerSpec> emptyPeers{};
  EXPECT_EQ(emptyPeers, store0->getPeers(kTestingAreaName));
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

  //
  // Add peers to all stores
  EXPECT_TRUE(
      r0->addPeer(kTestingAreaName, n0->getNodeId(), n0->getPeerSpec()));
  EXPECT_TRUE(
      r0->addPeer(kTestingAreaName, n1->getNodeId(), n1->getPeerSpec()));

  EXPECT_TRUE(
      r1->addPeer(kTestingAreaName, n0->getNodeId(), n0->getPeerSpec()));
  EXPECT_TRUE(
      r1->addPeer(kTestingAreaName, n1->getNodeId(), n1->getPeerSpec()));

  EXPECT_TRUE(
      n0->addPeer(kTestingAreaName, r0->getNodeId(), r0->getPeerSpec()));
  EXPECT_TRUE(
      n0->addPeer(kTestingAreaName, r1->getNodeId(), r1->getPeerSpec()));

  EXPECT_TRUE(
      n1->addPeer(kTestingAreaName, r0->getNodeId(), r0->getPeerSpec()));
  EXPECT_TRUE(
      n1->addPeer(kTestingAreaName, r1->getNodeId(), r1->getPeerSpec()));

  // let kvstore dual sync
  /* sleep override */
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

  // case1. validate all roots up case
  validateAllRootsUpCase();

  // case2. validate r0 down case
  // everybody should pick r1 as new root
  r0->delPeer(kTestingAreaName, "n0");
  r0->delPeer(kTestingAreaName, "n1");
  n0->delPeer(kTestingAreaName, "r0");
  n1->delPeer(kTestingAreaName, "r0");

  // let kvstore dual sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

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

  // case3. bring r0 back up, and validate again
  // everybody should pick r0 as new root
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

  // case4. bring link r0-n0 down
  // verify topology below
  //  r0    r1
  //   \   /|
  //    \_/ |
  //   /  \ |
  // n0    n1
  r0->delPeer(kTestingAreaName, "n0");
  n0->delPeer(kTestingAreaName, "r0");

  // let kvstore dual sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

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

  // case5. bring r0-n1 link back up, and validate again
  EXPECT_TRUE(
      r0->addPeer(kTestingAreaName, n0->getNodeId(), n0->getPeerSpec()));
  EXPECT_TRUE(
      n0->addPeer(kTestingAreaName, r0->getNodeId(), r0->getPeerSpec()));

  // let kvstore dual sync
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  validateAllRootsUpCase();

  // case6. mimic r0 non-graceful shutdown and restart
  // bring r0 down non-gracefully
  r0->delPeer(kTestingAreaName, "n0");
  r0->delPeer(kTestingAreaName, "n1");
  // NOTE: n0, n1 will NOT receive delPeer() command

  // wait 1 sec for r0 comes back up
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));

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
 * Perform KvStore synchronization test on full mesh.
 */
TEST_P(KvStoreTestTtlFixture, FullMesh) {
  // how many stores/syncers to create
  const unsigned int kNumStores = 8;

  // organize peering in a full mesh
  std::vector<std::unordered_set<int>> adjacencyList;
  for (unsigned int i = 0; i < kNumStores; ++i) {
    std::unordered_set<int> neighbors;
    for (unsigned int j = 0; j < kNumStores; ++j) {
      if (i == j) {
        continue;
      }
      neighbors.insert(j);
    }
    adjacencyList.push_back(neighbors);
  }

  performKvStoreSyncTest(
      adjacencyList, "kv_store_fullmesh::store", kNumStores, kNumStores + 1);
}

/**
 * Perform KvStore synchronization test on circular ring topology.
 */
TEST_P(KvStoreTestTtlFixture, Ring) {
  // how many stores/syncers to create
  const int kNumStores = 16;

  // organize peering in a ring
  std::vector<std::unordered_set<int>> adjacencyList;
  adjacencyList.push_back({kNumStores - 1, 0}); // Add a special case.
  for (int i = 1; i < kNumStores; ++i) {
    adjacencyList.push_back({(i - 1) % kNumStores, (i + 1) % kNumStores});
  }

  performKvStoreSyncTest(
      adjacencyList, "kv_store_ring::store", kNumStores, kNumStores + 1);
}

/**
 * Create an arbitrary topology using random Albert-Barabasi graph
 */
TEST_P(KvStoreTestTtlFixture, Graph) {
  // how many stores/syncers to create
  const unsigned int kNumStores = 16;
  // how many links for each new node
  const unsigned int kNumLinks = 3;

  // using set to avoid duplicate neighbors
  std::vector<std::unordered_set<int>> adjacencyList;
  // start w/ a core graph by connecting 2 nodes
  adjacencyList.push_back({1});
  adjacencyList.push_back({0});
  // track how many links a node is incident w/ by its occurrences
  std::vector<int> incidentNodes = {0, 1};

  // preferential attachment
  for (unsigned int i = 2; i < kNumStores; ++i) {
    // 2 * # of links
    int totalDegrees = incidentNodes.size();
    std::unordered_set<int> neighbors;
    for (unsigned int j = 0; j < kNumLinks; ++j) {
      auto neighbor = incidentNodes[folly::Random::rand32() % totalDegrees];
      // already connected
      if (adjacencyList[neighbor].count(i)) {
        continue;
      }
      // connect i w/ neighbor, bidirectionally
      adjacencyList[neighbor].insert(i);
      neighbors.insert(neighbor);
      incidentNodes.push_back(i);
      incidentNodes.push_back(neighbor);
    }
    adjacencyList.push_back(neighbors);
  }

  performKvStoreSyncTest(
      adjacencyList, "kv_store_graph::store", kNumStores, kNumStores + 1);
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
    auto key = folly::sformat("test-key-{}", store->getNodeId());
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        1 /* version */,
        "gotham_city" /* originatorId */,
        folly::sformat("test-value-{}", store->getNodeId()),
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
  auto myNodeId = getNodeId(kOriginBase, kNumStores);
  auto myStore = createKvStore(myNodeId);
  myStore->run();

  // NOTE: It is important to add peers after starting our store to avoid
  // race condition where certain updates are lost over PUB/SUB channel
  for (auto& store : peerStores) {
    myStore->addPeer(
        kTestingAreaName, store->getNodeId(), store->getPeerSpec());
    store->addPeer(
        kTestingAreaName, myStore->getNodeId(), myStore->getPeerSpec());
  }

  // Wait for full-sync to complete. Full-sync is complete when all of our
  // neighbors receive all the keys and we must receive `kNumStores`
  // key-value updates from each store over PUB socket.
  LOG(INFO) << "Waiting for full sync to complete.";
  for (auto& store : stores_) {
    std::unordered_set<std::string> keys;
    VLOG(3) << "Store " << store->getNodeId() << " received keys.";
    while (keys.size() < kNumStores) {
      auto publication = store->recvPublication();
      for (auto const& kv : *publication.keyVals_ref()) {
        VLOG(3) << "\tkey: " << kv.first
                << ", value: " << kv.second.value_ref().value();
        keys.insert(kv.first);
      }
    }
  }

  // Verify myStore database
  EXPECT_EQ(expectedKeyVals, myStore->dumpAll(kTestingAreaName));

  //
  // Submit another range of values
  //
  LOG(INFO) << "Submitting the second round of key-values...";
  for (auto& store : peerStores) {
    auto key = folly::sformat("test-key-{}", store->getNodeId());
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        2 /* version */,
        "gotham_city" /* originatorId */,
        folly::sformat("test-value-new-{}", store->getNodeId()),
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
      for (auto const& kv : *publication.keyVals_ref()) {
        VLOG(3) << "\tkey: " << kv.first
                << ", value: " << kv.second.value_ref().value();
        keys.insert(kv.first);
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
    auto key = folly::sformat("flood-test-key-1", store->getNodeId());
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        2 /* version */,
        "gotham_city" /* originatorId */,
        folly::sformat("flood-test-value-1", store->getNodeId()),
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
  }

  // need to wait on this for the list of nodeIds to be as expected.
  waitForAllPeersInitialized();

  //
  // Submit same key in store 0 and store N-1, use same version
  // but different values
  //
  LOG(INFO) << "Submitting key-values from first and last store";

  // set a key from first store
  thrift::Value thriftValFirst(
      apache::thrift::FRAGILE,
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
  thrift::Value thriftValLast(
      apache::thrift::FRAGILE,
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
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
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
    peerStores.push_back(store);
  }

  // Submit initial value set into all peerStores
  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
  LOG(INFO) << "Submitting initial key-value pairs into peer stores.";
  int i = 0;
  for (auto& store : peerStores) {
    auto key = folly::sformat("{}-test-key-{}", i % 2, store->getNodeId());
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        1 /* version */,
        "gotham_city" /* originatorId */,
        folly::sformat("test-value-{}", store->getNodeId()),
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
    if (i % 2 == 0) {
      expectedKeyVals[key] = thriftVal;
    }
    ++i;
  }

  LOG(INFO) << "Starting store under test";

  // set up the store that we'll be testing
  auto myNodeId = getNodeId(kOriginBase, kNumStores);
  auto myStore = createKvStore(myNodeId);
  myStore->run();

  // NOTE: It is important to add peers after starting our store to avoid
  // race condition where certain updates are lost over PUB/SUB channel
  for (auto& store : peerStores) {
    myStore->addPeer(
        kTestingAreaName, store->getNodeId(), store->getPeerSpec());
  }

  // Wait for full-sync to complete. Full-sync is complete when all of our
  // neighbors receive all the keys and we must receive `kNumStores`
  // key-value updates from each store over PUB socket.
  LOG(INFO) << "Waiting for full sync to complete.";
  {
    std::unordered_set<std::string> keys;
    VLOG(3) << "Store " << myStore->getNodeId() << " received keys.";
    while (keys.size() < kNumStores) {
      auto publication = myStore->recvPublication();
      for (auto const& kv : *publication.keyVals_ref()) {
        VLOG(3) << "\tkey: " << kv.first
                << ", value: " << kv.second.value_ref().value();
        keys.insert(kv.first);
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
    const auto key = folly::sformat("test-key-{}", i);
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        1 /* version */,
        "gotham_city" /* originatorId */,
        folly::sformat("test-value-{}", myStore->getNodeId()),
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
  const auto strVal = folly::sformat("test-value-{}", myStore->getNodeId());
  const thrift::Value thriftVal(
      apache::thrift::FRAGILE,
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

/**
 * Start single testable store, and set key values with oneway method. Verify
 * content of KvStore by querying it.
 */
TEST_F(KvStoreTestFixture, OneWaySetKey) {
  LOG(INFO) << "Starting test KvStore";

  // set up the store that we'll be testing
  const auto myNodeId = "test-node1";
  auto myStore = createKvStore(myNodeId);
  myStore->run();

  // Set key via KvStoreWrapper::setKey
  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
  const auto key = folly::sformat("test-key");
  const thrift::Value thriftVal(
      apache::thrift::FRAGILE,
      1 /* version */,
      "gotham_city" /* originatorId */,
      folly::sformat("test-value"),
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      generateHash(
          1,
          "gotham_city",
          thrift::Value().value_ref() = std::string("test-value")));
  expectedKeyVals[key] = thriftVal;
  myStore->setKey(kTestingAreaName, key, thriftVal);

  // Expect both keys in KvStore
  EXPECT_EQ(expectedKeyVals, myStore->dumpAll(kTestingAreaName));
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

  /**
   * check sync works fine, add a key with TTL > ttlDecr in store1,
   * verify key is synced to store0
   */
  int64_t ttl1 = 6000;
  thrift::Value thriftVal1(
      apache::thrift::FRAGILE,
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
  thrift::Value thriftVal2(
      apache::thrift::FRAGILE,
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
  thrift::KvstoreFloodRate floodRate(
      apache::thrift::FRAGILE,
      messageRate /*flood_msg_per_sec*/,
      burstSize /*flood_msg_burst_size*/);
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
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        1 /* version */,
        "store1" /* originatorId */,
        "value" /* value */,
        300000 /* ttl */,
        1 /* ttl version */,
        0 /* hash */);
    std::string key = folly::sformat("key{}", ++expectNumKeys);
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
  thrift::KvstoreFloodRate floodRate(
      apache::thrift::FRAGILE,
      messageRate /*flood_msg_per_sec*/,
      burstSize /*flood_msg_burst_size*/);
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
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
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
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
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
    auto key = folly::sformat("key3{}", ++i3);
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
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
    auto key = folly::sformat("key4{}", ++i4);
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
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
  for (const auto& keyVer : keyVersionAs) {
    thrift::Value val(
        apache::thrift::FRAGILE,
        keyVer.second /* version */,
        "storeA" /* originatorId */,
        "a" /* value */,
        30000 /* ttl */,
        99 /* ttl version */,
        0 /* hash*/);
    val.hash_ref() = generateHash(
        *val.version_ref(), *val.originatorId_ref(), val.value_ref());
    EXPECT_TRUE(storeA->setKey(kTestingAreaName, keyVer.first, val));
  }

  // set key vals in storeB
  for (const auto& keyVer : keyVersionBs) {
    thrift::Value val(
        apache::thrift::FRAGILE,
        keyVer.second /* version */,
        "storeA" /* originatorId */,
        "b" /* value */,
        30000 /* ttl */,
        99 /* ttl version */,
        0 /* hash*/);
    if (keyVer.first == k1) {
      val.value_ref() = "a"; // set same value for k1
    }
    val.hash_ref() = generateHash(
        *val.version_ref(), *val.originatorId_ref(), val.value_ref());
    EXPECT_TRUE(storeB->setKey(kTestingAreaName, keyVer.first, val));
  }

  // storeA has (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 1, a)
  // storeB has             (k1, 1, a), (k2, 1, b), (k3, 9, b), (k4, 6, b)
  // let A sends a full sync request to B and wait for completion
  storeA->addPeer(kTestingAreaName, "storeB", storeB->getPeerSpec());
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

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
  *pod.area_id_ref() = "pod-area";
  pod.neighbor_regexes_ref()->emplace_back(".*");
  *plane.area_id_ref() = "plane-area";
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

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return 1;
  }

  // Run the tests
  return RUN_ALL_TESTS();
}
