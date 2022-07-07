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

} // namespace

void
runExperiment(uint32_t n, size_t nNodes) {
#pragma region Setup
  std::vector<
      std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>
      kvStoreWrappers_;
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
  }
#pragma endregion Setup

#pragma region TearDown
  BENCHMARK_SUSPEND {
    kvStoreWrappers_.clear();
  }
#pragma endregion TearDown
}

BENCHMARK_PARAM(runExperiment, 1)
BENCHMARK_RELATIVE_PARAM(runExperiment, 2)
BENCHMARK_RELATIVE_PARAM(runExperiment, 10)
BENCHMARK_RELATIVE_PARAM(runExperiment, 100)

int
main(int argc, char** argv) {
  facebook::initFacebook(&argc, &argv);
  folly::runBenchmarks();
  return 0;
};
