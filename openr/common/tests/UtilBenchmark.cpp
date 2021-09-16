/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include <openr/common/Util.h>

using namespace openr;

std::shared_ptr<thrift::PrefixEntry>
createPrefixEntry(size_t index) {
  auto entry = std::make_shared<thrift::PrefixEntry>();
  auto metrics = entry->metrics_ref();
  metrics->path_preference_ref() = index % 2;
  metrics->source_preference_ref() = index % 4;
  metrics->distance_ref() = index % 8;
  return entry;
}

void
BM_SelectRoutes(uint32_t iters, size_t size) {
  auto suspender = folly::BenchmarkSuspender();
  suspender.dismiss(); // Start measuring benchmark time

  while (iters--) {
    PrefixEntries entries;
    for (size_t i = 0; i < size; ++i) {
      entries.emplace(
          NodeAndArea{"test", fmt::format("node{}", i)}, createPrefixEntry(i));
    }
    selectRoutes(entries, thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE);
  } // while
}

BENCHMARK_PARAM(BM_SelectRoutes, 2);
BENCHMARK_PARAM(BM_SelectRoutes, 4);
BENCHMARK_PARAM(BM_SelectRoutes, 6);
BENCHMARK_PARAM(BM_SelectRoutes, 8);
BENCHMARK_PARAM(BM_SelectRoutes, 16);
BENCHMARK_PARAM(BM_SelectRoutes, 32);
BENCHMARK_PARAM(BM_SelectRoutes, 64);
BENCHMARK_PARAM(BM_SelectRoutes, 128);
BENCHMARK_PARAM(BM_SelectRoutes, 256);

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
