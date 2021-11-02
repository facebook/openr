/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/utils/Utils.h>

#include <folly/Benchmark.h>
#include <folly/gen/Base.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/prefix-manager/PrefixManager.h>

namespace openr {

class PrefixManagerBenchmarkTestFixture {
 public:
  explicit PrefixManagerBenchmarkTestFixture(
      const std::string& nodeId, int areaNum) {
    // Construct basic `OpenrConfig`

    std::vector<openr::thrift::AreaConfig> areaConfig;
    for (size_t i = 0; i < areaNum; ++i) {
      areaConfig.emplace_back(
          createAreaConfig(std::to_string(i), {".*"}, {".*"}));
    }
    auto tConfig = getBasicOpenrConfig(nodeId, "doamin", areaConfig);
    tConfig.enable_kvstore_request_queue_ref() = true;
    config_ = std::make_shared<Config>(tConfig);

    // Spawn `KvStore` and `PrefixManager`
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(context_, config_);
    kvStoreWrapper_->run();

    prefixManager_ = std::make_unique<PrefixManager>(
        staticRouteUpdatesQueue_,
        kvRequestQueue_,
        prefixMgrRouteUpdatesQueue_,
        initializationEventQueue_,
        kvStoreUpdatesQueue_.getReader(),
        prefixUpdatesQueue_.getReader(),
        fibRouteUpdatesQueue_.getReader(),
        config_,
        kvStoreWrapper_->getKvStore());

    prefixManagerThread_ =
        std::make_unique<std::thread>([this]() { prefixManager_->run(); });
    prefixManager_->waitUntilRunning();
  }

  ~PrefixManagerBenchmarkTestFixture() {
    staticRouteUpdatesQueue_.close();
    prefixUpdatesQueue_.close();
    prefixMgrRouteUpdatesQueue_.close();

    fibRouteUpdatesQueue_.close();
    kvRequestQueue_.close();
    kvStoreUpdatesQueue_.close();
    initializationEventQueue_.close();
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
  pushEventsInPrefixUpdatesQueue(PrefixEvent event) {
    prefixUpdatesQueue_.push(std::move(event));
  }

  void
  checkKeyValRequest(
      uint32_t num, messaging::RQueue<KeyValueRequest> kvRequestReaderQ) {
    auto suspender = folly::BenchmarkSuspender();
    // auto kvRequestQ = kvRequestQueue_.getReader();

    // Start measuring time
    suspender.dismiss();
    while (true) {
      auto total = kvRequestReaderQ.size();
      if (total != 0) {
        // Stop measuring time
        suspender.rehire();

        if (total >= num) {
          return;
        }

        // Start measuring time again
        suspender.dismiss();
      }

      // Wait until all keys are populated
      std::this_thread::yield();
    }
  }

  fbzmq::Context context_;

  // Queue for publishing entries to PrefixManager
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue_;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> prefixMgrRouteUpdatesQueue_;
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue_;
  messaging::ReplicateQueue<thrift::InitializationEvent>
      initializationEventQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue_;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;

  std::shared_ptr<Config> config_;
  std::unique_ptr<PrefixManager> prefixManager_;
  std::unique_ptr<std::thread> prefixManagerThread_;
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_;
  PrefixGenerator prefixGenerator_; // for prefixes generation usage
};

/*
 * Benchmark test for Prefix Advertisement: The time measured includes prefix
 * manager processing time and pushes KeyValRequests into kvRequestQueue.
 *   - from prefixes arrived in prefix manager
 *   - to the requests are shown in the kvRequestQueue
 * Test setup:
 *  - Generate `numOfExistingPrefixes` and inject them into prefix manager
 * Benchmark:
 *  - Generate `numOfUpdatedPrefixes` and observe KeyValRequests
 */
static void
BM_AdvertiseWithKvRequestQueue(
    uint32_t iters,
    uint32_t numOfExistingPrefixes,
    uint32_t numOfUpdatedPrefixes) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();

  const std::string nodeId{"node-1"};
  for (uint32_t i = 0; i < iters; ++i) {
    auto testFixture =
        std::make_unique<PrefixManagerBenchmarkTestFixture>(nodeId, 1);

    // Create a reader to read requests showing up in kvRequestQueue
    auto kvRequestReaderQ = testFixture->kvRequestQueue_.getReader();
    // Generate `numOfExistingPrefixes`
    auto prefixes = generatePrefixEntries(
        testFixture->getPrefixGenerator(), numOfExistingPrefixes);
    // Generate events to be pushed into prefixUpdatesQueue_
    auto events = PrefixEvent(
        PrefixEventType::ADD_PREFIXES, thrift::PrefixType::BGP, prefixes);
    testFixture->prefixUpdatesQueue_.push(std::move(events));

    // Verify corresponding requests inside kvRequestQueue
    testFixture->checkKeyValRequest(numOfExistingPrefixes, kvRequestReaderQ);

    // Generate `numOfUpdatedPrefixes`
    auto prefixesToAdvertise = generatePrefixEntries(
        testFixture->getPrefixGenerator(), numOfUpdatedPrefixes);

    // Generate events to be pushed into prefixUpdatesQueue_
    auto addEvents = PrefixEvent(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::BGP,
        prefixesToAdvertise);

    // Start measuring benchmark time
    suspender.dismiss();

    // Push events and wait until requests show up in kvRequestQueue
    testFixture->prefixUpdatesQueue_.push(std::move(addEvents));
    testFixture->checkKeyValRequest(
        numOfExistingPrefixes + numOfUpdatedPrefixes, kvRequestReaderQ);

    // Stop measuring benchmark time
    suspender.rehire();
  }
}

/*
 * Benchmark test for Prefix Withdrawals: The time measured includes prefix
 * manager processing time and pushes KeyValRequests into kvRequestQueue.
 * Test setup:
 *  - Generate `numOfExistingPrefixes` and inject them into `KvStore`
 * Benchmark:
 *  - Withdraw `numOfWithdrawPrefixes` chunk from previous injected prefixes
 */
static void
BM_WithdrawWithKvRequestQueue(
    uint32_t iters,
    uint32_t numOfExistingPrefixes,
    uint32_t numOfWithdrawnPrefixes) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();

  // Make sure num of withdrawn prefixes are subset of existing prefixes
  CHECK_LE(numOfWithdrawnPrefixes, numOfExistingPrefixes);

  const std::string nodeId{"node-1"};
  for (uint32_t i = 0; i < iters; ++i) {
    auto testFixture =
        std::make_unique<PrefixManagerBenchmarkTestFixture>(nodeId, 1);

    // Create a reader to read requests showing up in kvRequestQueue
    auto kvRequestReaderQ = testFixture->kvRequestQueue_.getReader();
    // Generate `numOfExistingPrefixes`
    auto prefixes = generatePrefixEntries(
        testFixture->getPrefixGenerator(), numOfExistingPrefixes);
    // Generate events to be pushed into prefixUpdatesQueue_
    auto events = PrefixEvent(
        PrefixEventType::ADD_PREFIXES, thrift::PrefixType::BGP, prefixes);
    testFixture->prefixUpdatesQueue_.push(std::move(events));

    // Verify corresponding requests inside kvRequestQueue
    testFixture->checkKeyValRequest(numOfExistingPrefixes, kvRequestReaderQ);

    auto prefixesToWithdraw = prefixes; // NOTE explicitly copy
    prefixesToWithdraw.resize(numOfWithdrawnPrefixes);

    // Generate events to be pushed into prefixUpdatesQueue_
    auto withdrawEvents = PrefixEvent(
        PrefixEventType::WITHDRAW_PREFIXES,
        thrift::PrefixType::BGP,
        prefixesToWithdraw);

    // Start measuring benchmark time
    suspender.dismiss();

    // Push events and wait until requests shows up in kvRequestQueue
    testFixture->prefixUpdatesQueue_.push(std::move(withdrawEvents));
    testFixture->checkKeyValRequest(
        numOfExistingPrefixes + numOfWithdrawnPrefixes, kvRequestReaderQ);

    // Stop measuring benchmark time
    suspender.rehire();
  }
}

/*
 * Benchmark test for Redistribution of Fib add unicast route:
 * The time measured starts from routeUpdates are pushed to fibRouteUpdatesQueue
 * and ends by checking expected number of prefixes show up in kvRequestQueue.
 * Test setup:
 *  - Generate 2 area configuration
 *  - Generate `numOfExistingPrefixes` routes as existing setup
 *  - Generate unicast routes to be distributed from the other area
 * Benchmark:
 *  - Push Fib add unicast routeUpdates into fibRouteUpdatesQueue
 *  - and observe KeyValRequests
 */

static void
BM_RedistributeFibAddRoute(
    uint32_t iters,
    uint32_t numOfExistingPrefixes,
    uint32_t numOfRedistributeRoutes) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();

  const std::string nodeId{"node-1"};
  for (uint32_t i = 0; i < iters; ++i) {
    auto testFixture =
        std::make_unique<PrefixManagerBenchmarkTestFixture>(nodeId, 2);

    // Create a reader to read requests showing up in kvRequestQueue
    auto kvRequestReaderQ = testFixture->kvRequestQueue_.getReader();

    // Generate numOfExistingPrefixes of unicast routes to be added
    // All routes are contained in single DecisionRouteUpdate
    auto routeUpdateForExisting = generateDecisionRouteUpdate(
        testFixture->getPrefixGenerator(), numOfExistingPrefixes);
    testFixture->fibRouteUpdatesQueue_.push(std::move(routeUpdateForExisting));

    // Verify corresponding requests inside kvRequestQueue
    testFixture->checkKeyValRequest(numOfExistingPrefixes, kvRequestReaderQ);

    // Generate numOfRedistributeRoutes of unicast routes to be redistributed
    // All routes are contained in single DecisionRouteUpdate
    auto routeUpdate = generateDecisionRouteUpdate(
        testFixture->getPrefixGenerator(), numOfRedistributeRoutes);

    // Start measuring benchmark time
    suspender.dismiss();

    // Push DecisionRouteUpdate to fibRouteUpdatesQueue
    testFixture->fibRouteUpdatesQueue_.push(std::move(routeUpdate));
    testFixture->checkKeyValRequest(
        numOfExistingPrefixes + numOfRedistributeRoutes, kvRequestReaderQ);

    // Stop measuring benchmark time
    suspender.rehire();
  }
}

/*
 * Benchmark test for Redistribution of Fib delete unicast route:
 * The time measured starts from routeUpdates are pushed to fibRouteUpdatesQueue
 * and ends by checking expected number of prefixes show up in kvRequestQueue.
 * Test setup:
 *  - Generate 2 area configuration
 *  - Generate `numOfExistingPrefixes` routes as existing setup
 *  - Generate unicast routes to be distributed from the other area
 * Benchmark:
 *  - Push Fib delete unicast routeUpdates into fibRouteUpdatesQueue
 *  - and observe KeyValRequests
 */

static void
BM_RedistributeFibDeleteRoute(
    uint32_t iters,
    uint32_t numOfExistingPrefixes,
    uint32_t numOfRedistributeRoutes) {
  // Spawn suspender object to NOT calculating setup time into benchmark
  auto suspender = folly::BenchmarkSuspender();

  // Make sure the number of delete routes are subset of existing routes
  CHECK_LE(numOfRedistributeRoutes, numOfExistingPrefixes);

  const std::string nodeId{"node-1"};
  for (uint32_t i = 0; i < iters; ++i) {
    auto testFixture =
        std::make_unique<PrefixManagerBenchmarkTestFixture>(nodeId, 2);

    // Create a reader to read requests showing up in kvRequestQueue
    auto kvRequestReaderQ = testFixture->kvRequestQueue_.getReader();

    // Generate `numOfExistingPrefixes` of prefix entries
    auto prefixEntries = generatePrefixEntries(
        testFixture->getPrefixGenerator(), numOfExistingPrefixes);

    // Generate numOfExistingPrefixes of unicast routes to be added
    // All routes are contained in single DecisionRouteUpdate
    auto routeUpdateForExisting =
        generateDecisionRouteUpdateFromPrefixEntries(prefixEntries);
    testFixture->fibRouteUpdatesQueue_.push(std::move(routeUpdateForExisting));

    // Verify corresponding requests inside kvRequestQueue
    testFixture->checkKeyValRequest(numOfExistingPrefixes, kvRequestReaderQ);

    // Redistributed prefix entries will take the last `numOfRedistributeRoutes`
    auto prefixesToRedistribute = prefixEntries;
    prefixesToRedistribute.resize(numOfRedistributeRoutes);

    // Generate numOfRedistributeRoutes of unicast routes to be redistributed
    // All routes are contained in single DecisionRouteUpdate
    DecisionRouteUpdate routeUpdate;
    for (auto& prefixEntry : prefixEntries) {
      routeUpdate.unicastRoutesToDelete.emplace_back(
          toIPNetwork(prefixEntry.get_prefix()));
    }

    // Start measuring benchmark time
    suspender.dismiss();

    // Push DecisionRouteUpdate to fibRouteUpdatesQueue
    testFixture->fibRouteUpdatesQueue_.push(std::move(routeUpdate));
    testFixture->checkKeyValRequest(
        numOfExistingPrefixes + numOfRedistributeRoutes, kvRequestReaderQ);

    // Stop measuring benchmark time
    suspender.rehire();
  }
}

/*
 * @first integer: number of prefixes existing inside PrefixManager
 * @second integer: number of prefixes to advertise
 */

BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 100_1, 100, 1);
BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 1000_1, 1000, 1);
BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 10000_1, 10000, 1);
BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(
    BM_AdvertiseWithKvRequestQueue, 10000_10000, 10000, 10000);
BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 100000_1, 100000, 1);
BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 100000_10, 100000, 10);
BENCHMARK_NAMED_PARAM(BM_AdvertiseWithKvRequestQueue, 100000_100, 100000, 100);
BENCHMARK_NAMED_PARAM(
    BM_AdvertiseWithKvRequestQueue, 100000_1000, 100000, 1000);
BENCHMARK_NAMED_PARAM(
    BM_AdvertiseWithKvRequestQueue, 100000_10000, 100000, 10000);
BENCHMARK_NAMED_PARAM(
    BM_AdvertiseWithKvRequestQueue, 100000_100000, 100000, 100000);

/*
 * @first integer: number of prefixes existing inside PrefixManager
 * @second integer: number of prefixes to withdraw
 */

BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 100_1, 100, 1);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 1000_1, 1000, 1);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 10000_1, 10000, 1);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 10000_10000, 10000, 10000);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 100000_1, 100000, 1);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 100000_10, 100000, 10);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 100000_100, 100000, 100);
BENCHMARK_NAMED_PARAM(BM_WithdrawWithKvRequestQueue, 100000_1000, 100000, 1000);
BENCHMARK_NAMED_PARAM(
    BM_WithdrawWithKvRequestQueue, 100000_10000, 100000, 10000);
BENCHMARK_NAMED_PARAM(
    BM_WithdrawWithKvRequestQueue, 100000_100000, 100000, 100000);

/*
 * @first integer: number of prefixes existing inside PrefixManager
 * @second integer: number of redistributed Fib add route
 */

BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 100_1, 100, 1);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 1000_1, 1000, 1);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 10000_1, 10000, 1);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 10000_10000, 10000, 10000);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 100000_1, 100000, 1);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 100000_10, 100000, 10);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 100000_100, 100000, 100);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 100000_1000, 100000, 1000);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibAddRoute, 100000_10000, 100000, 10000);
BENCHMARK_NAMED_PARAM(
    BM_RedistributeFibAddRoute, 100000_100000, 100000, 100000);

/*
 * @first integer: number of prefixes existing inside PrefixManager
 * @second integer: number of redistributed Fib delete route
 */

BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 100_1, 100, 1);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 1000_1, 1000, 1);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 10000_1, 10000, 1);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 10000_10, 10000, 10);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 10000_100, 10000, 100);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 10000_1000, 10000, 1000);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 10000_10000, 10000, 10000);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 100000_1, 100000, 1);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 100000_10, 100000, 10);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 100000_100, 100000, 100);
BENCHMARK_NAMED_PARAM(BM_RedistributeFibDeleteRoute, 100000_1000, 100000, 1000);
BENCHMARK_NAMED_PARAM(
    BM_RedistributeFibDeleteRoute, 100000_10000, 100000, 10000);
BENCHMARK_NAMED_PARAM(
    BM_RedistributeFibDeleteRoute, 100000_100000, 100000, 100000);
} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
