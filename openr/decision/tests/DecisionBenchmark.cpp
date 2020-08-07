/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <openr/decision/tests/RoutingBenchmarkUtils.h>

namespace openr {

// The integer parameter is the number of nodes in grid topology
BENCHMARK_COUNTERS_PARAM(BM_DecisionGrid, counters, 10, SP_ECMP);
BENCHMARK_COUNTERS_PARAM(BM_DecisionGrid, counters, 100, SP_ECMP);
BENCHMARK_COUNTERS_PARAM(BM_DecisionGrid, counters, 1000, SP_ECMP);
BENCHMARK_COUNTERS_PARAM(BM_DecisionGrid, counters, 10000, SP_ECMP);

BENCHMARK_COUNTERS_PARAM(BM_DecisionGrid, counters, 10, KSP2_ED_ECMP);
BENCHMARK_COUNTERS_PARAM(BM_DecisionGrid, counters, 100, KSP2_ED_ECMP);
BENCHMARK_COUNTERS_PARAM(BM_DecisionGrid, counters, 1000, KSP2_ED_ECMP);

// The integer parameter is numOfGivenNodes in topology,
// which >= numOfActualNodesInTopo.
// numOfPods = (numOfGivenNodes - numOfSsws) / numOfFswsAndRswsPerPod
// numOfActualNodesInTopo = numOfSsws + numOfPods * numOfFswsAndRswsPerPod
// The minimum number of switches of one pod =
// numOfPlanes * numOfSswsPerPlane + numOfPods * numOfFswsAndRswsPerPod = 344
BENCHMARK_COUNTERS_PARAM(BM_DecisionFabric, counters, 344, SP_ECMP);
BENCHMARK_COUNTERS_PARAM(BM_DecisionFabric, counters, 1000, SP_ECMP);
BENCHMARK_COUNTERS_PARAM(BM_DecisionFabric, counters, 5000, SP_ECMP);
} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
