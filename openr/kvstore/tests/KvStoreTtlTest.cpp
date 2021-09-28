/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/zmq/Zmq.h>
#include <folly/init/Init.h>
#include <gtest/gtest.h>

#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

namespace {

// the size of the value string
const uint32_t kValueStrSize = 64;

// TTL in ms
const int64_t kTtlMs = 1000;

class KvStoreTestTtlFixture : public ::testing::TestWithParam<bool> {
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
  void
  initKvStore(std::string nodeId) {
    // create config
    auto tConfig = getBasicOpenrConfig(nodeId, "domain");
    auto config = std::make_shared<Config>(tConfig);

    // start kvstore
    stores_.emplace_back(std::make_unique<KvStoreWrapper>(context_, config));
    stores_.back()->run();
  }

  /**
   * Utility function to create a nodeId/originId based on it's index and
   * prefix
   */
  std::string
  buildNodeId(const std::string& prefix, const int index) const {
    return folly::sformat("{}{}", prefix, index);
  }

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
      const std::vector<std::unordered_set<int>>& adjacencyList,
      const std::string& kOriginBase,
      const unsigned int kNumStores = 16,
      const unsigned int kNumIter = 17,
      const unsigned int kNumKeys = 8) {
    // Some sanity checks
    CHECK_EQ(kNumStores, adjacencyList.size()) << "Incomplete adjacencies.";

    // Create and start stores
    VLOG(1) << "Creating and starting stores ...";
    for (unsigned int i = 0; i < kNumStores; ++i) {
      const auto nodeId = buildNodeId(kOriginBase, i);
      LOG(INFO) << "Creating store " << nodeId;
      initKvStore(nodeId);
    }

    // Add neighbors
    LOG(INFO) << "Adding neighbors ...";
    for (unsigned int i = 0; i < kNumStores; ++i) {
      const auto nodeId = buildNodeId(kOriginBase, i);
      for (const auto& j : adjacencyList[i]) {
        const auto neighborId = buildNodeId(kOriginBase, j);
        LOG(INFO) << "Adding neighbor " << neighborId << " to store " << nodeId;
        EXPECT_TRUE(stores_[i]->addPeer(
            kTestingAreaName,
            stores_[j]->getNodeId(),
            stores_[j]->getPeerSpec()));
      }
    }

    // Expected global Key-Value database
    std::unordered_map<std::string, thrift::Value> expectedGlobalKeyVals;

    // For each `key`, generate random `value` and submit it to any random
    // store. After submission of all keys, make sure all KvStore are in
    // consistent state. We perform the same thing `kNumIter` times.
    int64_t version = 1;
    CHECK_GT(kNumIter, kNumStores);
    for (unsigned int i = 0; i < kNumIter; ++i, ++version) {
      LOG(INFO) << "KeyValue Synchronization Test. Iteration# " << i;
      auto startTime = std::chrono::steady_clock::now();

      // we'll save expected keys and values which will be updated in all
      // KvStores in this iteration.
      std::map<std::string, thrift::Value> expectedKeyVals;

      // Submit a bunch of keys/values to a random store.
      for (unsigned int j = 0; j < kNumKeys; ++j) {
        // Create new key-value pair.
        const std::string key = folly::sformat("key-{}-{}", i, j);
        const auto value = genRandomStr(kValueStrSize);
        auto thriftVal = createThriftValue(
            version,
            "pluto" /* originatorId */,
            value,
            ttl /* ttl */,
            0 /* ttl version */,
            0 /* hash*/);

        // Submit this to a random store.
        auto& store = stores_[folly::Random::rand32() % stores_.size()];
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
          auto& store = stores_[j];

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
          for (auto const& [key, val] : receivedKeyVals) {
            VLOG(4) << fmt::format(
                "\tkey: {},  value: {}, version: {}",
                key,
                val.value_ref() ? *val.value_ref() : "",
                *val.version_ref());
          }
        } // for `j < kNumStores`

        // Verify the global key-value database from all nodes
        for (auto& store : stores_) {
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
          VLOG(2) << "Checking for empty stores. Iter#" << ++iter;
          bool allStoreEmpty = true;
          for (auto& store : stores_) {
            auto keyVals = store->dumpAll(kTestingAreaName);
            if (not keyVals.empty()) {
              VLOG(2) << store->getNodeId() << " still has " << keyVals.size()
                      << " keys remaining";
              for (auto& [key, val] : keyVals) {
                VLOG(2) << "  " << key << ", ttl: " << *val.ttl_ref();
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
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime)
                .count();
        LOG(INFO) << "ttl " << ttl << " vs elapsedTime " << elapsedTime;
        EXPECT_LE(
            ttl - Constants::kTtlDecrement.count() - errorMargin, elapsedTime);
      }
    } // for `i < kNumIter`
  }

  // Public member variables
  fbzmq::Context context_;

  // Internal stores
  std::vector<std::unique_ptr<KvStoreWrapper>> stores_{};

  // enable TTL check or not
  const bool checkTtl = GetParam();
  const int64_t ttl = checkTtl ? kTtlMs : Constants::kTtlInfinity;
};

INSTANTIATE_TEST_CASE_P(
    KvStoreTestInstance, KvStoreTestTtlFixture, ::testing::Bool());
} // namespace

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
