/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/decision/tests/RoutingBenchmarkUtils.h>

namespace openr {

/*
 * BM_DecisionGridInitialUpdate:
 * measures preformance of initial KvStore publication for a grid topology.
 * i.e. How long does it take after startup for Decision to calculate all
 * routes
 */

// scale up topology, keeping prefix scale constant
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridInitialUpdate, counters, 10, SP_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridInitialUpdate, counters, 100, SP_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridInitialUpdate, counters, 1000, SP_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridInitialUpdate, counters, 10000, SP_ECMP, 1);
// scale up prefixes per node keeping topology size constant
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridInitialUpdate, counters, 100, SP_ECMP, 10);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridInitialUpdate, counters, 100, SP_ECMP, 100);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridInitialUpdate, counters, 100, SP_ECMP, 1000);

/*
 * BM_DecisionGridAdjUpdates:
 * measures preformance of processing adjacency changes for a grid topology.
 * i.e. How long does it take after an adjacecy db update for Decision to
 * recalulate all routes
 */

BENCHMARK_COUNTERS_PARAM(BM_DecisionGridAdjUpdates, counters, 10, SP_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(BM_DecisionGridAdjUpdates, counters, 100, SP_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(BM_DecisionGridAdjUpdates, counters, 1000, SP_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridAdjUpdates, counters, 10000, SP_ECMP, 1);

BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridAdjUpdates, counters, 10, KSP2_ED_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridAdjUpdates, counters, 100, KSP2_ED_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridAdjUpdates, counters, 1000, KSP2_ED_ECMP, 1);

/*
 * BM_DecisionGridPrefixUpdates:
 * measures preformance of a prefix change for a grid topology.
 * i.e. How long does it take to update a route for a prefix after an
 * advertisemnet or withdrawal from some node
 */

BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridPrefixUpdates, counters, 100, SP_ECMP, 10);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridPrefixUpdates, counters, 100, SP_ECMP, 100);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridPrefixUpdates, counters, 100, SP_ECMP, 1000);

BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridPrefixUpdates, counters, 100, KSP2_ED_ECMP, 10);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridPrefixUpdates, counters, 100, KSP2_ED_ECMP, 100);
BENCHMARK_COUNTERS_PARAM(
    BM_DecisionGridPrefixUpdates, counters, 100, KSP2_ED_ECMP, 1000);

// The integer parameter is numOfGivenNodes in topology,
// which >= numOfActualNodesInTopo.
// numOfPods = (numOfGivenNodes - numOfSsws) / numOfFswsAndRswsPerPod
// numOfActualNodesInTopo = numOfSsws + numOfPods * numOfFswsAndRswsPerPod
// The minimum number of switches of one pod =
// numOfPlanes * numOfSswsPerPlane + numOfPods * numOfFswsAndRswsPerPod = 344
BENCHMARK_COUNTERS_PARAM(BM_DecisionFabric, counters, 344, SP_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(BM_DecisionFabric, counters, 1000, SP_ECMP, 1);
BENCHMARK_COUNTERS_PARAM(BM_DecisionFabric, counters, 5000, SP_ECMP, 1);
} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
