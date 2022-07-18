/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>

#if FOLLY_HAS_COROUTINES
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "common/init/Init.h"

#include <openr/kvstore/KvStoreUtil.h>
#include <openr/tests/utils/Utils.h>

#include <stdexcept>

using namespace openr;

FOLLY_INIT_LOGGING_CONFIG(
    ".=WARNING"
    ";default:async=true,sync_level=WARNING");

namespace {
const std::unordered_set<std::string> areaIds{kTestingAreaName};
} // namespace

void
runExperiment(
    uint32_t n,
    size_t nNodes,
    ClusterTopology topo,
    size_t nExistingKey = 0,
    size_t sizeOfKey = kSizeOfKey,
    size_t sizeOfVal = kSizeOfValue,
    OperationType operationType = OperationType::ADD_NEW_KEY) {
#pragma region ClusterSetup
  std::vector<std::unique_ptr<
      KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>>>
      kvStoreWrappers_;
  thrift::KeyVals events_;
  std::vector<std::pair<std::string, thrift::Value>> keyVals;

  BENCHMARK_SUSPEND {
    kvStoreWrappers_.reserve(nNodes);

    for (size_t i = 0; i < nNodes; i++) {
      thrift::KvStoreConfig kvStoreConfig;
      kvStoreConfig.node_name() = genNodeName(i);
      kvStoreWrappers_.emplace_back(
          std::make_unique<
              KvStoreWrapper<::apache::thrift::Client<thrift::KvStoreService>>>(
              areaIds, kvStoreConfig));
      kvStoreWrappers_.at(i)->run();
    }

    generateTopo(kvStoreWrappers_, topo);
#pragma endregion ClusterSetup

#pragma region ExistingKeySetup
    switch (operationType) {
    case OperationType::ADD_NEW_KEY:
    case OperationType::UPDATE_VERSION: {
      size_t nExist =
          operationType == OperationType::ADD_NEW_KEY ? nExistingKey : n;
      for (size_t i = 0; i < nExist; i++) {
        std::string key = genRandomStrWithPrefix("existingKey-", sizeOfKey);
        thrift::Value val = createThriftValue(
            1,
            kvStoreWrappers_.front()->getNodeId(),
            genRandomStrWithPrefix("existingVal-", sizeOfVal));
        kvStoreWrappers_.front()->setKey(kTestingAreaName, key, val);
        events_.emplace(std::move(key), std::move(val));
      }
      break;
    }
    }

    // Wait for existing key val to converge
    folly::coro::blockingWait(co_waitForConvergence(events_, kvStoreWrappers_));
#pragma endregion ExistingKeySetup

#pragma region EventSetup
    std::string nodeId = kvStoreWrappers_.front()->getNodeId();
    switch (operationType) {
    case OperationType::ADD_NEW_KEY: {
      keyVals.reserve(n);
      for (size_t i = 0; i < n; i++) {
        std::string key = genRandomStrWithPrefix("newKey-", sizeOfKey);
        thrift::Value val = createThriftValue(
            1, nodeId, genRandomStrWithPrefix("newVal-", sizeOfVal));
        events_.emplace(key, val);
        keyVals.emplace_back(std::move(key), std::move(val));
      }
      break;
    }
    case OperationType::UPDATE_VERSION: {
      for (auto& [key, val] : events_) {
        auto newVal =
            createThriftValue(*val.version_ref() + 1, nodeId, *val.value_ref());
        events_[key] = newVal;
        keyVals.emplace_back(key, newVal);
      }
      XCHECK_EQ(events_.size(), n);
      break;
    }
    }

#pragma endregion EventSetup
  } // end of BENCHMARK_SUSPEND

  kvStoreWrappers_.front()->setKeys(kTestingAreaName, keyVals);
  folly::coro::blockingWait(co_waitForConvergence(events_, kvStoreWrappers_));

#pragma region TearDown
  BENCHMARK_SUSPEND {
    // Need to explicity call destructor in suspend mode,
    // otherwise destruct time would be counted.
    // which could result in wrong benchmark result.
    kvStoreWrappers_.clear();
    events_.clear();
    keyVals.clear();
  }
#pragma endregion TearDown
}

#pragma region LINEAR
BENCHMARK_NAMED_PARAM(
    runExperiment,
    2_NODE_LINEAR_TOPO,
    /* nNodes = */ 2,
    ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_LINEAR_TOPO,
    /* nNodes = */ 10,
    ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    20_NODE_LINEAR_TOPO,
    /* nNodes = */ 20,
    ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    50_NODE_LINEAR_TOPO,
    /* nNodes = */ 50,
    ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    70_NODE_LINEAR_TOPO,
    /* nNodes = */ 70,
    ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO,
    /* nNodes = */ 100,
    ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    1000_NODE_LINEAR_TOPO,
    /* nNodes = */ 1000,
    ClusterTopology::LINEAR);
#pragma endregion LINEAR

BENCHMARK_DRAW_LINE();

#pragma region RING
BENCHMARK_NAMED_PARAM(
    runExperiment, 2_NODE_RING_TOPO, /* nNodes = */ 2, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 10_NODE_RING_TOPO, /* nNodes = */ 10, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 20_NODE_RING_TOPO, /* nNodes = */ 20, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 50_NODE_RING_TOPO, /* nNodes = */ 50, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 70_NODE_RING_TOPO, /* nNodes = */ 70, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO,
    /* nNodes = */ 100,
    ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    1000_NODE_RING_TOPO,
    /* nNodes = */ 1000,
    ClusterTopology::RING);
#pragma endregion RING

BENCHMARK_DRAW_LINE();

#pragma region STAR
BENCHMARK_NAMED_PARAM(
    runExperiment, 2_NODE_STAR_TOPO, /* nNodes = */ 2, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 10_NODE_STAR_TOPO, /* nNodes = */ 10, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 20_NODE_STAR_TOPO, /* nNodes = */ 20, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 50_NODE_STAR_TOPO, /* nNodes = */ 50, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 70_NODE_STAR_TOPO, /* nNodes = */ 70, ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO,
    /* nNodes = */ 100,
    ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    1000_NODE_STAR_TOPO,
    /* nNodes = */ 1000,
    ClusterTopology::STAR);
#pragma endregion STAR

BENCHMARK_DRAW_LINE();

#pragma region LINEAR_WITH_EXISTINGKEY
BENCHMARK_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_0_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::LINEAR,
    /* existingKey = */ 0);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_10_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::LINEAR,
    /* existingKey = */ 10);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_50_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::LINEAR,
    /* existingKey = */ 50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_100_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::LINEAR,
    /* existingKey = */ 100);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_500_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::LINEAR,
    /* existingKey = */ 500);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_LINEAR_TOPO_1000_EXISTING,
    /* nNodes = */ 1000,
    ClusterTopology::LINEAR,
    /* existingKey = */ 1000);
#pragma endregion LINEAR_WITH_EXISTINGKEY

BENCHMARK_DRAW_LINE();

#pragma region RING_WITH_EXISTINGKEY
BENCHMARK_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_0_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::RING,
    /* existingKey = */ 0);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_10_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::RING,
    /* existingKey = */ 10);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_50_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::RING,
    /* existingKey = */ 50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_100_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::RING,
    /* existingKey = */ 100);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_500_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::RING,
    /* existingKey = */ 500);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_RING_TOPO_1000_EXISTING,
    /* nNodes = */ 1000,
    ClusterTopology::RING,
    /* existingKey = */ 1000);
#pragma endregion RING_WITH_EXISTINGKEY

BENCHMARK_DRAW_LINE();

#pragma region STAR_WITH_EXISTINGKEY
BENCHMARK_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_0_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::STAR,
    /* existingKey = */ 0);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_10_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::STAR,
    /* existingKey = */ 10);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_50_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::STAR,
    /* existingKey = */ 50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_100_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::STAR,
    /* existingKey = */ 100);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_500_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::STAR,
    /* existingKey = */ 500);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    100_NODE_STAR_TOPO_1000_EXISTING,
    /* nNodes = */ 100,
    ClusterTopology::STAR,
    /* existingKey = */ 1000);
#pragma endregion STAR_WITH_EXISTINGKEY

BENCHMARK_DRAW_LINE();

#pragma region LINEAR_WITH_KEY_SIZE
BENCHMARK_NAMED_PARAM(
    runExperiment,
    10_NODE_LINEAR_TOPO_DEFAULT_KEYSIZE,
    /* nNodes = */ 10,
    ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_LINEAR_TOPO_5X_KEY_VAL_SIZE,
    /* nNodes = */ 10,
    ClusterTopology::LINEAR,
    /* existingKey = */ 0,
    /* keySize = */ 500,
    /* valSize = */ 50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_LINEAR_TOPO_10X_KEY_VAL_SIZE,
    /* nNodes = */ 10,
    ClusterTopology::LINEAR,
    /* existingKey = */ 0,
    /* keySize = */ 1000,
    /* valSize = */ 100);

BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_LINEAR_TOPO_100X_KEY_VAL_SIZE,
    /* nNodes = */ 10,
    ClusterTopology::LINEAR,
    /* existingKey = */ 0,
    /* keySize = */ 10000,
    /* valSize = */ 1000);

#pragma endregion LINEAR_WITH_KEY_SIZE

BENCHMARK_DRAW_LINE();

#pragma region RING_WITH_KEY_SIZE
BENCHMARK_NAMED_PARAM(
    runExperiment,
    10_NODE_RING_TOPO_DEFAULT_KEYSIZE,
    /* nNodes = */ 10,
    ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_RING_TOPO_5X_KEY_VAL_SIZE,
    /* nNodes = */ 10,
    ClusterTopology::RING,
    /* existingKey = */ 0,
    /* keySize = */ 500,
    /* valSize = */ 50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_RING_TOPO_10X_KEY_VAL_SIZE,
    /* nNodes = */ 10,
    ClusterTopology::RING,
    /* existingKey = */ 0,
    /* keySize = */ 1000,
    /* valSize = */ 100);

BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_RING_TOPO_100X_KEY_VAL_SIZE,
    /* nNodes = */ 10,
    ClusterTopology::RING,
    /* existingKey = */ 0,
    /* keySize = */ 10000,
    /* valSize = */ 1000);

#pragma endregion RING_WITH_KEY_SIZE

BENCHMARK_DRAW_LINE();

#pragma region STAR_WITH_KEY_SIZE
BENCHMARK_NAMED_PARAM(
    runExperiment,
    10_NODE_STAR_TOPO_DEFAULT_KEYSIZE,
    /* nNodes = */ 10,
    ClusterTopology::STAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_STAR_TOPO_5X_KEY_VAL_SIZE,
    /* nNodes = */ 10,
    ClusterTopology::STAR,
    /* existingKey = */ 0,
    /* keySize = */ 500,
    /* valSize = */ 50);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_STAR_TOPO_10X_KEY_VAL_SIZE,
    10,
    ClusterTopology::STAR,
    /* existingKey = */ 0,
    /* keySize = */ 1000,
    /* valSize = */ 100);

BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_STAR_TOPO_100X_KEY_VAL_SIZE,
    /* nNodes = */ 10,
    ClusterTopology::STAR,
    /* existingKey = */ 0,
    /* keySize = */ 10000,
    /* valSize = */ 1000);

#pragma endregion STAR_WITH_KEY_SIZE

BENCHMARK_DRAW_LINE();

#pragma region LINEAR_WITH_UPDATE_OPERATION
BENCHMARK_NAMED_PARAM(
    runExperiment,
    10_NODE_LINEAR_TOPO_DEFAULT_OPERATION,
    /* nNodes = */ 10,
    ClusterTopology::LINEAR,
    /* existingKey = */ 0,
    /* keySize = */ kSizeOfKey,
    /* valSize = */ kSizeOfValue);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_LINEAR_TOPO_UPDATE_OPERATION,
    /* nNodes = */ 10,
    ClusterTopology::LINEAR,
    /* existingKey = */ 0,
    /* keySize = */ kSizeOfKey,
    /* valSize = */ kSizeOfValue,
    /* operationType = */ OperationType::UPDATE_VERSION);

#pragma endregion LINEAR_WITH_UPDATE_OPERATION

BENCHMARK_DRAW_LINE();

#pragma region RING_WITH_UPDATE_OPERATION
BENCHMARK_NAMED_PARAM(
    runExperiment,
    10_NODE_RING_TOPO_DEFAULT_OPERATION,
    /* nNodes = */ 10,
    ClusterTopology::RING,
    /* existingKey = */ 0,
    /* keySize = */ kSizeOfKey,
    /* valSize = */ kSizeOfValue);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_RING_TOPO_UPDATE_OPERATION,
    /* nNodes = */ 10,
    ClusterTopology::RING,
    /* existingKey = */ 0,
    /* keySize = */ kSizeOfKey,
    /* valSize = */ kSizeOfValue,
    /* operationType = */ OperationType::UPDATE_VERSION);

#pragma endregion RING_WITH_UPDATE_OPERATION

BENCHMARK_DRAW_LINE();

#pragma region STAR_WITH_UPDATE_OPERATION
BENCHMARK_NAMED_PARAM(
    runExperiment,
    10_NODE_STAR_TOPO_DEFAULT_OPERATION,
    /* nNodes = */ 10,
    ClusterTopology::STAR,
    /* existingKey = */ 0,
    /* keySize = */ kSizeOfKey,
    /* valSize = */ kSizeOfValue);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment,
    10_NODE_STAR_TOPO_UPDATE_OPERATION,
    /* nNodes = */ 10,
    ClusterTopology::STAR,
    /* existingKey = */ 0,
    /* keySize = */ kSizeOfKey,
    /* valSize = */ kSizeOfValue,
    /* operationType = */ OperationType::UPDATE_VERSION);

#pragma endregion STAR_WITH_UPDATE_OPERATION

BENCHMARK_DRAW_LINE();

#endif

int
main(int argc, char** argv) {
  facebook::initFacebook(&argc, &argv);

#if FOLLY_HAS_COROUTINES
  folly::runBenchmarks();
#endif

  return 0;
};
