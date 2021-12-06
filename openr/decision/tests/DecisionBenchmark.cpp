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
 * @first param - integer: num of nodes in a grid topology
 * @second param - string: the forwarding algorithm
 * @third param - integer: num of prefixes per node
 * @fourth param - integer: num of updating prefixes per node
 *
 * Measures preformance of changing prefixes for a grid topology.
 * i.e. How long does it take to update a number of routes after
 * advertisemnet
 */
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_100_10_10,
    100,
    SP_ECMP,
    10,
    10);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_100_100_100,
    100,
    SP_ECMP,
    100,
    100);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_100_1k_1k,
    100,
    SP_ECMP,
    1000,
    1000);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_100_10k_1,
    100,
    SP_ECMP,
    10000,
    1);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_100_10k_10,
    100,
    SP_ECMP,
    10000,
    10);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_100_10k_100,
    100,
    SP_ECMP,
    10000,
    100);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_100_10k_1k,
    100,
    SP_ECMP,
    10000,
    1000);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_100_10k_10k,
    100,
    SP_ECMP,
    10000,
    10000);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    SP_ECMP_1000_1k_1k,
    1000,
    SP_ECMP,
    1000,
    1000);

BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_100_10_10,
    100,
    KSP2_ED_ECMP,
    10,
    10);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_100_100_100,
    100,
    KSP2_ED_ECMP,
    100,
    100);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_100_1k_1k,
    100,
    KSP2_ED_ECMP,
    1000,
    1000);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_100_10k_1,
    100,
    KSP2_ED_ECMP,
    10000,
    1);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_100_10k_10,
    100,
    KSP2_ED_ECMP,
    10000,
    10);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_100_10k_100,
    100,
    KSP2_ED_ECMP,
    10000,
    100);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_100_10k_1k,
    100,
    KSP2_ED_ECMP,
    10000,
    1000);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_100_10k_10k,
    100,
    KSP2_ED_ECMP,
    10000,
    10000);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionGridPrefixUpdates,
    counters,
    KSP2_ED_ECMP_1000_1k_1k,
    1000,
    KSP2_ED_ECMP,
    1000,
    1000);
/*
 * BM_DecisionFabricInitialUpdate:
 * @first param - integer: num of pods in a fabric topology
 * @second param - integer: num of planes in a fabric topology
 * @third param - integer: num of prefixes per node
 * @fourth param - string: type of the forwarding algorithm
 *
 * Measures preformance of initiating route build for a fabric topology.
 * i.e. How long does it take to estalish all routes given
 * `numberOfPrefixes` per node
 */
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionFabricInitialUpdate,
    counters,
    1_1_100_SP_ECMP,
    1,
    1,
    100,
    SP_ECMP);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionFabricInitialUpdate,
    counters,
    1_2_100_SP_ECMP,
    1,
    2,
    100,
    SP_ECMP);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionFabricInitialUpdate,
    counters,
    1_3_100_SP_ECMP,
    1,
    3,
    100,
    SP_ECMP);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionFabricInitialUpdate,
    counters,
    1_4_100_SP_ECMP,
    1,
    4,
    100,
    SP_ECMP);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionFabricInitialUpdate,
    counters,
    1_5_100_SP_ECMP,
    1,
    5,
    100,
    SP_ECMP);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionFabricInitialUpdate,
    counters,
    1_6_100_SP_ECMP,
    1,
    6,
    100,
    SP_ECMP);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionFabricInitialUpdate,
    counters,
    1_7_100_SP_ECMP,
    1,
    7,
    100,
    SP_ECMP);
BENCHMARK_COUNTERS_PARAM2(
    BM_DecisionFabricInitialUpdate,
    counters,
    1_8_100_SP_ECMP,
    1,
    8,
    100,
    SP_ECMP);
/*
 * BM_DecisionFabricPrefixUpdates:
 * @first param - integer: num of pods in a fabric topology
 * @second param - integer: num of planes in a fabric topology
 * @third param - integer: num of prefixes per node
 * @fourth param - integer: num of updating prefixes per node
 * @fifth param - string: name of the forwarding algorithm
 *
 * Measures preformance of incremental route build for a fabric topology.
 * i.e. How long does it take to incremental change routes given
 * `numOfUpdatePrefixes` per node
 *
 * total # of sws = numOfPlanes * kNumOfSswsPerPlane + numOfPods * numOfPlanes +
 *                 numOfPods * kNumOfRswsPerPod
 *                = 36 * numOfPlanes + numOfPods * numOfPlanes +
 *                 48 * kNumOfRswsPerPod
 */
// total = 85
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    1_1_100_10_SP_ECMP,
    1,
    1,
    100,
    100,
    SP_ECMP);
// total = 122
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    1_2_100_10_SP_ECMP,
    1,
    2,
    100,
    100,
    SP_ECMP);
// total = 159
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    1_3_100_10_SP_ECMP,
    1,
    3,
    100,
    100,
    SP_ECMP);
// total = 196
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    1_4_100_10_SP_ECMP,
    1,
    4,
    100,
    100,
    SP_ECMP);
// total = 233
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    1_5_100_10_SP_ECMP,
    1,
    5,
    100,
    100,
    SP_ECMP);
// total = 270
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    1_6_100_10_SP_ECMP,
    1,
    6,
    100,
    100,
    SP_ECMP);
// total = 307
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    1_7_100_10_SP_ECMP,
    1,
    7,
    100,
    100,
    SP_ECMP);
// total = 344
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    1_8_100_10_SP_ECMP,
    1,
    8,
    100,
    100,
    SP_ECMP);
// total = 526
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    10_1_100_10_SP_ECMP,
    10,
    1,
    100,
    100,
    SP_ECMP);
// total = 572
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    10_2_100_10_SP_ECMP,
    10,
    2,
    100,
    100,
    SP_ECMP);
// total = 618
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    10_3_100_10_SP_ECMP,
    10,
    3,
    100,
    100,
    SP_ECMP);
// total = 664
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    10_4_100_10_SP_ECMP,
    10,
    4,
    100,
    100,
    SP_ECMP);
// total = 710
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    10_5_100_10_SP_ECMP,
    10,
    5,
    100,
    100,
    SP_ECMP);
// total = 756
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    10_6_100_10_SP_ECMP,
    10,
    6,
    100,
    100,
    SP_ECMP);
// total = 802
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    10_7_100_10_SP_ECMP,
    10,
    7,
    100,
    100,
    SP_ECMP);
// total = 848
BENCHMARK_COUNTERS_PARAM3(
    BM_DecisionFabricPrefixUpdates,
    counters,
    10_8_100_10_SP_ECMP,
    10,
    8,
    100,
    100,
    SP_ECMP);
} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
