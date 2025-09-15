/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <gtest/gtest.h>

#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <range/v3/view/enumerate.hpp>

using namespace openr;

namespace {

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
    for (auto& store : stores_) {
      store->stop();
    }
  }

  /**
   * Helper function to create KvStoreWrapper. The underlying stores will be
   * stopped as well as destroyed automatically when test exits.
   * Retured raw pointer of an object will be freed as well.
   */
  void
  initKvStore(std::string nodeId) {
    // create KvStoreConfig
    thrift::KvStoreConfig kvStoreConfig;
    kvStoreConfig.node_name() = nodeId;
    const std::unordered_set<std::string> areaIds{kTestingAreaName};

    // start kvstore
    stores_.emplace_back(
        std::make_unique<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            areaIds, kvStoreConfig));
    stores_.back()->run();
  }

  /**
   * Utility function to create a nodeId/originId based on it's index and
   * prefix
   */
  std::string
  buildNodeId(const std::string& prefix, const int index) const {
    return fmt::format("{}{}", prefix, index);
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
    thrift::KeyVals expectedGlobalKeyVals;

    // For each `key`, generate a `value` and submit it to the first
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

      // Submit a bunch of keys/values to the first store.
      for (unsigned int j = 0; j < kNumKeys; ++j) {
        // Create new key-value pair.
        const std::string key = fmt::format("key-{}-{}", i, j);
        const auto value = fmt::format("value-{}-{}", i, j);
        auto thriftVal = createThriftValue(
            version,
            "pluto" /* originatorId */,
            value,
            ttl /* ttl */,
            0 /* ttl version */,
            0 /* hash*/);

        auto& store = stores_[0];
        EXPECT_GE(stores_.size(), 0);
        EXPECT_TRUE(store->setKey(kTestingAreaName, key, thriftVal));
        const auto dump = store->dumpAll(kTestingAreaName);
        // TODO: This is a hack! Pause thread for a bit to allow key to be
        // retrieved. T102358658 is task that has been created to address this
        // issue.
        std::this_thread::sleep_for(500ms);
        EXPECT_FALSE(dump.empty());
        // Verify 1. hash is updated in KvStore
        // 2. dumpHashes request returns key values as expected
        const auto hashDump = store->dumpHashes(kTestingAreaName);
        for (const auto& [key, value] : dump) {
          EXPECT_TRUE(value.hash().value() != 0);
          if (!hashDump.contains(key)) {
            continue;
          }
          EXPECT_EQ(value.hash().value(), hashDump.at(key).hash().value());
        }

        // Update hash
        thriftVal.hash() = generateHash(
            *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());
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
      if (!checkTtl) {
        LOG(INFO) << "Expecting publications from stores. Iteration# " << i;
        for (unsigned int j = 0; j < kNumStores; ++j) {
          auto& store = stores_[j];

          // Receive all expected number of updates.
          std::map<std::string, thrift::Value> receivedKeyVals;
          while (receivedKeyVals.size() < kNumKeys) {
            auto publication = store->recvPublication();
            for (auto const& kv : *publication.keyVals()) {
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
                val.value() ? *val.value() : "",
                *val.version());
          }
        } // for `j < kNumStores`

        // Verify the global key-value database from all nodes
        for (auto& store : stores_) {
          const auto dump = store->dumpAll(kTestingAreaName);
          const auto hashDump = store->dumpHashes(kTestingAreaName);
          EXPECT_EQ(expectedGlobalKeyVals, dump);
          for (const auto& [key, value] : dump) {
            EXPECT_TRUE(value.hash().value() != 0);
            EXPECT_TRUE(hashDump.contains(key));
            EXPECT_EQ(value.hash().value(), hashDump.at(key).hash().value());
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
            if (!keyVals.empty()) {
              VLOG(2) << store->getNodeId() << " still has " << keyVals.size()
                      << " keys remaining";
              for (auto& [key, val] : keyVals) {
                VLOG(2) << "  " << key << ", ttl: " << *val.ttl();
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

  // Internal stores
  std::vector<
      std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>
      stores_{};

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
 * Create a grid topology in a 3 by 3 grid. Each node is connected to all
 * adjacent nodes.
 *
 * 0   1   2
 * 3   4   5
 * 6   7   8
 */
TEST_P(KvStoreTestTtlFixture, Graph) {
  // how many stores/syncers to create
  const unsigned int kNumStores = 9;

  // using set to avoid duplicate neighbors
  std::vector<std::unordered_set<int>> adjacencyList = {
      {3, 1},
      {0, 4, 2},
      {1, 5},
      {0, 4, 6},
      {1, 3, 5, 7},
      {2, 4, 8},
      {3, 7},
      {6, 4, 8},
      {7, 5},
  };

  VLOG(1) << "Adjacency list: ";
  for (const auto&& [index, set] : adjacencyList | ranges::views::enumerate) {
    VLOG(1) << index << ": " << fmt::format("{}", folly::join(", ", set));
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
