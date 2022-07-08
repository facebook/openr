/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "common/init/Init.h"

#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/tests/utils/TestUtil.h>

#include <stdexcept>

using namespace openr;
using namespace openr::util;

FOLLY_INIT_LOGGING_CONFIG(
    ".=WARNING"
    ";default:async=true,sync_level=WARNING");

namespace {
const std::unordered_set<std::string> areaIds{kTestingAreaName};
} // namespace

void
runExperiment(
    uint32_t n, size_t nNodes, ClusterTopology topo, size_t nExistingKey = 0) {
#pragma region ClusterSetup
  std::vector<
      std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>
      kvStoreWrappers_;
  std::unordered_map<std::string, thrift::Value> events_;
  BENCHMARK_SUSPEND {
    kvStoreWrappers_.reserve(nNodes);
    for (size_t i = 0; i < nNodes; i++) {
      thrift::KvStoreConfig kvStoreConfig;
      kvStoreConfig.node_name() = genNodeName(i);
      kvStoreWrappers_.emplace_back(
          std::make_unique<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
              areaIds, kvStoreConfig));
      kvStoreWrappers_.at(i)->run();
    }

    generateTopo(kvStoreWrappers_, topo);
#pragma endregion ClusterSetup

#pragma region ExistingKeySetup
    for (size_t i = 0; i < nExistingKey; i++) {
      std::string key = folly::to<std::string>("existingKey", i);
      auto val = createThriftValue(
          1,
          kvStoreWrappers_.front()->getNodeId(),
          folly::to<std::string>("existingValue", i));
      kvStoreWrappers_.front()->setKey(kTestingAreaName, key, val);
      events_.emplace(key, val);
    }
    // Wait for existing key val to converge
    folly::coro::blockingWait(co_waitForConvergence(events_, kvStoreWrappers_));
#pragma endregion ExistingKeySetup

#pragma region EventSetup
    // TODO: make this mult-thread
    for (size_t i = 0; i < n; i++) {
      std::string key = folly::to<std::string>("key", i);
      auto val = createThriftValue(
          1,
          kvStoreWrappers_.front()->getNodeId(),
          folly::to<std::string>("value", i));
      events_.emplace(key, val);
    }
    for (const auto& [key, val] : events_) {
      kvStoreWrappers_.front()->setKey(kTestingAreaName, key, val);
    }
#pragma endregion EventSetup
  }
  // end of BENCHMARK_SUSPEND

  folly::coro::blockingWait(co_waitForConvergence(events_, kvStoreWrappers_));

#pragma region TearDown
  BENCHMARK_SUSPEND {
    // Need to explicity call destructor in suspend mode,
    // otherwise destruct time would be counted.
    // which results in wrong benchmark result.
    kvStoreWrappers_.clear();
  }
#pragma endregion TearDown
}

#pragma region LINEAR
BENCHMARK_NAMED_PARAM(
    runExperiment, 2_NODE_LINEAR_TOPO, 2, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 10_NODE_LINEAR_TOPO, 10, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 20_NODE_LINEAR_TOPO, 20, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 50_NODE_LINEAR_TOPO, 50, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 70_NODE_LINEAR_TOPO, 70, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 100_NODE_LINEAR_TOPO, 100, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 1000_NODE_LINEAR_TOPO, 1000, ClusterTopology::LINEAR);
#pragma endregion LINEAR

BENCHMARK_DRAW_LINE();

#pragma region RING
BENCHMARK_NAMED_PARAM(
    runExperiment, 2_NODE_RING_TOPO, 2, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 10_NODE_RING_TOPO, 10, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 20_NODE_RING_TOPO, 20, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 50_NODE_RING_TOPO, 50, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 70_NODE_RING_TOPO, 70, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 100_NODE_RING_TOPO, 100, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 1000_NODE_RING_TOPO, 1000, ClusterTopology::RING);
#pragma endregion RING

BENCHMARK_DRAW_LINE();

#pragma region STAR
BENCHMARK_NAMED_PARAM(
    runExperiment, 2_NODE_STAR_TOPO, 2, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 10_NODE_STAR_TOPO, 10, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 20_NODE_STAR_TOPO, 20, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 50_NODE_STAR_TOPO, 50, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 70_NODE_STAR_TOPO, 70, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 100_NODE_STAR_TOPO, 100, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 1000_NODE_STAR_TOPO, 1000, ClusterTopology::STAR);
#pragma endregion STAR

BENCHMARK_DRAW_LINE();

#pragma region LINEAR_WITH_EXISTINGKEY
BENCHMARK_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_0_Existing,
    100,
    ClusterTopology::LINEAR,
    0);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_10_Existing,
    100,
    ClusterTopology::LINEAR,
    10);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_50_Existing,
    100,
    ClusterTopology::LINEAR,
    50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_100_Existing,
    100,
    ClusterTopology::LINEAR,
    100);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_500_Existing,
    100,
    ClusterTopology::LINEAR,
    500);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_1000_Existing,
    1000,
    ClusterTopology::LINEAR,
    1000);
#pragma endregion LINEAR_WITH_EXISTINGKEY

BENCHMARK_DRAW_LINE();

#pragma region RING_WITH_EXISTINGKEY
BENCHMARK_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_0_Existing,
    100,
    ClusterTopology::RING,
    0);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_10_Existing,
    100,
    ClusterTopology::RING,
    10);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_50_Existing,
    100,
    ClusterTopology::RING,
    50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_100_Existing,
    100,
    ClusterTopology::RING,
    100);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_500_Existing,
    100,
    ClusterTopology::RING,
    500);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_1000_Existing,
    1000,
    ClusterTopology::RING,
    1000);
#pragma endregion RING_WITH_EXISTINGKEY

BENCHMARK_DRAW_LINE();

#pragma region STAR_WITH_EXISTINGKEY
BENCHMARK_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_0_Existing,
    100,
    ClusterTopology::STAR,
    0);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_10_Existing,
    100,
    ClusterTopology::STAR,
    10);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_50_Existing,
    100,
    ClusterTopology::STAR,
    50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_100_Existing,
    100,
    ClusterTopology::STAR,
    100);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_500_Existing,
    100,
    ClusterTopology::STAR,
    500);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_1000_Existing,
    1000,
    ClusterTopology::STAR,
    1000);
#pragma endregion STAR_WITH_EXISTINGKEY

BENCHMARK_DRAW_LINE();

int
main(int argc, char** argv) {
  facebook::initFacebook(&argc, &argv);
  folly::runBenchmarks();
  return 0;
};
