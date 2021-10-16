/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>
#include <folly/gen/Base.h>
#include <openr/common/Types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/utils/Utils.h>

namespace openr {

// Approximate prefix key and value length in production
const uint32_t keyLen(60);
const uint32_t valLen(300);

class kvStoreBenchmarkTestFixture {
 public:
  explicit kvStoreBenchmarkTestFixture(const std::string& nodeId) {
    auto tConfig = getBasicOpenrConfig(nodeId, "domain");
    auto config = std::make_shared<Config>(tConfig);

    // start kvstore
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(
        context_,
        config,
        std::nullopt /* peerUpdatesQueue */,
        kvRequestQueue_.getReader());
    kvStoreWrapper_->run();
  }

  void
  checkThriftPublication(
      uint32_t num, messaging::RQueue<Publication> kvStoreUpdatesQ) {
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
      total += thriftPub.value().tPublication.keyVals_ref()->size();

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
    uint32_t iters, uint32_t numOfExistingKeys, uint32_t numOfUpdatedKeys) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();

  const std::string nodeId = "node-1";
  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<kvStoreBenchmarkTestFixture>(nodeId);
    auto kvStoreUpdatesQ = testFixture->kvStoreWrapper_->getReader();

    // Generate `numOfExistingKeys`
    for (int cnt = 0; cnt < numOfExistingKeys; ++cnt) {
      auto persistKey = PersistKeyValueRequest(
          kTestingAreaName, genRandomStr(keyLen), genRandomStr(valLen));
      testFixture->kvRequestQueue_.push(std::move(persistKey));
    }

    // Verify keys are injected
    testFixture->checkThriftPublication(numOfExistingKeys, kvStoreUpdatesQ);

    // Start measuring benchmark time
    suspender.dismiss();

    for (int cnt = 0; cnt < numOfUpdatedKeys; ++cnt) {
      // Stop measuring benchmark time, just generating key
      suspender.rehire();

      // Generate `numOfUpdatedKeys`
      auto persistKeyAdd = PersistKeyValueRequest(
          kTestingAreaName, genRandomStr(keyLen), genRandomStr(valLen));

      // Resume measuring time
      suspender.dismiss();

      testFixture->kvRequestQueue_.push(std::move(persistKeyAdd));
    }

    // Verify keys are injected
    testFixture->checkThriftPublication(numOfUpdatedKeys, kvStoreUpdatesQ);

    // Stop measuring benchmark time
    suspender.rehire();
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
    uint32_t iters, uint32_t numOfExistingKeys, uint32_t numOfUpdatedKeys) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();

  const std::string nodeId = "node-1";
  for (int i = 0; i < iters; ++i) {
    auto testFixture = std::make_unique<kvStoreBenchmarkTestFixture>(nodeId);
    auto kvStoreUpdatesQ = testFixture->kvStoreWrapper_->getReader();
    std::vector<std::string> keyList;

    // Generate `numOfExistingKeys`
    for (int cnt = 0; cnt < numOfExistingKeys; ++cnt) {
      auto key = genRandomStr(keyLen);
      auto persistKey =
          PersistKeyValueRequest(kTestingAreaName, key, genRandomStr(valLen));
      keyList.emplace_back(std::move(key));
      testFixture->kvRequestQueue_.push(std::move(persistKey));
    }

    testFixture->checkThriftPublication(numOfExistingKeys, kvStoreUpdatesQ);

    // Get `numOfUpdatedKeys` from `numOfExistingKeys`
    auto updateKeys = keyList;
    updateKeys.resize(numOfUpdatedKeys);

    // Start measuring benchmark time
    suspender.dismiss();

    for (auto& updateKey : updateKeys) {
      // Stop measuring benchmark time, just generating key
      suspender.rehire();

      // Generate updated PersistKeyValueRequest
      auto persistKeyAdd = PersistKeyValueRequest(
          kTestingAreaName, updateKey, genRandomStr(valLen));

      // Resume measuring time
      suspender.dismiss();

      testFixture->kvRequestQueue_.push(std::move(persistKeyAdd));
    }

    // Verify keys are injected
    testFixture->checkThriftPublication(numOfUpdatedKeys, kvStoreUpdatesQ);

    // Stop measuring benchmark time
    suspender.rehire();
  }
}

/*
 * @first integer: number of keys existing inside kvStore
 * @second integer: number of keys to persist for the first time
 */

BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 100_1, 100, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 1000_1, 1000, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 10000_1, 10000, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 10000_10000, 10000, 10000);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 100000_1, 100000, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 100000_10, 100000, 10);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 100000_100, 100000, 100);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 100000_1000, 100000, 1000);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreFirstTimePersistKey, 100000_10000, 100000, 10000);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreFirstTimePersistKey, 100000_100000, 100000, 100000);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 1000000_1, 1000000, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreFirstTimePersistKey, 1000000_100, 1000000, 100);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreFirstTimePersistKey, 1000000_10000, 1000000, 10000);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreFirstTimePersistKey, 1000000_100000, 1000000, 100000);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreFirstTimePersistKey, 1000000_1000000, 1000000, 1000000);

/*
 * @first integer: number of keys existing inside kvStore
 * @second integer: number of keys to persist for second time
 */

BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 100_1, 100, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 1000_1, 1000, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 10000_1, 10000, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 10000_10000, 10000, 10000);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 100000_1, 100000, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 100000_10, 100000, 10);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 100000_100, 100000, 100);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 100000_1000, 100000, 1000);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 100000_10000, 100000, 10000);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreUpdatePersistKey, 100000_100000, 100000, 100000);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 1000000_1, 1000000, 1);
BENCHMARK_NAMED_PARAM(BM_KvStoreUpdatePersistKey, 1000000_100, 1000000, 100);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreUpdatePersistKey, 1000000_10000, 1000000, 10000);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreUpdatePersistKey, 1000000_100000, 1000000, 100000);
BENCHMARK_NAMED_PARAM(
    BM_KvStoreUpdatePersistKey, 1000000_1000000, 1000000, 1000000);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
