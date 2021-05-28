/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <openr/config/tests/Utils.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/mocks/PrefixGenerator.h>

namespace detail {

// interval for periodic sync
const std::chrono::seconds kDbSyncIntervalOverride(10000);
// Prefix length of a subnet
static const uint8_t kBitMaskLen = 128;

} // namespace detail

namespace openr {

class PrefixManagerBenchmarkTestFixture {
 public:
  explicit PrefixManagerBenchmarkTestFixture(const std::string& nodeId) {
    // construct basic `OpenrConfig`
    auto tConfig = getBasicOpenrConfig(nodeId);
    tConfig.kvstore_config_ref()->sync_interval_s_ref() =
        ::detail::kDbSyncIntervalOverride.count();
    config_ = std::make_shared<Config>(tConfig);

    // spawn `KvStore` and `PrefixManager` for benchmarking
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(context_, config_);
    kvStoreWrapper_->run();

    prefixManager_ = std::make_unique<PrefixManager>(
        staticRouteUpdatesQueue_,
        prefixUpdatesQueue_.getReader(),
        fibRouteUpdatesQueue_.getReader(),
        config_,
        kvStoreWrapper_->getKvStore(),
        std::chrono::seconds{0} /* no delay for initial dump */);

    prefixManagerThread_ =
        std::make_unique<std::thread>([this]() { prefixManager_->run(); });
    prefixManager_->waitUntilRunning();
  }

  ~PrefixManagerBenchmarkTestFixture() {
    staticRouteUpdatesQueue_.close();
    prefixUpdatesQueue_.close();
    fibRouteUpdatesQueue_.close();
    kvStoreWrapper_->closeQueue();

    prefixManager_->stop();
    prefixManagerThread_->join();
    prefixManager_.reset();

    kvStoreWrapper_->stop();
    kvStoreWrapper_.reset();
  }

  PrefixManager*
  getPrefixManager() {
    return prefixManager_.get();
  }

  PrefixGenerator&
  getPrefixGenerator() {
    return prefixGenerator_;
  }

  void
  checkPrefixesInKvStore(uint32_t num) {
    while (true) {
      auto res = kvStoreWrapper_->dumpHashes(
          kTestingAreaName, Constants::kPrefixDbMarker.toString());
      if (res.size() >= num) {
        break;
      }
      // wait until all keys are populated
      std::this_thread::yield();
    }
  }

  void
  checkThriftPublication(uint32_t num, bool checkDeletion) {
    auto suspender = folly::BenchmarkSuspender();
    uint32_t total{0};
    auto kvStoreUpdatesQ = kvStoreWrapper_->getReader();

    // start measuring time
    suspender.dismiss();
    while (true) {
      auto thriftPub = kvStoreUpdatesQ.get();
      if (not thriftPub.hasValue()) {
        continue;
      }

      // stop measuring time as this is just parsing
      suspender.rehire();
      if (not checkDeletion) {
        total += thriftPub.value().keyVals_ref()->size();
      } else {
        for (const auto& [key, tVal] : *thriftPub.value().keyVals_ref()) {
          if (auto value = tVal.value_ref()) {
            const auto prefixDb =
                readThriftObjStr<thrift::PrefixDatabase>(*value, serializer_);
            if (*prefixDb.deletePrefix_ref()) {
              ++total;
            }
          }
        }
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

 private:
  fbzmq::Context context_;

  // Queue for publishing entries to PrefixManager
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue_;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue_;

  std::shared_ptr<Config> config_;
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_;
  std::unique_ptr<PrefixManager> prefixManager_;
  std::unique_ptr<std::thread> prefixManagerThread_;
  PrefixGenerator prefixGenerator_; // for prefixes generation usage
  apache::thrift::CompactSerializer serializer_;
};

std::vector<thrift::PrefixEntry>
generatePrefixEntries(uint32_t num, PrefixGenerator& prefixGenerator) {
  // generate `num` of random prefixes
  std::vector<thrift::IpPrefix> prefixes =
      prefixGenerator.ipv6PrefixGenerator(num, ::detail::kBitMaskLen);
  auto tPrefixEntries =
      folly::gen::from(prefixes) |
      folly::gen::mapped([](const thrift::IpPrefix& prefix) {
        return createPrefixEntry(prefix, thrift::PrefixType::DEFAULT);
      }) |
      folly::gen::as<std::vector<thrift::PrefixEntry>>();
  return tPrefixEntries;
}

/*
 * Benchmark test for advertisePrefixes:
 * Test setup:
 *  - Generate `numOfExistingPrefixes` and inject them into KvStore
 * Benchmark:
 *  - Generate `numOfUpdatedPrefixes` and inject them into KvStore
 */
static void
BM_PrefixManagerAdvertisePrefixes(
    uint32_t iters,
    uint32_t numOfExistingPrefixes,
    uint32_t numOfUpdatedPrefixes) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();

  const std::string nodeId{"node-1"};
  auto testFixture =
      std::make_unique<PrefixManagerBenchmarkTestFixture>(nodeId);
  auto prefixMgr = testFixture->getPrefixManager();

  // Generate `numOfExistingPrefixes` and make sure `KvStore` is updated
  auto prefixes = generatePrefixEntries(
      numOfExistingPrefixes, testFixture->getPrefixGenerator());
  prefixMgr->advertisePrefixes(std::move(prefixes)).get();

  // Verify pre-existing prefixes inside `KvStore`
  testFixture->checkPrefixesInKvStore(numOfExistingPrefixes);

  // Generate `numOfUpdatedPrefixes`
  auto prefixesToAdvertise = generatePrefixEntries(
      numOfUpdatedPrefixes, testFixture->getPrefixGenerator());

  // Start measuring benchmark time
  suspender.dismiss();

  for (uint32_t i = 0; i < iters; ++i) {
    // advertise prefixes into `KvStore` and make sure update received
    prefixMgr->advertisePrefixes(prefixesToAdvertise).get();
    testFixture->checkThriftPublication(numOfUpdatedPrefixes, false);
  }
}

/*
 * Benchmark test for witndrawPrefixes:
 * Test setup:
 *  - Generate `numOfExistingPrefixes` and inject them into `KvStore`
 * Benchmark:
 *  - Withdraw `numOfWithdrawPrefixes` chunk from previous injected prefixes
 */
static void
BM_PrefixManagerWithdrawPrefixes(
    uint32_t iters,
    uint32_t numOfExistingPrefixes,
    uint32_t numOfWithdrawnPrefixes) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();

  // Make sure num of withdrawn prefixes are subset of existing prefixes
  CHECK_LE(numOfWithdrawnPrefixes, numOfExistingPrefixes);

  const std::string nodeId{"node-1"};
  auto testFixture =
      std::make_unique<PrefixManagerBenchmarkTestFixture>(nodeId);
  auto prefixMgr = testFixture->getPrefixManager();

  // Generate `numOfExistingPrefixes` and advertise to `KvStore` first
  auto prefixes = generatePrefixEntries(
      numOfExistingPrefixes, testFixture->getPrefixGenerator());
  auto prefixesToWithdraw = prefixes; // NOTE explicitly copy
  prefixesToWithdraw.resize(numOfWithdrawnPrefixes);
  prefixMgr->advertisePrefixes(prefixes);

  // Verify pre-existing prefixes inside `KvStore
  testFixture->checkPrefixesInKvStore(numOfExistingPrefixes);

  // Start measuring benchmark time
  suspender.dismiss();

  for (uint32_t i = 0; i < iters; ++i) {
    // withdraw prefixes from `KvStore` and make sure update received
    prefixMgr->withdrawPrefixes(prefixesToWithdraw).get();
    testFixture->checkThriftPublication(numOfWithdrawnPrefixes, true);
  }
}

/*
 * @first integer: number of prefixes existing inside PrefixManager
 * @second integer: number of prefixes to advertise/withdraw
 */
BENCHMARK_NAMED_PARAM(BM_PrefixManagerAdvertisePrefixes, 100_1, 100, 1);
BENCHMARK_NAMED_PARAM(BM_PrefixManagerAdvertisePrefixes, 1000_1, 1000, 1);
BENCHMARK_NAMED_PARAM(BM_PrefixManagerAdvertisePrefixes, 10000_1, 10000, 1);
BENCHMARK_NAMED_PARAM(BM_PrefixManagerAdvertisePrefixes, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_PrefixManagerAdvertisePrefixes, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(
    BM_PrefixManagerAdvertisePrefixes, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(
    BM_PrefixManagerAdvertisePrefixes, 10000_10000, 10000, 10000);

BENCHMARK_NAMED_PARAM(BM_PrefixManagerWithdrawPrefixes, 100_1, 100, 1);
BENCHMARK_NAMED_PARAM(BM_PrefixManagerWithdrawPrefixes, 1000_1, 1000, 1);
BENCHMARK_NAMED_PARAM(BM_PrefixManagerWithdrawPrefixes, 10000_1, 10000, 1);
BENCHMARK_NAMED_PARAM(BM_PrefixManagerWithdrawPrefixes, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_PrefixManagerWithdrawPrefixes, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(
    BM_PrefixManagerWithdrawPrefixes, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(
    BM_PrefixManagerWithdrawPrefixes, 10000_10000, 10000, 10000);

/*
 * TODO: add decision route processing benchmark
 */

/*
 * TODO: add initial sync of KvStore benchmark
 */

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
