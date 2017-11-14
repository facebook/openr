/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sodium.h>
#include <cstdlib>
#include <thread>
#include <unordered_set>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/Random.h>
#include <folly/gen/Base.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;
using apache::thrift::CompactSerializer;
using namespace std::chrono_literals;
using namespace std::chrono;

namespace {

// the size of the value string
const uint32_t kValueStrSize = 64;

// interval for periodic syncs
const std::chrono::seconds kDbSyncInterval(1);
const std::chrono::seconds kMonitorSubmitInterval(3600);

// TTL in ms
const int64_t kTtlMs = 100;

// Timeout for recieving publication from KvStore. This spans the maximum
// duration it can take to propogate an update through KvStores
const std::chrono::seconds kTimeout(12);

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
      std::unordered_map<std::string, thrift::PeerSpec> peers) {
    auto ptr = std::make_unique<KvStoreWrapper>(
        context,
        nodeId,
        kDbSyncInterval,
        kMonitorSubmitInterval,
        std::move(peers));
    stores_.emplace_back(std::move(ptr));
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

  // Public member variables
  fbzmq::Context context;

 private:
  std::vector<std::unique_ptr<KvStoreWrapper>> stores_{};
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
    const std::unordered_map<std::string, thrift::PeerSpec> emptyPeers;
    std::vector<KvStoreWrapper*> stores;
    for (unsigned int i = 0; i < kNumStores; ++i) {
      const auto nodeId = getNodeId(kOriginBase, i);
      VLOG(1) << "Creating store " << nodeId;
      stores.push_back(createKvStore(nodeId, emptyPeers));
      stores.back()->run();
    }

    // Add neighbors
    VLOG(1) << "Adding neighbors ...";
    for (unsigned int i = 0; i < kNumStores; ++i) {
      const auto nodeId = getNodeId(kOriginBase, i);
      for (const auto j : adjacencyList[i]) {
        const auto neighborId = getNodeId(kOriginBase, j);
        VLOG(1) << "Adding neighbor " << neighborId << " to store " << nodeId;
        EXPECT_TRUE(
            stores[i]->addPeer(stores[j]->nodeId, stores[j]->getPeerSpec()));
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
      VLOG(1) << "KeyValue Synchronization Test. Iteration# " << i;
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
        EXPECT_TRUE(store->setKey(key, thriftVal));
        const auto dump = store->dumpAll();
        EXPECT_FALSE(dump.empty());
        // Verify 1. hash is updated in KvStore
        // 2. HASH_DUMP request returns key values as expected
        const auto hashDump = store->dumpHashes();
        for (const auto kv : dump) {
          EXPECT_TRUE(kv.second.hash.value() != 0);
          EXPECT_TRUE(hashDump.count(kv.first) != 0);
          EXPECT_EQ(kv.second.hash.value(), hashDump.at(kv.first).hash.value());
        }

        // Update hash
        thriftVal.hash = generateHash(
            thriftVal.version, thriftVal.originatorId, thriftVal.value);
        // Add this to expected key/vals
        expectedKeyVals.emplace(key, thriftVal);
        expectedGlobalKeyVals.emplace(key, thriftVal);
      } // for `j < kNumKeys`
      VLOG(1) << "Done submitting key-value pairs. Iteration# " << i;

      // We just generated kNumKeys `new` keys-vals with random values and
      // submitted each one in a different Publication. So we must receive
      // exactly kNumKeys key-value updates from all KvStores.
      // NOTE: It is not necessary to receive kNumKeys publications. Just one
      // publication can be published for all changes and depends on internal
      // implementation of KvStore (we do not rely on it).
      if (not checkTtl) {
        VLOG(1) << "Expecting publications from stores. Iteration# " << i;
        for (unsigned int j = 0; j < kNumStores; ++j) {
          auto& store = stores[j];

          // Receive all expected number of updates.
          std::map<std::string, thrift::Value> receivedKeyVals;
          while (receivedKeyVals.size() < kNumKeys) {
            auto publication = store->recvPublication(kTimeout);
            for (auto const& kv : publication.keyVals) {
              receivedKeyVals.emplace(kv);
            }
          }

          // Verify expectations for kv-store updates
          EXPECT_EQ(kNumKeys, receivedKeyVals.size());
          EXPECT_EQ(expectedKeyVals, receivedKeyVals);

          // Print for debugging.
          VLOG(4) << "Store " << store->nodeId << " received keys.";
          for (auto const& kv : receivedKeyVals) {
            VLOG(4) << "\tkey: " << kv.first
                    << ", value: " << kv.second.value.value()
                    << ", version: " << kv.second.version;
          }
        } // for `j < kNumStores`

        // Verify the global key-value database from all nodes
        for (auto& store : stores) {
          const auto dump = store->dumpAll();
          const auto hashDump = store->dumpHashes();
          EXPECT_EQ(expectedGlobalKeyVals, dump);
          for (const auto kv : dump) {
            EXPECT_TRUE(kv.second.hash.value() != 0);
            EXPECT_TRUE(hashDump.count(kv.first) != 0);
            EXPECT_EQ(
                kv.second.hash.value(), hashDump.at(kv.first).hash.value());
          }
        }
      } else {
        // wait for all keys to expire
        while (true) {
          bool allStoreEmpty = true;
          for (auto& store : stores) {
            if (not store->dumpAll().empty()) {
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
        const auto elapsedTime =
            duration_cast<milliseconds>(steady_clock::now() - startTime)
                .count();
        VLOG(2) << "ttl " << ttl << " vs elapsedTime " << elapsedTime;
        EXPECT_LE(ttl, elapsedTime);
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
// validate mergeKeyValues
//
TEST(KvStore, mergeKeyValuesTest) {
  std::unordered_map<std::string, thrift::Value> oldStore;
  std::unordered_map<std::string, thrift::Value> myStore;
  std::unordered_map<std::string, thrift::Value> newStore;

  std::string key{"key"};

  thrift::Value thriftValue(
      apache::thrift::FRAGILE,
      5, /* version */
      "node5", /* node id */
      "dummyValue",
      3600, /* ttl */
      0 /* ttl version */,
      0 /* hash */);
  oldStore.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(key),
      std::forward_as_tuple(thriftValue));
  myStore.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(key),
      std::forward_as_tuple(thriftValue));
  newStore.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(key),
      std::forward_as_tuple(thriftValue));
  auto oldKvIt = oldStore.find(key);
  auto myKvIt = myStore.find(key);
  auto newKvIt = newStore.find(key);

  // update with newer version
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.version++;
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(deltaPub.keyVals, newStore);
  }

  // update with lower version
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.version--;
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(deltaPub.keyVals.size(), 0);
  }

  // update with higher originatorId
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.originatorId = "node55";
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(deltaPub.keyVals, newStore);
  }

  // update with lower originatorId
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.originatorId = "node3";
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(deltaPub.keyVals.size(), 0);
  }

  // update larger value
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value = "dummyValueTest";
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(deltaPub.keyVals, newStore);
  }

  // update smaller value
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value = "dummy";
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(deltaPub.keyVals.size(), 0);
  }

  // update ttl only (new value.value() is none)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value = folly::none;
    newKvIt->second.ttl = 123;
    newKvIt->second.ttlVersion ++;
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    auto deltaKvIt = deltaPub.keyVals.find(key);
    // new ttl, ttlversion
    EXPECT_EQ(myKvIt->second.ttlVersion, newKvIt->second.ttlVersion);
    EXPECT_EQ(myKvIt->second.ttl, newKvIt->second.ttl);
    // old value tho
    EXPECT_EQ(myKvIt->second.value, oldKvIt->second.value);

    EXPECT_EQ(deltaKvIt->second.ttlVersion, newKvIt->second.ttlVersion);
    EXPECT_EQ(deltaKvIt->second.ttl, newKvIt->second.ttl);
    EXPECT_EQ(deltaKvIt->second.value.hasValue(), false);
  }

  // update ttl only (same version, originatorId and value,
  // but higher ttlVersion)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.ttl = 123;
    newKvIt->second.ttlVersion ++;
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(deltaPub.keyVals, newStore);
  }

  // invalid ttl update (higher ttlVersion, smaller value)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value = "dummy";
    newKvIt->second.ttlVersion ++;
    auto deltaPub = KvStore::mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(deltaPub.keyVals.size(), 0);
  }
}

//
// Test counter reporting
//
TEST(KvStore, MonitorReport) {
  fbzmq::Context context;
  CompactSerializer serializer;

  KvStore kvStore(
      context,
      "test",
      KvStoreLocalPubUrl{"inproc://local_pub"},
      KvStoreGlobalPubUrl{"inproc://global_pub"},
      KvStoreLocalCmdUrl{"inproc://local_cmd"},
      KvStoreGlobalCmdUrl{"inproc://global_cmd"},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      folly::none /* ip-tos */,
      fbzmq::util::genKeyPair(),
      std::chrono::seconds(1) /* Db Sync Interval */,
      std::chrono::seconds(1) /* Monitor Submit Interval */,
      std::unordered_map<std::string, thrift::PeerSpec>{});

  // create and bind socket to receive counters
  fbzmq::Socket<ZMQ_DEALER, fbzmq::ZMQ_SERVER> server(context);
  server.bind(fbzmq::SocketUrl{"inproc://monitor_submit"}).value();

  std::thread t([&kvStore]() {
    kvStore.run();
    LOG(INFO) << "KvStore stopped";
  });

  std::vector<fbzmq::PollItem> pollItems = {
      {reinterpret_cast<void*>(*server), 0, ZMQ_POLLIN, 0}};
  fbzmq::poll(pollItems);

  // wait till we receive counters
  server.recvOne()
      .value()
      .readThriftObj<fbzmq::thrift::MonitorRequest>(serializer)
      .value();
  LOG(INFO) << "Counters received, yo";
  kvStore.stop();
  t.join();
  LOG(INFO) << "KvStore thread finished";
}

/**
 * Test to verify PEER_ADD/PEER_DEL and verify that keys are synchronized
 * to the neighbor.
 * 1. Start store0, store1 and store2
 * 2. Add store1 and store2 to store2
 * 3. Advertise keys in store1 and store2
 * 4. Verify that they appear in store0
 * 5. Verify PEER_DEL API
 */
TEST_F(KvStoreTestFixture, PeerAddRemove) {
  const std::unordered_map<std::string, thrift::PeerSpec> emptyPeers;

  auto store0 = createKvStore("store0", emptyPeers);
  auto store1 = createKvStore("store1", emptyPeers);
  auto store2 = createKvStore("store2", emptyPeers);

  // Start stores in their respective threads.
  store0->run();
  store1->run();
  store2->run();

  // Add peers to store0
  EXPECT_TRUE(store0->addPeer(store1->nodeId, store1->getPeerSpec()));
  EXPECT_TRUE(store0->addPeer(store2->nodeId, store2->getPeerSpec()));

  // map of peers we expect and dump peers to expect the results.
  std::unordered_map<std::string, thrift::PeerSpec> expectedPeers = {
      {store1->nodeId, store1->getPeerSpec()},
      {store2->nodeId, store2->getPeerSpec()},
  };
  EXPECT_EQ(expectedPeers, store0->getPeers());

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
  EXPECT_TRUE(store1->setKey("test", thriftVal));
  // Update hash
  thriftVal.hash =
      generateHash(thriftVal.version, thriftVal.originatorId, thriftVal.value);

  // Receive publication from store0 for new key-update
  LOG(INFO) << "Waiting for message from store0 on pub socket ...";
  auto pub = store0->recvPublication(kTimeout);
  EXPECT_EQ(1, pub.keyVals.size());
  EXPECT_EQ(thriftVal, pub.keyVals["test"]);

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
  EXPECT_TRUE(store2->setKey("test", thriftVal));
  // Update hash
  thriftVal.hash =
      generateHash(thriftVal.version, thriftVal.originatorId, thriftVal.value);

  // Receive publication from store0 for new key-update
  LOG(INFO) << "Waiting for message from store0...";
  pub = store0->recvPublication(kTimeout);
  EXPECT_EQ(1, pub.keyVals.size());
  EXPECT_EQ(thriftVal, pub.keyVals["test"]);

  // Remove store1 and verify
  LOG(INFO) << "Deleting the peers for store0...";
  store0->delPeer("store1");
  expectedPeers.clear();
  expectedPeers[store2->nodeId] = store2->getPeerSpec();
  EXPECT_EQ(expectedPeers, store0->getPeers());

  // Remove store2 and verify that there are no more peers
  store0->delPeer("store2");
  EXPECT_EQ(emptyPeers, store0->getPeers());
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
 * Start single testable store, and make it sync with N other stores (only one
 * way connection). We only rely on pub-sub and sync logic on a single store to
 * do all the work.
 */
TEST_F(KvStoreTestFixture, BasicSync) {
  const std::string kOriginBase = "peer-store-";
  const unsigned int kNumStores = 16;
  const std::unordered_map<std::string, thrift::PeerSpec> emptyPeers;

  // Create and start peer-stores
  std::vector<KvStoreWrapper*> peerStores;
  for (unsigned int j = 0; j < kNumStores; ++j) {
    auto nodeId = getNodeId(kOriginBase, j);
    auto store = createKvStore(nodeId, emptyPeers);
    store->run();
    peerStores.push_back(store);
  }

  // Submit initial value set into all peerStores
  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
  LOG(INFO) << "Submitting initial key-value pairs into peer stores.";
  for (auto& store : peerStores) {
    auto key = folly::sformat("test-key-{}", store->nodeId);
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        1 /* version */,
        "gotham_city" /* originatorId */,
        folly::sformat("test-value-{}", store->nodeId),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);

    // Submit the key-value to store
    store->setKey(key, thriftVal);
    // Update hash
    thriftVal.hash = generateHash(
        thriftVal.version, thriftVal.originatorId, thriftVal.value);

    // Store this in expectedKeyVals
    expectedKeyVals[key] = thriftVal;
  }

  LOG(INFO) << "Starting store under test";

  // set up the store that we'll be testing
  auto myNodeId = getNodeId(kOriginBase, kNumStores);
  auto myStore = createKvStore(myNodeId, emptyPeers);
  myStore->run();

  // NOTE: It is important to add peers after starting our store to avoid
  // race condition where certain updates are lost over PUB/SUB channel
  for (auto& store : peerStores) {
    myStore->addPeer(store->nodeId, store->getPeerSpec());
  }

  // Wait for full-sync to complete. Full-sync is complete when all of our
  // neighbors receive all the keys and we must receive `kNumStores`
  // key-value updates from each store over PUB socket.
  LOG(INFO) << "Waiting for full sync to complete.";
  {
    std::unordered_set<std::string> keys;
    VLOG(3) << "Store " << myStore->nodeId << " received keys.";
    while (keys.size() < kNumStores) {
      auto publication = myStore->recvPublication(kTimeout);
      for (auto const& kv : publication.keyVals) {
        VLOG(3) << "\tkey: " << kv.first
                << ", value: " << kv.second.value.value();
        keys.insert(kv.first);
      }
    }
  }

  // Verify myStore database
  EXPECT_EQ(expectedKeyVals, myStore->dumpAll());

  //
  // Submit another range of values
  //
  LOG(INFO) << "Submitting the second round of key-values...";
  for (auto& store : peerStores) {
    auto key = folly::sformat("test-key-{}", store->nodeId);
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        2 /* version */,
        "gotham_city" /* originatorId */,
        folly::sformat("test-value-new-{}", store->nodeId),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);

    // Submit the key-value to store
    store->setKey(key, thriftVal);
    // Update hash
    thriftVal.hash = generateHash(
        thriftVal.version, thriftVal.originatorId, thriftVal.value);

    // Store this in expectedKeyVals
    expectedKeyVals[key] = thriftVal;
  }

  // Wait again for the full sync to complete. Full-sync is complete when all
  // of our neighbors receive all the keys and we must receive `kNumStores`
  // key-value updates from each store over PUB socket.
  LOG(INFO) << "waiting for another full sync to complete...";
  {
    std::unordered_set<std::string> keys;
    VLOG(3) << "Store " << myStore->nodeId << " received keys.";
    while (keys.size() < kNumStores) {
      auto publication = myStore->recvPublication(kTimeout);
      for (auto const& kv : publication.keyVals) {
        VLOG(3) << "\tkey: " << kv.first
                << ", value: " << kv.second.value.value();
        keys.insert(kv.first);
      }
    }
  }

  // Verify our database and all neighbor database
  EXPECT_EQ(expectedKeyVals, myStore->dumpAll());
}

/**
 * Make two stores race for the same key, and make sure tie-breaking is working
 * as expected. We do this by connecting N stores in a chain, and then
 * submitting different values at each end of the chain, with same version
 * numbers. We also try injecting lower version number to make sure it does not
 * overwrite anything.
 */
TEST_F(KvStoreTestFixture, TieBreaking) {
  using namespace folly::gen;

  const std::string kOriginBase = "store";
  const unsigned int kNumStores = 16;
  const std::string kKeyName = "test-key";

  //
  // Start the intermediate stores in string topology
  //
  LOG(INFO) << "Preparing and starting stores.";
  std::vector<KvStoreWrapper*> stores;
  const std::unordered_map<std::string, thrift::PeerSpec> emptyPeers;
  for (unsigned int i = 0; i < kNumStores; ++i) {
    auto nodeId = getNodeId(kOriginBase, i);
    auto store = createKvStore(nodeId, emptyPeers);
    LOG(INFO) << "Preparing store " << nodeId;
    store->run();
    stores.push_back(store);
  }

  // Add neighbors to the nodes.
  LOG(INFO) << "Adding neighbors in chain topology.";
  for (unsigned int i = 0; i < kNumStores; ++i) {
    auto& store = stores[i];
    if (i > 0) {
      auto& peerStore = stores[i - 1];
      EXPECT_TRUE(store->addPeer(peerStore->nodeId, peerStore->getPeerSpec()));
    }
    if (i < kNumStores - 1) {
      auto& peerStore = stores[i + 1];
      EXPECT_TRUE(store->addPeer(peerStore->nodeId, peerStore->getPeerSpec()));
    }
  }

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
  EXPECT_TRUE(stores[0]->setKey(kKeyName, thriftValFirst));
  // Update hash
  thriftValFirst.hash = generateHash(
      thriftValFirst.version,
      thriftValFirst.originatorId,
      thriftValFirst.value);

  // set a key from the store on the other end of the chain
  thrift::Value thriftValLast(
      apache::thrift::FRAGILE,
      10 /* version */,
      "2" /* originatorId */,
      "test-value-2",
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
  EXPECT_TRUE(stores[kNumStores - 1]->setKey(kKeyName, thriftValLast));
  // Update hash
  thriftValLast.hash = generateHash(
      thriftValLast.version, thriftValLast.originatorId, thriftValLast.value);

  //
  // We expect test-value-2 because "2" > "1" in tie-breaking
  //
  LOG(INFO) << "Pulling values from every store";

  // We have to wait until we see two updates on the first node and verify them.
  {
    auto pub1 = stores[0]->recvPublication(kTimeout);
    auto pub2 = stores[0]->recvPublication(kTimeout);
    ASSERT_EQ(1, pub1.keyVals.count(kKeyName));
    ASSERT_EQ(1, pub2.keyVals.count(kKeyName));
    EXPECT_EQ(thriftValFirst, pub1.keyVals.at(kKeyName));
    EXPECT_EQ(thriftValLast, pub2.keyVals.at(kKeyName));
  }

  // python style baby!
  for (auto& store : stores) {
    LOG(INFO) << "Pulling state from " << store->nodeId;
    auto maybeThriftVal = store->getKey(kKeyName);
    ASSERT_TRUE(maybeThriftVal.hasValue());
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
    EXPECT_TRUE(stores[0]->setKey(kKeyName, thriftVal));
    // Update hash
    thriftVal.hash = generateHash(
        thriftVal.version, thriftVal.originatorId, thriftVal.value);

    // Make sure the old value still exists
    EXPECT_EQ(thriftValLast, stores[0]->getKey(kKeyName));
  }
}

TEST_F(KvStoreTestFixture, DumpPrefix) {
  const std::string kOriginBase = "peer-store-";
  const unsigned int kNumStores = 16;
  const std::unordered_map<std::string, thrift::PeerSpec> emptyPeers;

  // Create and start peer-stores
  std::vector<KvStoreWrapper*> peerStores;
  for (unsigned int j = 0; j < kNumStores; ++j) {
    auto nodeId = getNodeId(kOriginBase, j);
    auto store = createKvStore(nodeId, emptyPeers);
    store->run();
    peerStores.push_back(store);
  }

  // Submit initial value set into all peerStores
  std::unordered_map<std::string, thrift::Value> expectedKeyVals;
  LOG(INFO) << "Submitting initial key-value pairs into peer stores.";
  int i = 0;
  for (auto& store : peerStores) {
    auto key = folly::sformat("{}-test-key-{}", i % 2, store->nodeId);
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
        1 /* version */,
        "gotham_city" /* originatorId */,
        folly::sformat("test-value-{}", store->nodeId),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);

    // Submit the key-value to store
    store->setKey(key, thriftVal);
    // Update hash
    thriftVal.hash = generateHash(
        thriftVal.version, thriftVal.originatorId, thriftVal.value);

    // Store this in expectedKeyVals
    if (i % 2 == 0) {
      expectedKeyVals[key] = thriftVal;
    }
    ++i;
  }

  LOG(INFO) << "Starting store under test";

  // set up the store that we'll be testing
  auto myNodeId = getNodeId(kOriginBase, kNumStores);
  auto myStore = createKvStore(myNodeId, emptyPeers);
  myStore->run();

  // NOTE: It is important to add peers after starting our store to avoid
  // race condition where certain updates are lost over PUB/SUB channel
  for (auto& store : peerStores) {
    myStore->addPeer(store->nodeId, store->getPeerSpec());
  }

  // Wait for full-sync to complete. Full-sync is complete when all of our
  // neighbors receive all the keys and we must receive `kNumStores`
  // key-value updates from each store over PUB socket.
  LOG(INFO) << "Waiting for full sync to complete.";
  {
    std::unordered_set<std::string> keys;
    VLOG(3) << "Store " << myStore->nodeId << " received keys.";
    while (keys.size() < kNumStores) {
      auto publication = myStore->recvPublication(kTimeout);
      for (auto const& kv : publication.keyVals) {
        VLOG(3) << "\tkey: " << kv.first
                << ", value: " << kv.second.value.value();
        keys.insert(kv.first);
      }
    }
  }

  // Verify myStore database. we only want keys with "0" prefix
  EXPECT_EQ(expectedKeyVals, myStore->dumpAll("0"));
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return 1;
  }

  // Run the tests
  return RUN_ALL_TESTS();
}
