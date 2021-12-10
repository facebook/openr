/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/zmq/Zmq.h>
#include <folly/Benchmark.h>
#include <folly/Format.h>
#include <folly/Random.h>
#include <folly/init/Init.h>

#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/monitor/SystemMetrics.h>
#include <openr/tests/utils/Utils.h>

#define BENCHMARK_COUNTERS_NAME_PARAM(name, counters, param_name, ...) \
  BENCHMARK_IMPL_COUNTERS(                                             \
      FB_CONCATENATE(name, FB_CONCATENATE(_, param_name)),             \
      FOLLY_PP_STRINGIZE(name) "(" FOLLY_PP_STRINGIZE(param_name) ")", \
      counters,                                                        \
      iters,                                                           \
      unsigned,                                                        \
      iters) {                                                         \
    name(counters, iters, ##__VA_ARGS__);                              \
  }

namespace {

// The byte size of a key
const int kSizeOfKey = 32;
// The byte size of a value
const int kSizeOfValue = 1024;

} // namespace

namespace openr {

/**
 * Fixture for abstracting out common functionality for unittests.
 */
class KvStoreTestFixture {
 public:
  KvStoreTestFixture() {
    // nothing to do
  }

  ~KvStoreTestFixture() {
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
  createKvStore(const std::string& nodeId) {
    config_ = std::make_shared<Config>(getBasicOpenrConfig(nodeId));
    stores_.emplace_back(std::make_unique<KvStoreWrapper>(context_, config_));
    return stores_.back().get();
  }

 private:
  // Public member variables
  fbzmq::Context context_;

  // Internal stores
  std::shared_ptr<Config> config_;
  std::vector<std::unique_ptr<KvStoreWrapper>> stores_{};
};

/**
 * Set keys into kvStore and make sure they appear in kvStore
 */
void
floodingUpdate(
    const uint32_t numOfUpdateKeys,
    uint64_t& version,
    const std::vector<std::string>& keys,
    KvStoreWrapper* kvStore) {
  auto suspender = folly::BenchmarkSuspender();
  // Set keys into kvStore
  std::vector<std::pair<std::string, thrift::Value>> keyVals;
  keyVals.reserve(numOfUpdateKeys);
  for (uint32_t idx = 0; idx < numOfUpdateKeys; idx++) {
    auto key = keys[idx];
    auto value = genRandomStr(kSizeOfValue);
    auto thriftVal = createThriftValue(
        version /* version */,
        "kvStore" /* originatorId */,
        value /* value */,
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);

    // Update hash
    thriftVal.hash_ref() = generateHash(
        *thriftVal.version_ref(),
        *thriftVal.originatorId_ref(),
        thriftVal.value_ref());
    auto keyVal = std::make_pair(key, thriftVal);
    keyVals.emplace_back(keyVal);
  }
  suspender.dismiss(); // Start measuring benchmark time
  kvStore->setKeys(kTestingAreaName, keyVals);
  version++;

  // Receive publication from kvStore for new key-update
  auto pub = kvStore->recvPublication();
  CHECK_EQ(numOfUpdateKeys, pub.keyVals_ref()->size());
}

/**
 * Benchmark for mergeKeyValues():
 * 1. Generate (key, value) pairs, and put them into kvStore
 * 2. Merge update with kvStore
 */
static void
BM_KvStoreMergeKeyValues(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfKeysInStore,
    size_t numOfUpdateKeys) {
  CHECK_LE(numOfUpdateKeys, numOfKeysInStore);
  auto suspender = folly::BenchmarkSuspender();
  // Add boolean to control profiling memory for the 1st iteration
  SystemMetrics sysMetrics;
  bool record = true;

  uint64_t version = 1;
  for (uint32_t i = 0; i < iters; i++) {
    std::unordered_map<std::string, thrift::Value> kvStore;
    std::unordered_map<std::string, thrift::Value> update;
    // Insert (key, value)s into kvStore

    for (uint32_t idx = 0; idx < numOfKeysInStore; idx++) {
      auto key = genRandomStr(kSizeOfKey);
      auto value = genRandomStr(kSizeOfValue);
      auto thriftValue = createThriftValue(
          version, /* version */
          "kvStore", /* node id */
          value,
          3600, /* ttl */
          0 /* ttl version */,
          0 /* hash */);

      kvStore.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(key),
          std::forward_as_tuple(thriftValue));

      if (idx < numOfUpdateKeys) {
        auto updateThriftValue = createThriftValue(
            version + 1, /* version */
            "kvStore", /* node id */
            value,
            3600, /* ttl */
            0 /* ttl version */,
            0 /* hash */);
        update.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(updateThriftValue));
      }
    }

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }
    suspender.dismiss(); // Start measuring benchmark time
    // Merge update with kvStore
    mergeKeyValues(kvStore, update);
    // Stop measuring benchmark time
    suspender.rehire();
    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_after_operation(MB)"] = mem.value() / 1024 / 1024;
      }
      record = false;
    }
  }
}

/**
 * Benchmark for a full dump:
 * 1. Start kvStore
 * 2. Set (key, value)s into kvStore
 * 3. Benchmark the time for dumpAll()
 */
static void
BM_KvStoreDumpAll(
    folly::UserCounters& counters, uint32_t iters, size_t numOfKeysInStore) {
  auto suspender = folly::BenchmarkSuspender();
  // Add boolean to control profiling memory for the 1st iteration
  SystemMetrics sysMetrics;
  bool record = true;
  for (uint32_t i = 0; i < iters; i++) {
    auto kvStoreTestFixture = std::make_unique<KvStoreTestFixture>();
    auto kvStore = kvStoreTestFixture->createKvStore("kvStore");
    kvStore->run();

    for (uint32_t idx = 0; idx < numOfKeysInStore; idx++) {
      auto key = genRandomStr(kSizeOfKey);
      auto value = genRandomStr(kSizeOfValue);
      auto thriftVal = createThriftValue(
          1 /* version */,
          "kvStore" /* originatorId */,
          value /* value */,
          Constants::kTtlInfinity /* ttl */,
          0 /* ttl version */,
          0 /* hash */);
      thriftVal.hash_ref() = generateHash(
          *thriftVal.version_ref(),
          *thriftVal.originatorId_ref(),
          thriftVal.value_ref());

      // Adding key to kvStore
      kvStore->setKey(kTestingAreaName, key, thriftVal);
    }

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }

    suspender.dismiss(); // Start measuring benchmark time

    kvStore->dumpAll(kTestingAreaName);
    // Stop measuring benchmark time
    suspender.rehire();

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_after_operation(MB)"] = mem.value() / 1024 / 1024;
      }
      record = false;
    }
  }
}

/**
 * Benchmark for synchronizing update from a peer
 * 1. Start kvStore
 * 2. Advertise keys in kvStore and wait until they appear in kvStore
 */
static void
BM_KvStoreFloodingUpdate(
    folly::UserCounters& counters, uint32_t iters, size_t numOfUpdateKeys) {
  auto suspender = folly::BenchmarkSuspender();
  // Add boolean to control profiling memory for the 1st iteration
  SystemMetrics sysMetrics;
  bool record = true;
  for (uint32_t i = 0; i < iters; i++) {
    auto kvStoreTestFixture = std::make_unique<KvStoreTestFixture>();
    auto kvStore = kvStoreTestFixture->createKvStore("kvStore");

    // Start stores in their respective threads.
    kvStore->run();

    // Generate random keys beforehand for updating
    std::vector<std::string> keys;
    keys.reserve(numOfUpdateKeys);
    for (uint32_t idx = 0; idx < numOfUpdateKeys; idx++) {
      keys.emplace_back(genRandomStr(kSizeOfKey));
    }

    // Version starts with 1
    uint64_t version = 1;

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }

    suspender.dismiss(); // Start measuring benchmark time

    floodingUpdate(numOfUpdateKeys, version, keys, kvStore);
    // Stop measuring benchmark time
    suspender.rehire();

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_after_operation(MB)"] = mem.value() / 1024 / 1024;
      }
      record = false;
    }
  }
}

// The first integer parameter is number of keyVals already in store
// The second integer parameter is the number of keyVals for update
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 10_10, 10, 10);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 100_10, 100, 10);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 1000_10, 1000, 10);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 10000_10, 10000, 10);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 10000_100, 10000, 100);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 10000_1000, 10000, 1000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 10000_10000, 10000, 10000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 100000_1, 100000, 1);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 100000_10, 100000, 10);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 100000_100, 100000, 100);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 100000_1000, 100000, 1000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 100000_10000, 100000, 10000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 100000_100000, 100000, 100000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 1000000_1, 1000000, 1);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 1000000_100, 1000000, 100);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 1000000_10000, 1000000, 10000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 1000000_100000, 1000000, 100000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreMergeKeyValues, counters, 1000000_1000000, 1000000, 1000000);

// The parameter is number of keyVals already in store
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 10, 10);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 100, 100);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 1000, 1000);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 10000, 10000);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 100000, 100000);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 1000000, 1000000);

// The parameter is number of keyVals for update
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreFloodingUpdate, counters, 10, 10);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreFloodingUpdate, counters, 100, 10);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreFloodingUpdate, counters, 1000, 1000);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreFloodingUpdate, counters, 10000, 10000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreFloodingUpdate, counters, 100000, 100000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreFloodingUpdate, counters, 1000000, 1000000);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
