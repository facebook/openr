/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>

#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
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

FOLLY_INIT_LOGGING_CONFIG(
    ".=WARNING"
    ";default:async=true,sync_level=WARNING");

namespace openr {
namespace {
const std::string kMemoryBeforeOperationMB = "memory_before_operation(MB)";
const std::string kMemoryAfterOperationMB = "memory_after_operation(MB)";
const std::string kNodeId = "kvStore";
} // namespace

/**
 * Harness for abstracting out common functionality for unittests.
 */
class KvStoreHarness {
 public:
  KvStoreHarness() {
    // nothing to do
  }

  ~KvStoreHarness() {
    clear();
  }

  // explicit cleanup
  void
  clear() {
    stores_.clear();
  }

  /**
   * Helper function to create KvStoreWrapper. The underlying stores will be
   * stopped as well as destroyed automatically when test exits.
   * Retured raw pointer of an object will be freed as well.
   */
  KvStoreWrapper<thrift::KvStoreServiceAsyncClient>*
  createKvStore(const std::string& nodeId) {
    // create KvStoreConfig
    thrift::KvStoreConfig kvStoreConfig;
    kvStoreConfig.node_name() = nodeId;
    const folly::F14FastSet<std::string> areaIds{kTestingAreaName.t};

    stores_.emplace_back(
        std::make_unique<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            areaIds, kvStoreConfig));
    return stores_.back().get();
  }

  void
  recordMemory(folly::UserCounters& counters, const std::string& counterKey) {
    auto mem = sysMetrics_.getVirtualMemBytes();
    if (mem.has_value()) {
      counters[counterKey] = mem.value() / 1024 / 1024;
    }
  }

  uint64_t
  getVersion() {
    return version_;
  }

  void
  incrementVersion() {
    version_++;
  }

  void
  genKeyVals(
      std::vector<std::pair<std::string, thrift::Value>>& keyVals, size_t n) {
    keyVals.clear();
    keyVals.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
      auto key = genRandomStr(kSizeOfKey);
      auto value = genRandomStr(kSizeOfValue);
      auto thriftVal = createThriftValue(
          getVersion() /* version */,
          kNodeId /* originatorId */,
          value /* value */);
      // Update hash
      thriftVal.hash() = generateHash(
          *thriftVal.version(), *thriftVal.originatorId(), thriftVal.value());
      keyVals.emplace_back(key, thriftVal);
    }
  }

 private:
  // Internal stores
  std::vector<
      std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>
      stores_{};
  SystemMetrics sysMetrics_{};
  uint64_t version_{1}; // Version starts with 1
};

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
  auto kvStoreHarness = std::make_unique<KvStoreHarness>();

  for (uint32_t i = 0; i < iters; i++) {
    thrift::KeyVals kvStore;
    thrift::KeyVals update;

    // Insert (key, value)s into kvStore
    for (uint32_t idx = 0; idx < numOfKeysInStore; idx++) {
      auto key = genRandomStr(kSizeOfKey);
      auto value = genRandomStr(kSizeOfValue);
      auto thriftValue = createThriftValue(
          kvStoreHarness->getVersion(), /* version */
          kNodeId, /* originatorId */
          value /* value */);

      kvStore.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(key),
          std::forward_as_tuple(thriftValue));

      if (idx < numOfUpdateKeys) {
        auto updateThriftValue = createThriftValue(
            kvStoreHarness->getVersion() + 1, /* version */
            kNodeId, /* originatorId */
            value /* value */);
        update.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(updateThriftValue));
      }
    }

    // collect mem usage ONLY with the first iteration for profiling purpose
    if (i == 0) {
      kvStoreHarness->recordMemory(counters, kMemoryBeforeOperationMB);
    }

    suspender.dismiss(); // Start measuring benchmark time
    mergeKeyValues(kvStore, update);
    suspender.rehire(); // Stop measuring benchmark time

    if (i == 0) {
      kvStoreHarness->recordMemory(counters, kMemoryAfterOperationMB);
    }
  }

  kvStoreHarness->clear();
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
  auto kvStoreHarness = std::make_unique<KvStoreHarness>();
  KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* kvStore{
      nullptr};

  kvStore = kvStoreHarness->createKvStore(kNodeId);
  kvStore->run();

  for (uint32_t i = 0; i < iters; i++) {
    std::vector<std::pair<std::string, thrift::Value>> keyVals;

    if (i == 0) {
      // collect mem usage ONLY with the first iteration for profiling purpose
      kvStoreHarness->recordMemory(counters, kMemoryBeforeOperationMB);
    }

    // Do NOT account key generation + injection as part benchmark timing
    kvStoreHarness->genKeyVals(keyVals, numOfKeysInStore);
    for (const auto& [key, thriftVal] : keyVals) {
      kvStore->setKey(kTestingAreaName, key, thriftVal);
    }

    suspender.dismiss(); // Start measuring benchmark time
    kvStore->dumpAll(kTestingAreaName);
    suspender.rehire(); // Stop measuring benchmark time

    if (i == 0) {
      kvStoreHarness->recordMemory(counters, kMemoryAfterOperationMB);
    }
  }

  kvStoreHarness->clear();
}

/**
 * Benchmark for synchronizing update from a peer
 * 1. Start kvStore
 * 2. Advertise keys in kvStore and wait until they appear in kvStore
 */
static void
BM_KvStoreValueUpdate(
    folly::UserCounters& counters, uint32_t iters, size_t numOfUpdateKeys) {
  auto kvStoreHarness = std::make_unique<KvStoreHarness>();
  auto suspender = folly::BenchmarkSuspender();
  KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* kvStore{
      nullptr};
  std::vector<std::pair<std::string, thrift::Value>> keyVals;

  kvStore = kvStoreHarness->createKvStore(kNodeId);
  XCHECK_NE(kvStore, nullptr);
  kvStore->run();

  for (size_t i = 0; i < iters; i++) {
    kvStoreHarness->genKeyVals(keyVals, numOfUpdateKeys);
    if (i == 0) {
      kvStoreHarness->recordMemory(counters, kMemoryBeforeOperationMB);
    }

    /*
     * Core function to run benchmarking against
     */
    suspender.dismiss(); // Start measuring benchmark time
    kvStore->setKeys(kTestingAreaName, keyVals);
    // Receive publication from kvStore for new key-update
    auto pub = kvStore->recvPublication();
    CHECK_EQ(numOfUpdateKeys, pub.keyVals()->size());
    suspender.rehire(); // Stop measuring benchmark time

    if (i == 0) {
      kvStoreHarness->recordMemory(counters, kMemoryAfterOperationMB);
    }
    kvStoreHarness->incrementVersion();
  }

  kvStoreHarness->clear();
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

BENCHMARK_DRAW_LINE();

// The parameter is number of keyVals already in store
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 10, 10);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 100, 100);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 1000, 1000);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 10000, 10000);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 100000, 100000);
BENCHMARK_COUNTERS_NAME_PARAM(BM_KvStoreDumpAll, counters, 1000000, 1000000);

BENCHMARK_DRAW_LINE();

BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreValueUpdate, counters, 10_keys, /* numOfUpdateKeys = */ 10);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreValueUpdate, counters, 100_keys, /* numOfUpdateKeys = */ 100);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreValueUpdate,
    counters,
    1000_keys,
    /* numOfUpdateKeys = */ 1000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreValueUpdate,
    counters,
    10000_keys,
    /* numOfUpdateKeys = */ 10000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreValueUpdate,
    counters,
    100000_keys,
    /* numOfUpdateKeys = */ 100000);
BENCHMARK_COUNTERS_NAME_PARAM(
    BM_KvStoreValueUpdate,
    counters,
    1000000_keys,
    /* numOfUpdateKeys = */ 1000000);
} // namespace openr

int
main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
