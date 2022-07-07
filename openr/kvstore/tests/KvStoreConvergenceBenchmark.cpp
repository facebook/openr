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
  // TODO: add more topo
};

void
generateTopo(
    std::vector<std::shared_ptr<
        KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>& stores,
    ClusterTopology topo) {
  switch (topo) {
  case ClusterTopology::LINEAR: {
    if (stores.empty()) {
      // no peers to connect
      return;
    }
    auto& prev = stores.front();
    for (size_t i = 1; i < stores.size(); i++) {
      auto cur = stores.at(i);
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
      prev = cur;
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
      std::shared_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>
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

BENCHMARK_NAMED_PARAM(runExperiment, ONE_LINEAR, 1, ClusterTopology::LINEAR)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, TWO_LINEAR, 2, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, TEN_LINEAR, 10, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, HUNDRED_LINEAR, 100, ClusterTopology::LINEAR);
BENCHMARK_RELATIVE_NAMED_PARAM(
    runExperiment, THOUSAND_LINEAR, 1000, ClusterTopology::LINEAR);

int
main(int argc, char** argv) {
  facebook::initFacebook(&argc, &argv);
  folly::runBenchmarks();
  return 0;
};
