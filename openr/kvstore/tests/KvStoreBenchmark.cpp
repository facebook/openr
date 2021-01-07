/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>
#include <cstdlib>
#include <unordered_set>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Random.h>
#include <folly/init/Init.h>

#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreWrapper.h>

namespace {

// interval for periodic syncs
const std::chrono::seconds kDbSyncInterval(10000);

// The byte size of a key
const int kSizeOfKey = 32;
// The byte size of a value
const int kSizeOfValue = 1024;

/**
 * Produce a random string of given length - for value generation
 */
std::string
genRandomStr(const int len) {
  std::string s;
  s.resize(len);

  static const std::string alphanum =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  for (int i = 0; i < len; ++i) {
    s[i] = alphanum[folly::Random::rand32() % alphanum.size()];
  }
  return s;
}
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
    auto tConfig = getBasicOpenrConfig(nodeId);
    tConfig.kvstore_config_ref()->sync_interval_s_ref() =
        kDbSyncInterval.count();
    config_ = std::make_shared<Config>(tConfig);
    auto ptr = std::make_unique<KvStoreWrapper>(context, config_);
    stores_.emplace_back(std::move(ptr));
    return stores_.back().get();
  }

 private:
  // Public member variables
  fbzmq::Context context;

  // Internal stores
  std::shared_ptr<Config> config_;
  std::vector<std::unique_ptr<KvStoreWrapper>> stores_{};
};

/**
 * Merge update with kvStore:
 * 1. Randomly choose #numOfUpdateKeys keys from kvStore
 * 2. Randomly choose a newValue for each key
 * 3. Insert (key, newValue)s into update
 * 4. Merge update with kvStore
 */
void
updateKvStore(
    const uint32_t numOfUpdateKeys,
    uint64_t& version,
    std::unordered_map<std::string, thrift::Value>& kvStore) {
  auto suspender = folly::BenchmarkSuspender();
  std::unordered_map<std::string, thrift::Value> update;
  // Randomly choose the start index of the keys to be updated
  auto offsetIdx = folly::Random::rand32() % kvStore.size();
  offsetIdx = offsetIdx > kvStore.size() - numOfUpdateKeys
      ? kvStore.size() - numOfUpdateKeys
      : offsetIdx;

  for (uint32_t idx = offsetIdx; idx < offsetIdx + numOfUpdateKeys; idx++) {
    auto kvIt = kvStore.begin();
    std::advance(kvIt, idx);
    auto key = kvIt->first;
    auto newValue = genRandomStr(kSizeOfValue);
    thrift::Value thriftValue(
        apache::thrift::FRAGILE,
        version, /* version */
        "kvStore", /* node id */
        newValue,
        3600, /* ttl */
        0 /* ttl version */,
        0 /* hash */);

    update.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(thriftValue));
  }
  version++;
  suspender.dismiss(); // Start measuring benchmark time

  // Merge update with kvStore
  KvStore::mergeKeyValues(kvStore, update);
}

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
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
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
    uint32_t iters, uint32_t numOfKeysInStore, size_t numOfUpdateKeys) {
  CHECK_LE(numOfUpdateKeys, numOfKeysInStore);
  auto suspender = folly::BenchmarkSuspender();
  std::unordered_map<std::string, thrift::Value> kvStore;

  // Insert (key, value)s into kvStore
  uint64_t version = 1;
  for (uint32_t idx = 0; idx < numOfKeysInStore; idx++) {
    auto key = genRandomStr(kSizeOfKey);
    auto value = genRandomStr(kSizeOfValue);
    thrift::Value thriftValue(
        apache::thrift::FRAGILE,
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
  }

  // Version starts with 2 since keys aleady in kvStore have a version of 1
  version++;
  suspender.dismiss(); // Start measuring benchmark time
  for (uint32_t i = 0; i < iters; i++) {
    updateKvStore(numOfUpdateKeys, version, kvStore);
  }
}

/**
 * Benchmark for a full dump:
 * 1. Start kvStore
 * 2. Set (key, value)s into kvStore
 * 3. Benchmark the time for dumpAll()
 */
static void
BM_KvStoreDumpAll(uint32_t iters, size_t numOfKeysInStore) {
  auto suspender = folly::BenchmarkSuspender();
  auto kvStoreTestFixture = std::make_unique<KvStoreTestFixture>();
  auto kvStore = kvStoreTestFixture->createKvStore("kvStore");
  kvStore->run();

  for (uint32_t idx = 0; idx < numOfKeysInStore; idx++) {
    auto key = genRandomStr(kSizeOfKey);
    auto value = genRandomStr(kSizeOfValue);
    thrift::Value thriftVal(
        apache::thrift::FRAGILE,
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

  suspender.dismiss(); // Start measuring benchmark time
  for (uint32_t i = 0; i < iters; i++) {
    kvStore->dumpAll(kTestingAreaName);
  }
}

/**
 * Benchmark for synchronizing update from a peer
 * 1. Start kvStore
 * 2. Advertise keys in kvStore and wait until they appear in kvStore
 */
static void
BM_KvStoreFloodingUpdate(uint32_t iters, size_t numOfUpdateKeys) {
  auto suspender = folly::BenchmarkSuspender();
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
  suspender.dismiss(); // Start measuring benchmark time

  for (uint32_t i = 0; i < iters; i++) {
    floodingUpdate(numOfUpdateKeys, version, keys, kvStore);
  }
}

// The first integer parameter is number of keyVals already in store
// The second integer parameter is the number of keyVals for update
BENCHMARK_NAMED_PARAM(BM_KvStoreMergeKeyValues, 10_10, 10, 10);
BENCHMARK_NAMED_PARAM(BM_KvStoreMergeKeyValues, 100_10, 100, 10);
BENCHMARK_NAMED_PARAM(BM_KvStoreMergeKeyValues, 1000_10, 1000, 10);
BENCHMARK_NAMED_PARAM(BM_KvStoreMergeKeyValues, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_KvStoreMergeKeyValues, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(BM_KvStoreMergeKeyValues, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(BM_KvStoreMergeKeyValues, 10000_10000, 10000, 10000);

// The parameter is number of keyVals already in store
BENCHMARK_PARAM(BM_KvStoreDumpAll, 10);
BENCHMARK_PARAM(BM_KvStoreDumpAll, 100);
BENCHMARK_PARAM(BM_KvStoreDumpAll, 1000);
BENCHMARK_PARAM(BM_KvStoreDumpAll, 10000);

// The parameter is number of keyVals for update
BENCHMARK_PARAM(BM_KvStoreFloodingUpdate, 10);
BENCHMARK_PARAM(BM_KvStoreFloodingUpdate, 100);
BENCHMARK_PARAM(BM_KvStoreFloodingUpdate, 1000);
BENCHMARK_PARAM(BM_KvStoreFloodingUpdate, 10000);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
