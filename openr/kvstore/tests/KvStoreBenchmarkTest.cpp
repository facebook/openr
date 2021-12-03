/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>
#include <folly/gen/Base.h>
#include <openr/common/Types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/monitor/SystemMetrics.h>
#include <openr/tests/utils/Utils.h>

#define STR_CONCATENATE(s1, s2, s3, s4) s1##_##s2##_##s3##_##s4
#define THREE_CONCATENATE(s1, s2, s3) s1##_##s2##_##s3

/**
 * Defines a benchmark that allows users to record customized counter during
 * benchmarking and passes a parameter to another one. This is common for
 * benchmarks that need a "problem size" in addition to "number of iterations".
 */
#define BENCHMARK_COUNTERS_PARAM(name, counters, existing, update) \
  BENCHMARK_COUNTERS_NAME_PARAM(                                   \
      name,                                                        \
      counters,                                                    \
      FB_CONCATENATE(existing, FB_CONCATENATE(_, update)),         \
      existing,                                                    \
      update)

#define BENCHMARK_COUNTERS_PARAM2(                     \
    name, counters, param1, param2, param3, param4)    \
  BENCHMARK_COUNTERS_NAME_PARAM(                       \
      name,                                            \
      counters,                                        \
      STR_CONCATENATE(param1, param2, param3, param4), \
      param1,                                          \
      param2,                                          \
      param3,                                          \
      param4)

#define BENCHMARK_COUNTERS_PARAM3(name, counters, param1, param2, param3) \
  BENCHMARK_COUNTERS_NAME_PARAM(                                          \
      name,                                                               \
      counters,                                                           \
      THREE_CONCATENATE(param1, param2, param3),                          \
      param1,                                                             \
      param2,                                                             \
      param3)

/*
 * Like BENCHMARK_COUNTERS_PARAM(), but allows a custom name to be specified for
 * each parameter, rather than using the parameter value.
 */
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

namespace openr {

// Approximate prefix key and value length in production
const uint32_t kKeyLen(60);
const uint32_t kValLen(300);
// Ttl time reference
const uint64_t kTtl{300000};

// number of total devices
const uint32_t kDevice{48};

class KvStoreBenchmarkTestFixture {
 public:
  explicit KvStoreBenchmarkTestFixture() {
    auto tConfig = getBasicOpenrConfig(nodeId_, "domain");
    config_ = std::make_shared<Config>(tConfig);

    // start kvstore
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(
        context_,
        config_,
        std::nullopt /* peerUpdatesQueue */,
        kvRequestQueue_.getReader());
    kvStoreWrapper_->run();
  }

  /*
   * Description:
   * - Generate `numOfEntries` of keyVals and push corresponding queueEntry
   *   to TtlCountdownQueue
   * - Return a subset of keys that are kept in TtlCountdownQueue
   *
   * @first param: num of entries to be pushed in the ttlCountdownQueue
   * @second param: num of entries in ttlCountdownQueue that contains the
   *                the keys to be returned
   * @third param: a TtlCountdownQueue
   *
   * @return: a subset of keys that exist in ttlCountdownQueue
   */

  std::unordered_map<std::string, thrift::Value>
  setCountdownQueueEntry(
      uint32_t numOfEntries,
      uint32_t numOfReturnEntries,
      TtlCountdownQueue& ttlCountdownQueue) {
    std::unordered_map<std::string, thrift::Value> keyValsForReturn;
    thrift::Publication thriftPub;
    for (uint32_t i = 0; i < numOfEntries; ++i) {
      auto keyValPair =
          genRandomKvStoreKeyVal(kKeyLen, kValLen, 1, "originator", kTtl);
      if (i < numOfReturnEntries) {
        keyValsForReturn[keyValPair.first] = keyValPair.second;
      }

      TtlCountdownQueueEntry queueEntry;
      queueEntry.expiryTime = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(keyValPair.second.get_ttl());
      queueEntry.key = keyValPair.first;
      queueEntry.version = (keyValPair.second).get_version();
      queueEntry.ttlVersion = (keyValPair.second).get_ttlVersion();
      queueEntry.originatorId = (keyValPair.second).get_originatorId();
      ttlCountdownQueue.push(std::move(queueEntry));
    }
    return keyValsForReturn;
  }

  void
  checkThriftPublication(
      uint32_t num, messaging::RQueue<KvStorePublication> kvStoreUpdatesQ) {
    auto suspender = folly::BenchmarkSuspender();
    uint32_t total{0};

    // start measuring time
    suspender.dismiss();
    while (true) {
      auto thriftPub = kvStoreUpdatesQ.get();
      if (not thriftPub.hasValue()) {
        continue;
      }

      // stop measuring time as this is just parsing
      suspender.rehire();
      if (auto* pub = std::get_if<thrift::Publication>(&(thriftPub.value()))) {
        total += pub->keyVals_ref()->size();
      }

      // start measuring time again
      suspender.dismiss();

      if (total >= num) {
        return;
      }

      // wait until all keys are populated
      std::this_thread::yield();
    }
  }

  // Public member variables
  fbzmq::Context context_;

  // Internal stores
  std::shared_ptr<Config> config_;
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;

 private:
  const std::string nodeId_{"node-1"};
};

/*
 * Benchmark test for first time PersistKeyValueRequest: The time
 * measured includes KvStore processing time
 * Test setup:
 *  - Generate `numOfExistingKeys` and inject them into KvStore
 *    as existing setup
 * Benchmark:
 *  - Generate `numOfUpdatedKeys` and inject them into KvStore
 *    and observe KvStore publications
 */

static void
BM_KvStoreFirstTimePersistKey(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfExistingKeys,
    uint32_t numOfUpdatedKeys) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();
  // Add boolean to control profiling memory for the 1st iteration
  SystemMetrics sysMetrics;
  bool record = true;

  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<KvStoreBenchmarkTestFixture>();
    auto kvStoreUpdatesQ = testFixture->kvStoreWrapper_->getReader();

    // Generate `numOfExistingKeys`
    for (int cnt = 0; cnt < numOfExistingKeys; ++cnt) {
      auto persistKey = PersistKeyValueRequest(
          kTestingAreaName, genRandomStr(kKeyLen), genRandomStr(kValLen));
      testFixture->kvRequestQueue_.push(std::move(persistKey));
    }

    // Verify keys are injected
    testFixture->checkThriftPublication(numOfExistingKeys, kvStoreUpdatesQ);

    // Start measuring benchmark time
    suspender.dismiss();

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }
    for (int cnt = 0; cnt < numOfUpdatedKeys; ++cnt) {
      // Stop measuring benchmark time, just generating key
      suspender.rehire();

      // Generate `numOfUpdatedKeys`
      auto persistKeyAdd = PersistKeyValueRequest(
          kTestingAreaName, genRandomStr(kKeyLen), genRandomStr(kValLen));

      // Resume measuring time
      suspender.dismiss();

      testFixture->kvRequestQueue_.push(std::move(persistKeyAdd));
    }

    // Verify keys are injected
    testFixture->checkThriftPublication(numOfUpdatedKeys, kvStoreUpdatesQ);

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

/*
 * Benchmark test for update key's version PersistKeyValueRequest:
 * The time measured includes KvStore processing time
 * Test setup:
 *  - Generate `numOfExistingKeys` and inject them into KvStore
 *    as existing setup
 * Benchmark:
 *  - Generate `numOfUpdatedKeys` and inject them into KvStore
 *    and observe KvStore publications
 */

static void
BM_KvStoreUpdatePersistKey(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfExistingKeys,
    uint32_t numOfUpdatedKeys) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();
  // Add boolean to control profiling memory for the 1st iteration
  SystemMetrics sysMetrics;
  bool record = true;

  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<KvStoreBenchmarkTestFixture>();
    auto kvStoreUpdatesQ = testFixture->kvStoreWrapper_->getReader();
    std::vector<std::string> keyList;

    // Generate `numOfExistingKeys`
    for (int cnt = 0; cnt < numOfExistingKeys; ++cnt) {
      auto key = genRandomStr(kKeyLen);
      auto persistKey =
          PersistKeyValueRequest(kTestingAreaName, key, genRandomStr(kValLen));
      keyList.emplace_back(std::move(key));
      testFixture->kvRequestQueue_.push(std::move(persistKey));
    }

    testFixture->checkThriftPublication(numOfExistingKeys, kvStoreUpdatesQ);

    // Get `numOfUpdatedKeys` from `numOfExistingKeys`
    auto updateKeys = keyList;
    updateKeys.resize(numOfUpdatedKeys);

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }
    // Start measuring benchmark time
    suspender.dismiss();

    for (auto& updateKey : updateKeys) {
      // Stop measuring benchmark time, just generating key
      suspender.rehire();

      // Generate updated PersistKeyValueRequest
      auto persistKeyAdd = PersistKeyValueRequest(
          kTestingAreaName, updateKey, genRandomStr(kValLen));

      // Resume measuring time
      suspender.dismiss();

      testFixture->kvRequestQueue_.push(std::move(persistKeyAdd));
    }

    // Verify keys are injected
    testFixture->checkThriftPublication(numOfUpdatedKeys, kvStoreUpdatesQ);

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

/*
 * Benchmark test for ClearKeyValueRequest:
 * The time measured includes KvStore processing time
 * Test setup:
 *  - Generate `numOfExistingKeys` and inject them into KvStore
 *    as existing setup
 * Benchmark:
 *  - Generate `numOfClearKeys` and inject them into KvStore
 *    and observe KvStore publications
 */

static void
BM_KvStoreClearKey(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfExistingKeys,
    uint32_t numOfClearKeys) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();
  SystemMetrics sysMetrics;
  bool record = true;

  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<KvStoreBenchmarkTestFixture>();
    auto kvStoreUpdatesQ = testFixture->kvStoreWrapper_->getReader();
    std::vector<std::string> keyList;

    // Generate `numOfExistingKeys`
    for (int cnt = 0; cnt < numOfExistingKeys; ++cnt) {
      auto key = genRandomStr(kKeyLen);
      auto persistKey =
          PersistKeyValueRequest(kTestingAreaName, key, genRandomStr(kValLen));
      keyList.emplace_back(std::move(key));
      testFixture->kvRequestQueue_.push(std::move(persistKey));
    }

    testFixture->checkThriftPublication(numOfExistingKeys, kvStoreUpdatesQ);

    // Get `numOfClearKeys` from `numOfExistingKeys`
    auto clearKeys = keyList;
    clearKeys.resize(numOfClearKeys);

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }
    // Start measuring benchmark time
    suspender.dismiss();

    for (auto& clearKey : clearKeys) {
      // Stop measuring benchmark time, just generating key
      suspender.rehire();

      // Generate updated PersistKeyValueRequest
      auto unsetPrefixRequest = ClearKeyValueRequest(
          kTestingAreaName, clearKey, genRandomStr(kValLen), true);

      // Resume measuring time
      suspender.dismiss();

      testFixture->kvRequestQueue_.push(std::move(unsetPrefixRequest));
    }

    // Verify keys are marked as delete
    testFixture->checkThriftPublication(numOfClearKeys, kvStoreUpdatesQ);

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

/*
 * Benchmark test for dumpDifference:
 * The time measured includes KvStore processing time
 * Test setup:
 *  - Generate `numOfMyKeyVals` and `numOfComparedKeyVals`
 *    and inject them into KvStore as existing setup
 * Benchmark:
 *  - Call dumpDifference function to compare the differences
 *    between `numOfMyKeyVals` and `numOfComparedKeyVals`
 */

static void
BM_KvStoreDumpDifference(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfMyKeyVals,
    uint32_t numOfComparedKeyVals) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();
  SystemMetrics sysMetrics;
  bool record = true;

  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<KvStoreBenchmarkTestFixture>();
    std::unordered_map<std::string, thrift::Value> myKeyVals;
    std::unordered_map<std::string, thrift::Value> comparedKeyVals;
    for (int cnt = 0; cnt < numOfMyKeyVals; ++cnt) {
      const auto& [key, thriftVal] =
          genRandomKvStoreKeyVal(kKeyLen, kValLen, 1);
      myKeyVals[key] = thriftVal;
      if (cnt < numOfComparedKeyVals) {
        comparedKeyVals[key] = thriftVal;
      }
    }

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }
    suspender.dismiss();
    // Call dumpDifference
    dumpDifference(kTestingAreaName, myKeyVals, comparedKeyVals);
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

/*
 * Benchmark test for updatePublicationTtl:
 * Tech setup:
 *  - Generate `numOfMyEntries` and push to ttlCountdownQueue
 *  - Generate `numOfPubEntries` to be updated
 * Benchmark:
 *  - Call updatePublicationTtl function to update Ttl
 *    for `numOfPubEntries`
 */

static void
BM_KvStoreUpdatePubTtl(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfMyEntries,
    uint32_t numOfPubEntries) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();
  SystemMetrics sysMetrics;
  bool record = true;

  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<KvStoreBenchmarkTestFixture>();

    // Create and push `numOfMyEntries` of keyVals to ttlCountdownQueue
    // and return `numOfPubEntries` of keyVals as publication keyVals
    TtlCountdownQueue ttlCountdownQueue;
    auto keyVals = testFixture->setCountdownQueueEntry(
        numOfMyEntries, numOfPubEntries, ttlCountdownQueue);

    // Setup publication with the return keyVals
    thrift::Publication thriftPub;
    thriftPub.keyVals_ref() = std::move(keyVals);
    thriftPub.area_ref() = kTestingAreaName;

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }
    // Start measuring time
    suspender.dismiss();

    updatePublicationTtl(
        ttlCountdownQueue, Constants::kTtlThreshold, thriftPub);

    // Stop measuring time
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

/*
 * Benchmark test for dumpAllWithFilters:
 * Tech setup:
 *  - Generate `numOfExistingKeyVals` and push into kvStore
 *  - Generate the filter condition by specifying `numOfKeysToMatch`,
 *    `numOfOriginatorIds` and `doNotPublishValue` flag
 * Benchmark:
 *  - Call dumpAllWithFilters function to dump a thrift with all
 *    matching keyVals
 */
static void
BM_KvStoreDumpAllWithFilters(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfExistingKeyVals,
    uint32_t numOfKeysToMatch,
    uint32_t numOfOriginatorIds,
    bool doNotPublishValue) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();
  SystemMetrics sysMetrics;
  bool record = true;

  // TODO: Move the common code to KvStoreBenchmarkTestFixture
  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<KvStoreBenchmarkTestFixture>();
    std::unordered_map<std::string, thrift::Value> keyVals;
    std::unordered_set<std::string> prefixSet;
    std::vector<std::string> keyPrefixList;
    thrift::KeyDumpParams keyDumpParams;

    for (int j = 0; j < numOfExistingKeyVals; ++j) {
      int deviceId = folly::Random::rand32() % kDevice;
      auto keyVal =
          genRandomKvStoreKeyVal(kKeyLen, kValLen, 1, std::to_string(deviceId));
      keyVals[keyVal.first] = keyVal.second;
      if (prefixSet.size() <= numOfKeysToMatch) {
        prefixSet.emplace(keyVal.first);
      }
    }

    keyPrefixList.insert(
        keyPrefixList.end(), prefixSet.begin(), prefixSet.end());

    for (int k = 0; k < numOfOriginatorIds; ++k) {
      keyDumpParams.originatorIds_ref()->insert(std::to_string(k));
    }

    const auto keyPrefixMatch =
        KvStoreFilters(keyPrefixList, keyDumpParams.get_originatorIds());

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }
    // Start measuring time
    suspender.dismiss();

    dumpAllWithFilters(
        kTestingAreaName, keyVals, keyPrefixMatch, doNotPublishValue);

    // Stop measuring time
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

/*
 * Benchmark test for dumpHashWithFilters:
 * Tech setup:
 *  - Generate `numOfExistingKeyVals` and push into kvStore
 *  - Generate the filter condition by specifying `numOfKeysToMatch`,
 *    `numOfOriginatorIds`
 * Benchmark:
 *  - Call dumpHashWithFilters function to dump a thrift with all
 *    matching keyVals
 */
static void
BM_KvStoreDumpHashWithFilters(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfExistingKeyVals,
    uint32_t numOfKeysToFilter,
    uint32_t numOfOriginatorIds) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();
  SystemMetrics sysMetrics;
  bool record = true;

  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<KvStoreBenchmarkTestFixture>();

    std::unordered_map<std::string, thrift::Value> keyVals;
    std::unordered_set<std::string> prefixSet; // avoid replicate keys
    std::vector<std::string> keyPrefixList; // key set that's fed into filter
    thrift::KeyDumpParams keyDumpParams;

    for (int j = 0; j < numOfExistingKeyVals; ++j) {
      int deviceId = folly::Random::rand32() % kDevice;
      auto key = genRandomStr(kKeyLen);
      auto keyVal =
          genRandomKvStoreKeyVal(kKeyLen, kValLen, 1, std::to_string(deviceId));
      keyVals[keyVal.first] = keyVal.second;
      if (prefixSet.size() <= numOfKeysToFilter)
        prefixSet.emplace(keyVal.first);
    }

    keyPrefixList.insert(
        keyPrefixList.end(), prefixSet.begin(), prefixSet.end());

    for (int k = 0; k < numOfOriginatorIds; ++k) {
      keyDumpParams.originatorIds_ref()->insert(std::to_string(k));
    }

    const auto keyPrefixMatch =
        KvStoreFilters(keyPrefixList, keyDumpParams.get_originatorIds());

    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory_before_opertion(MB)"] = mem.value() / 1024 / 1024;
      }
    }
    // Start measuring time
    suspender.dismiss();

    dumpHashWithFilters(kTestingAreaName, keyVals, keyPrefixMatch);

    // Stop measuring time
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

/*
 * @first integer: number of keys existing inside kvStore
 * @second integer: number of keys to persist for the first time
 */

BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 100, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 1000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 10000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 10000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 10000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 10000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 10000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 100000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 100000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 100000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 100000, 1000);
BENCHMARK_COUNTERS_PARAM(
    BM_KvStoreFirstTimePersistKey, counters, 100000, 10000);
BENCHMARK_COUNTERS_PARAM(
    BM_KvStoreFirstTimePersistKey, counters, 100000, 100000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 1000000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreFirstTimePersistKey, counters, 1000000, 100);
BENCHMARK_COUNTERS_PARAM(
    BM_KvStoreFirstTimePersistKey, counters, 1000000, 10000);
BENCHMARK_COUNTERS_PARAM(
    BM_KvStoreFirstTimePersistKey, counters, 1000000, 100000);

/*
 * @first integer: number of keys existing inside kvStore
 * @second integer: number of keys to persist for second time
 */

BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 100, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 1000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 10000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 10000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 10000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 10000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 10000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 100000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 100000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 100000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 100000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 100000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 100000, 100000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 1000000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 1000000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 1000000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePersistKey, counters, 1000000, 100000);

/*
 * @first integer: number of keys existing inside kvStore
 * @second integer: number of keys to clear
 */

BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 100, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 1000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 10000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 10000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 10000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 10000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 10000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 100000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 100000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 100000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 100000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 100000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 100000, 100000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 1000000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 1000000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 1000000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreClearKey, counters, 1000000, 1000000);

/*
 * @first integer: number of myKeyVals
 * @second integer: number of comparedKeyVals
 */

BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 100, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 1000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 10000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 10000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 10000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 10000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 10000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 100000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 100000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 100000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 100000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 100000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 100000, 100000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 1000000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 1000000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 1000000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreDumpDifference, counters, 1000000, 1000000);

/*
 * @first integer: num of keyVals in ttlCountdownQueue
 * @second integer: num Of keyVals that will get compared in publication
 */

BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 100, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 1000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 10000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 10000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 10000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 10000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 10000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 100000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 100000, 10);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 100000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 100000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 100000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 100000, 100000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 1000000, 1);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 1000000, 100);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 1000000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_KvStoreUpdatePubTtl, counters, 1000000, 1000000);

/*
 * @first integer: num of existing keyVals in unordered_map
 * @second integer: num of keys to be matched in the filter setting
 * @third integer: num of OriginatorIds to be matched in the filter setting
 * @fourth boolean: flag if the thrift::Value.value is not set in publications
 *
 * Consider cases:
 *  - Keys only, regardless of originator_Ids
 *  - originator_Ids, regardless of keys
 *  - originator_Ids and keys
 */

// Keys only, regardless of originator_Ids
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 1000, 0, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 1000, 0, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 1000, 0, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 1000, 0, false);
// originator_Ids, regardless of keys
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 0, 1, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 0, 10, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 0, 40, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 0, 1, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 0, 10, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 0, 40, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 0, 1, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 0, 10, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 0, 40, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 0, 1, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 0, 10, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 0, 40, false);
// originator_Ids and keys
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 1000, 1, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 1000, 40, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 1000, 1, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 100000, 1000, 40, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 1000, 1, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 1000, 40, true);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 1000, 1, false);
BENCHMARK_COUNTERS_PARAM2(
    BM_KvStoreDumpAllWithFilters, counters, 1000000, 1000, 40, false);

/*
 * @first integer: num of existing keyVals in unordered_map
 * @second integer: num of keys to be matched in the filter setting
 * @third integer: num of OriginatorIds to be matched in the filter setting
 *
 * Consider cases:
 *  - Keys only, regardless of originator_Ids
 *  - originator_Ids, regardless of keys
 *  - originator_Ids and keys
 */

// Keys only, regardless of originator_Ids
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 100000, 1000, 0);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 1000000, 1000, 0);
// originator_Ids, regardless of keys
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 100000, 0, 1);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 100000, 0, 10);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 100000, 0, 40);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 1000000, 0, 1);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 1000000, 0, 10);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 1000000, 0, 40);
// originator_Ids and keys
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 100000, 1000, 1);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 100000, 1000, 10);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 100000, 1000, 40);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 1000000, 1000, 1);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 1000000, 1000, 10);
BENCHMARK_COUNTERS_PARAM3(
    BM_KvStoreDumpHashWithFilters, counters, 1000000, 1000, 40);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
