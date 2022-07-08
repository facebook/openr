/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>

#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "common/init/Init.h"

#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>

#include <stdexcept>

using namespace openr;

FOLLY_INIT_LOGGING_CONFIG(
    ".=WARNING"
    ";default:async=true,sync_level=WARNING");

namespace {
const std::unordered_set<std::string> areaIds{kTestingAreaName};

std::string
genNodeName(size_t i) {
  return folly::to<std::string>("node-", i);
}

enum class ClusterTopology {
  LINEAR = 0,
  RING = 1,
  // TODO: add more topo
};

void
generateTopo(
    const std::vector<std::unique_ptr<
        KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>& kvStoreWrappers_,
    ClusterTopology topo) {
  switch (topo) {
    /*
     * Linear Topology Illustration:
     * 0 - 1 - 2 - 3 - 4 - 5 - 6 - 7
     */
  case ClusterTopology::LINEAR: {
    if (kvStoreWrappers_.empty()) {
      // no peers to connect
      return;
    }
    KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* prev =
        kvStoreWrappers_.front().get();
    for (size_t i = 1; i < kvStoreWrappers_.size(); i++) {
      KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* cur =
          kvStoreWrappers_.at(i).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
      prev = cur;
    }
    break;
  }
  /*
   * Ring Topology Illustration:
   *   1 - 3 - 5
   *  /         \
   * 0           7
   *  \         /
   *   2 - 4 - 6
   * This is designed such that the last node is the furthest from first node
   */
  case ClusterTopology::RING: {
    if (kvStoreWrappers_.size() <= 1) {
      // no peers to connect
      return;
    }
    for (size_t i = 1; i < kvStoreWrappers_.size(); i++) {
      KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* cur =
          kvStoreWrappers_.at(i).get();
      KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* prev =
          kvStoreWrappers_.at(i == 1 ? 0 : i - 2).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
    }
    if (kvStoreWrappers_.size() > 2) {
      KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* cur =
          kvStoreWrappers_.back().get();
      KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* prev =
          kvStoreWrappers_.at(kvStoreWrappers_.size() - 2).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
    }
    break;
  }
  default: {
    throw std::runtime_error("invalid topology type");
  }
  }
}
} // namespace

void
runExperiment(uint32_t n, size_t nNodes, ClusterTopology topo) {
#pragma region ClusterSetup
  std::vector<
      std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>
      kvStoreWrappers_;
  size_t eventCount_ = 0;
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

#pragma region EventSetup
    // TODO: make this mult-thread
    for (size_t i = 0; i < n; i++) {
      std::string key = folly::to<std::string>("key", i);
      auto val = createThriftValue(
          1,
          kvStoreWrappers_.front()->getNodeId(),
          folly::to<std::string>("value", i));
      kvStoreWrappers_.front()->setKey(kTestingAreaName, key, val);
      eventCount_++;
    }
#pragma endregion EventSetup
  }

  while (eventCount_ !=
         kvStoreWrappers_.back()->dumpAll(kTestingAreaName).size()) {
    // yield to avoid hogging the process
    std::this_thread::yield();
  }

#pragma region TearDown
  BENCHMARK_SUSPEND {
    kvStoreWrappers_.clear();
  }
#pragma endregion TearDown
}

#pragma region LINEAR
BENCHMARK_NAMED_PARAM(runExperiment, 2_LINEAR, 2, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 10_LINEAR, 10, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 100_LINEAR, 100, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 1000_LINEAR, 1000, ClusterTopology::LINEAR);
#pragma endregion LINEAR

BENCHMARK_DRAW_LINE();

#pragma region RING
BENCHMARK_NAMED_PARAM(runExperiment, 2_RING, 2, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 10_RING, 10, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 100_RING, 100, ClusterTopology::RING);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, 1000_RING, 1000, ClusterTopology::RING);
#pragma endregion RING

BENCHMARK_DRAW_LINE();

int
main(int argc, char** argv) {
  facebook::initFacebook(&argc, &argv);
  folly::runBenchmarks();
  return 0;
};
