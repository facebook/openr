/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define SpfSolver_TEST_FRIENDS \
  FRIEND_TEST(SpfSolverUnitTest, GetReachablePrefixEntriesTest);

#include <openr/common/LsdbUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/SpfSolver.h>
#include <openr/decision/tests/Consts.h>
#include <openr/decision/tests/DecisionTestUtils.h>
#include <openr/tests/utils/Utils.h>

using namespace ::testing;
using namespace ::std;
using namespace ::openr;

DEFINE_bool(stress_test, false, "pass this to run the stress test");

// DEPRECATED: utility functions provided for old test callsites that once used
// PrefixState::updatePrefixDatabase() expecting all node route advertisments to
// be synced.
//
// In newly written tests, prefer
// PrefixState::updatePrefix() and PrefixState::deletePrefix() for writing
// PrefixState::getReceivedRoutesFiltered() for reading

thrift::PrefixDatabase
getPrefixDbForNode(
    PrefixState const& state,
    std::string const& name,
    std::string const& area = kTestingAreaName) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName() = name;
  thrift::ReceivedRouteFilter filter;
  filter.nodeName() = name;
  filter.areaName() = area;
  for (auto const& routeDetail : state.getReceivedRoutesFiltered(filter)) {
    prefixDb.prefixEntries()->push_back(*routeDetail.routes()->at(0).route());
  }
  return prefixDb;
}

std::unordered_set<folly::CIDRNetwork>
updatePrefixDatabase(
    PrefixState& state,
    thrift::PrefixDatabase const& prefixDb,
    std::string const& area = kTestingAreaName) {
  auto const& nodeName = *prefixDb.thisNodeName();

  std::unordered_set<PrefixKey> oldKeys, newKeys;
  auto oldDb = getPrefixDbForNode(state, prefixDb.thisNodeName().value(), area);
  for (auto const& entry : *oldDb.prefixEntries()) {
    oldKeys.emplace(nodeName, toIPNetwork(*entry.prefix()), area);
  }
  std::unordered_set<folly::CIDRNetwork> changed;

  for (auto const& entry : *prefixDb.prefixEntries()) {
    PrefixKey key(nodeName, toIPNetwork(*entry.prefix()), area);
    changed.merge(state.updatePrefix(key, entry));
    newKeys.insert(std::move(key));
  }

  for (auto const& key : oldKeys) {
    if (not newKeys.count(key)) {
      changed.merge(state.deletePrefix(key));
    }
  }

  return changed;
}

RouteMap
getRouteMap(
    SpfSolver& spfSolver,
    const vector<string>& nodes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  RouteMap routeMap;

  for (string const& node : nodes) {
    auto routeDb = spfSolver.buildRouteDb(node, areaLinkStates, prefixState);
    if (not routeDb.has_value()) {
      continue;
    }

    fillRouteMap(node, routeMap, routeDb.value());
  }

  return routeMap;
}

const auto&
getUnicastNextHops(const thrift::UnicastRoute& r) {
  return *r.nextHops();
}

//
// Create a broken topology where R1 and R2 connect no one
// Expect no routes coming out of the spfSolver
//
TEST(ShortestPathTest, UnreachableNodes) {
  // no adjacency
  auto adjacencyDb1 = createAdjDb("1", {}, 0);
  auto adjacencyDb2 = createAdjDb("2", {}, 0);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable best route selection */,
      false /* disable v4_over_v6 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                   .topologyChanged);

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  for (string const& node : {"1", "2"}) {
    auto routeDb = spfSolver.buildRouteDb(node, areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(0, routeDb->unicastRoutes.size());
  }
}

/*
 * 1 - 2 - 3, 1 and 3 both originating same prefix
 * 3 originates higher/better metric than 1
 * 0) nothing drained, we should choose 3 (baseline)
 * Independent / separate scenarios
 * 1) Softdrain 3, we should choose 1
 * 2) HardDrain 3, we should choose 1
 * 3) Set drain_metric at 3, we should choose 1
 */
TEST(SpfSolver, DrainedNodeLeastPreferred) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 0);

  std::string nodeName("2");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
  linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
  linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);

  // Originate same prefix, pp=100/300, sp=100/300, d=0;
  const auto prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::CONFIG, createMetrics(100, 100, 0));
  auto prefixHighMetric = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::CONFIG, createMetrics(300, 300, 0));
  const auto localPrefixDb1 = createPrefixDb("1", {prefix});
  const auto localPrefixDb2 = createPrefixDb("2", {});
  const auto localPrefixDb3 = createPrefixDb("3", {prefixHighMetric});

  EXPECT_FALSE(updatePrefixDatabase(prefixState, localPrefixDb1).empty());
  EXPECT_TRUE(updatePrefixDatabase(prefixState, localPrefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, localPrefixDb3).empty());

  // 0) nothing drained, we should choose 3 (baseline)
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 3
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj23, false, *adj23.metric()), nh);
    // check that drain metric is not set, 3 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 1) Softdrain 3, we should choose 1
  adjacencyDb3.nodeMetricIncrementVal() = 100;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 3
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj21, false, *adj21.metric()), nh);
    // check that drain metric is not set, 1 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 2) HardDrain 3, we should choose 1
  adjacencyDb3.nodeMetricIncrementVal() = 0;
  adjacencyDb3.isOverloaded() = true;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 1
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj21, false, *adj21.metric()), nh);
    // check that drain metric is not set, 1 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 3) Set drain_metric at 3, we should choose 1
  adjacencyDb3.isOverloaded() = false;
  *prefixHighMetric.metrics()->drain_metric() = 1;
  updatePrefixDatabase(prefixState, createPrefixDb("3", {prefixHighMetric}));
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 1
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj21, false, *adj21.metric()), nh);
    // check that drain metric is not set, 1 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }
}

//
// R1 and R2 are adjacent, and R1 has this declared in its
// adjacency database. However, R1 is missing the AdjDb from
// R2. It should not be able to compute path to R2 in this case.
//
TEST(ShortestPathTest, MissingNeighborAdjacencyDb) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable best route selection */,
      false /* disable v4_over_v6 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(0, routeDb->unicastRoutes.size());
}

//
// R1 and R2 are adjacent, and R1 has this declared in its
// adjacency database. R1 received AdjacencyDatabase from R2,
// but it missing adjacency to R1. We should not see routes
// from R1 to R2.
//
TEST(ShortestPathTest, EmptyNeighborAdjacencyDb) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);
  auto adjacencyDb2 = createAdjDb("2", {}, 0);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable best route selection */,
      false /* disable v4_over_v6 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                   .topologyChanged);
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  // dump routes for both nodes, expect no routing entries

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(0, routeDb->unicastRoutes.size());

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(0, routeDb->unicastRoutes.size());
}

//
// Query route for unknown neighbor. It should return none
//
TEST(ShortestPathTest, UnknownNode) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable best route selection */,
      false /* disable v4_over_v6 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  PrefixState prefixState;

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  EXPECT_FALSE(routeDb.has_value());

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  EXPECT_FALSE(routeDb.has_value());
}

/*
 * 1 - 2 - 3, 1 and 3 both originating same prefix
 * 1) 1 is softdrained(50), 2 will reach prefix via 3
 * 2) both 1, 3 softdrained(50), 2 will reach prefix via both
 * 3) drain 1 with 100, 2 will reach via 3
 * 4) undrain 1, 2 will reach via 1
 */

TEST(SpfSolver, NodeSoftDrainedChoice) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 0);

  std::string nodeName("2");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable best route selection */,
      false /* disable v4_over_v6 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1, R2, R3 adjacency + prefix dbs
  //
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }

  // Originate same prefix
  const auto prefix1 = createPrefixEntry(addr1, thrift::PrefixType::CONFIG);
  const auto localPrefixDb1 = createPrefixDb("1", {prefix1});
  const auto localPrefixDb2 = createPrefixDb("2", {});
  const auto localPrefixDb3 = createPrefixDb("3", {prefix1});

  EXPECT_FALSE(updatePrefixDatabase(prefixState, localPrefixDb1).empty());
  EXPECT_TRUE(updatePrefixDatabase(prefixState, localPrefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, localPrefixDb3).empty());

  const unsigned int nodeIncVal50 = 50;
  const unsigned int nodeIncVal100 = 100;

  // 1] Soft Drain 1; 2 should only have one nexthop
  adjacencyDb1.nodeMetricIncrementVal() = nodeIncVal50;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 3
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj23, false, *adj23.metric()), nh);
    // check that drain metric is not set, 3 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 2] Soft Drain 3, now both 1 and 3 are drained; 2 should have two nexthop
  adjacencyDb3.nodeMetricIncrementVal() = nodeIncVal50;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check two nexthop (ecmp to both drained)
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(2, ribEntry.nexthops.size());
    // check that drain metric is set
    EXPECT_EQ(1, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 3] soft Drain 1 harder (100), 2 will still have both next hop.
  adjacencyDb1.nodeMetricIncrementVal() = nodeIncVal100;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to 3
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(2, ribEntry.nexthops.size());
    // check that drain metric is set
    EXPECT_EQ(1, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 3] undrain 1, 3 is still softdrained. Will choose 1
  adjacencyDb1.nodeMetricIncrementVal() = 0;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj21, false, *adj21.metric()), nh);
    // check that drain metric is not set
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }
}

/*
 * 1-2-3, where both 1 and 3 advertise same prefix but 1 is overloaded.
 * 1 and 2 will choose only 3 (despite 1 advertising the prefix itself)
 * 3 will choose itself
 */
TEST(SpfSolver, NodeOverloadRouteChoice) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable best route selection */,
      false /* disable v4_over_v6 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1, R2, R3 adjacency + prefix dbs
  //
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged); // label changed for node1
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }

  // Originate same prefix differently
  const auto prefix1 = createPrefixEntry(addr1, thrift::PrefixType::CONFIG);
  const auto prefix3 = createPrefixEntry(addr1, thrift::PrefixType::VIP);
  const auto localPrefixDb1 = createPrefixDb("1", {prefix1});
  const auto localPrefixDb2 = createPrefixDb("2", {});
  const auto localPrefixDb3 = createPrefixDb("3", {prefix3});

  EXPECT_FALSE(updatePrefixDatabase(prefixState, localPrefixDb1).empty());
  EXPECT_TRUE(updatePrefixDatabase(prefixState, localPrefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, localPrefixDb3).empty());

  //
  // dump routes for all nodes. expect one unicast route, no overload
  //
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check two nexthop
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(2, ribEntry.nexthops.size());
  }
  {
    auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(0, routeDb->unicastRoutes.size()); // self originated
  }
  {
    auto routeDb = spfSolver.buildRouteDb("3", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(0, routeDb->unicastRoutes.size()); // self originated
  }

  // Overload node 1
  adjacencyDb1.isOverloaded() = true;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_FALSE(res.nodeLabelChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check two nexthop
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
  }
  {
    auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    // Not choosing itself even it originates this prefix
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(prefix3, ribEntry.bestPrefixEntry);
    // let others know that local route has been considered when picking the
    // route (and lost)
    EXPECT_TRUE(ribEntry.localRouteConsidered);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("3", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(0, routeDb->unicastRoutes.size()); // self originated
  }
}

/**
 * Test to verify adjacencyDatabase update
 */
TEST(SpfSolver, AdjacencyUpdate) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 2);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable best route selection */,
      false /* disable v4_over_v6 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1 and R2's adjacency + prefix dbs
  //

  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged); // label changed for node1
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  //
  // dump routes for both nodes, expect 3 route entries (1 unicast, 2 label) on
  // each (node1-label, node2-label)
  //
  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());

  //
  // Update adjacency database of node 1 by changing it's nexthops and verift
  // that update properly responds to the event
  //
  adjacencyDb1.adjacencies()[0].nextHopV6() =
      toBinaryAddress("fe80::1234:b00c");
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.linkAttributesChanged);
  }

  //
  // dump routes for both nodes, expect 3 route entries (1 unicast, 2 label) on
  // each (node1-label, node2-label)
  //

  routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());

  //
  // Update adjacency database of node 2 by changing it's nexthops and verift
  // that update properly responds to the event (no spf trigger needed)
  //
  *adjacencyDb2.adjacencies()[0].nextHopV6() =
      toBinaryAddress("fe80::5678:b00c");
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.linkAttributesChanged);
  }

  //
  // dump routes for both nodes, expect 3 route entries (1 unicast, 2 label) on
  // each (node1-label, node2-label)
  //

  routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
}

/**
 * node1 connects to node2 and node3. Both are same distance away (10). Both
 * node2 and node3 announces prefix1 with same metric vector. Routes for prefix1
 * is inspected on node1 at each step. Test outline follows
 *
 * 1) prefix1 -> {node2, node3}
 * 2) Increase cost towards node3 to 20; prefix -> {node2}
 * 3) mark link towards node2 as drained; prefix1 -> {node3}
 * 3) Set cost towards node2 to 20 (still drained); prefix1 -> {node3}
 * 4) Undrain link; prefix1 -> {node2, node3}
 */
TEST(BGPRedistribution, IgpMetric) {
  const std::string data1{"data1"};
  const auto expectedAddr = addr1;
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* enableV4 */, true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  //
  // Create BGP prefix
  //
  const auto bgpPrefix2 = createPrefixEntry(
      addr1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP);
  const auto bgpPrefix3 = createPrefixEntry(
      addr1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP);

  //
  // Setup adjacencies
  //
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  //
  // Update prefix databases
  //
  auto prefixDb2WithBgp =
      createPrefixDb("2", {createPrefixEntry(addr2), bgpPrefix2});
  auto prefixDb3WithBgp =
      createPrefixDb("3", {createPrefixEntry(addr3), bgpPrefix3});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2WithBgp).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3WithBgp).empty());

  //
  // Step-1 prefix1 -> {node2, node3}
  //
  auto decisionRouteDb =
      *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  auto routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10),
                  createNextHopFromAdj(adj13, false, 10))))));

  //
  // Increase cost towards node3 to 20; prefix -> {node2}
  //
  adjacencyDb1.adjacencies()[1].metric() = 20;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10))))));

  //
  // mark link towards node2 as drained; prefix1 -> {node3}
  // No route towards addr2 (node2's loopback)
  //
  adjacencyDb1.adjacencies()[0].isOverloaded() = true;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();

  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(2));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj13, false, 20))))));

  //
  // Set cost towards node2 to 20 (still drained); prefix1 -> {node3}
  // No route towards addr2 (node2's loopback)
  //
  adjacencyDb1.adjacencies()[0].metric() = 20;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(2));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj13, false, 20))))));

  //
  // Undrain link; prefix1 -> {node2, node3}
  //
  adjacencyDb1.adjacencies()[0].isOverloaded() = false;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 20),
                  createNextHopFromAdj(adj13, false, 20))))));
}

TEST(Decision, IgpCost) {
  std::string nodeName("1");
  const auto expectedAddr = addr1;
  SpfSolver spfSolver(
      nodeName, false /* enableV4 */, true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;

  // Test topology: spine
  // Setup adjacencies: note each link cost is 10
  // 1     4 (SSW)
  // |  x  |
  // 2     3 (FSW)

  // Setup adjacency
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj24}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj31, adj34}, 3);
  auto adjacencyDb4 = createAdjDb("4", {adj42, adj43}, 4);
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb4, kTestingAreaName)
                  .topologyChanged);

  // Setup prefixes. node2 annouces the prefix
  const auto node2Prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 0, 0));
  EXPECT_FALSE(
      updatePrefixDatabase(prefixState, createPrefixDb("2", {node2Prefix}))
          .empty());

  // Case-1 node1 route to 2 with direct link: igp cost = 1 * 10
  {
    auto decisionRouteDb =
        *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
    auto route = decisionRouteDb.unicastRoutes.at(toIPNetwork(expectedAddr));
    EXPECT_EQ(route.igpCost, 10);
  }

  // Case-2 link 21 broken, node1 route to 2 (1->3->4->2): igp cost = 3 * 10
  {
    auto newAdjacencyDb2 = createAdjDb("2", {adj24}, 4);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(newAdjacencyDb2, kTestingAreaName)
            .topologyChanged);
    auto decisionRouteDb =
        *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
    auto route = decisionRouteDb.unicastRoutes.at(toIPNetwork(expectedAddr));
    EXPECT_EQ(route.igpCost, 30);
  }
}

TEST(Decision, BestRouteSelection) {
  std::string nodeName("1");
  const auto expectedAddr = addr1;
  SpfSolver spfSolver(
      nodeName, false /* enableV4 */, true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;

  //
  // Setup adjacencies
  // 2 <--> 1 <--> 3
  //
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 3);
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  //
  // Setup prefixes. node2 and node3 announces the same prefix with same metrics
  // and different types. The type shouldn't have any effect on best route
  // selection.
  //
  const auto node2Prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 0, 0));
  const auto node3Prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::BGP, createMetrics(200, 0, 0));
  EXPECT_FALSE(
      updatePrefixDatabase(prefixState, createPrefixDb("2", {node2Prefix}))
          .empty());
  EXPECT_FALSE(
      updatePrefixDatabase(prefixState, createPrefixDb("3", {node3Prefix}))
          .empty());

  //
  // Verifies that best routes cache is empty
  //
  EXPECT_TRUE(spfSolver.getBestRoutesCache().empty());

  //
  // Case-1 node1 ECMP towards {node2, node3}
  //
  auto decisionRouteDb =
      *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  auto routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(1));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10),
                  createNextHopFromAdj(adj13, false, 10))))));

  //
  // Verify that prefix-state report two best routes
  //
  {
    auto bestRoutesCache = spfSolver.getBestRoutesCache();
    ASSERT_EQ(1, bestRoutesCache.count(toIPNetwork(addr1)));
    auto& bestRoutes = bestRoutesCache.at(toIPNetwork(addr1));
    EXPECT_EQ(2, bestRoutes.allNodeAreas.size());
    EXPECT_EQ(1, bestRoutes.allNodeAreas.count({"2", kTestingAreaName}));
    EXPECT_EQ(1, bestRoutes.allNodeAreas.count({"3", kTestingAreaName}));
    EXPECT_EQ("2", bestRoutes.bestNodeArea.first);
  }

  //
  // Case-2 node1 prefers node2 (prefix metrics)
  //
  const auto node2PrefixPreferred = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 100, 0));
  EXPECT_FALSE(updatePrefixDatabase(
                   prefixState, createPrefixDb("2", {node2PrefixPreferred}))
                   .empty());

  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(1));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10))))));
  //
  // Verify that prefix-state report only one best route
  //
  {
    auto bestRoutesCache = spfSolver.getBestRoutesCache();
    ASSERT_EQ(1, bestRoutesCache.count(toIPNetwork(addr1)));
    auto& bestRoutes = bestRoutesCache.at(toIPNetwork(addr1));
    EXPECT_EQ(1, bestRoutes.allNodeAreas.size());
    EXPECT_EQ(1, bestRoutes.allNodeAreas.count({"2", kTestingAreaName}));
    EXPECT_EQ("2", bestRoutes.bestNodeArea.first);
  }
}

//
// Test topology:
// connected bidirectionally
//  1 <----> 2 <----> 3
// partitioned
//  1 <----  2  ----> 3
//
class ConnectivityTest : public ::testing::TestWithParam<bool> {};

TEST_P(ConnectivityTest, GraphConnectedOrPartitioned) {
  auto partitioned = GetParam();

  auto adjacencyDb1 = createAdjDb("1", {}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {}, 3);
  if (!partitioned) {
    adjacencyDb1 = createAdjDb("1", {adj12}, 1);
    adjacencyDb3 = createAdjDb("3", {adj32}, 3);
  }

  std::string nodeName("1");
  SpfSolver spfSolver(nodeName, false /* disable v4 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName));
  EXPECT_EQ(
      LinkState::LinkStateChange(!partitioned, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName));
  EXPECT_EQ(
      LinkState::LinkStateChange(!partitioned, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName));

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  // route from 1 to 3
  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  bool foundRouteV6 = false;
  if (routeDb.has_value()) {
    for (auto const& [routePrefix, _] : routeDb->unicastRoutes) {
      if (toIpPrefix(routePrefix) == addr3) {
        foundRouteV6 = true;
        break;
      }
    }
  }

  EXPECT_EQ(partitioned, !foundRouteV6);
}

INSTANTIATE_TEST_CASE_P(
    PartitionedTopologyInstance, ConnectivityTest, ::testing::Bool());

//
// Overload node test in a linear topology with shortest path calculation
//
// 1<--->2<--->3
//   10     10
//
TEST(ConnectivityTest, NodeHardDrainTest) {
  std::string nodeName("1");
  SpfSolver spfSolver(nodeName, false /* disable v4 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  // Make node-2 overloaded
  adjacencyDb2.isOverloaded() = true;

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  auto routeMap =
      getRouteMap(spfSolver, {"1", "2", "3"}, areaLinkStates, prefixState);

  /*
   * We only expect 4 unicast routes because node-1 and
   * node-3 are disconnected.
   *
   * node-1 => node-2 (unicast)
   * node-2 => node-1, node-3 (unicast)
   * node-3 => node-2 (unicast)
   */
  EXPECT_EQ(4, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));
}

/*
 * Interface soft-drain test will mimick the soft-drain behavior to change
 * adj metric on one side, aka, uni-directionally. The test will verify both
 * ends of the link will react to this drain behavior and change SPF calculation
 * result accordinly.
 *
 * The test forms a circle topology for SPF calculation.
 *
 *         20       10
 *     1<------>2<------>3(new)
 *     ^   10       10   ^
 *     |                 |
 *     |        10       |
 *     |-----------------|
 *              10
 */
TEST(ConnectivityTest, InterfaceSoftDrainTest) {
  const std::string nodeName("1");
  SpfSolver spfSolver(nodeName, false /* disable v4 */);

  // Initialize link-state and prefix-state obj
  std::unordered_map<std::string, LinkState> areaLinkStates = {
      {kTestingAreaName, LinkState(kTestingAreaName, nodeName)}};
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  /*
   * Create adjacency DBs with:
   *
   * node1 -> {node2(metric = 10)}
   * node2 -> {node1(metric = 10), node3(metric = 10)}
   * node3 -> {node1(metric = 10), node2(metric = 10)}
   */
  auto adjacencyDb1 = createAdjDb("1", {adj12_1}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32, adj31_old}, 3);

  {
    EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
    EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
    EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

    // No bi-directional adjacencies yet. No topo change.
    EXPECT_FALSE(
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
            .topologyChanged);
    // node2 <-> node3 has bi-directional adjs. Expect topo change.
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
            .topologyChanged);
    // node1 <-> node2 has bi-directional adjs. Expect topo change.
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
            .topologyChanged);
  }

  /*
   * add/update adjacency of node1 with old versions
   * node1 -> {node2(metric = 20), node3(metric = 10)}
   * node2 -> {node1(metric = 10), node3(metric = 10)}
   * node3 -> {node1(metric = 10), node2(metric = 10)}
   */
  {
    // Update adjDb to add node1 -> node3 to form bi-dir adj. Expect topo
    // change.
    auto adjDb1 = createAdjDb("1", {adj12_1, adj13}, 1);
    EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjDb1, kTestingAreaName)
                    .topologyChanged);
    // Update adjDb1 to increase node1 -> node2 metric. Expect topo change.
    adjDb1 = createAdjDb("1", {adj12_2, adj13}, 1);
    EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjDb1, kTestingAreaName)
                    .topologyChanged);
  }

  auto routeMap =
      getRouteMap(spfSolver, {"1", "2", "3"}, areaLinkStates, prefixState);

  /*
   * We only expect 6 unicast routes
   * node-1 => node-2, node-3
   * node-2 => node-1, node-3
   * node-3 => node-2, node-1
   */
  EXPECT_EQ(6, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 20),
           createNextHopFromAdj(adj13, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));
  // SPF will choose the max metric between node1 and node2. Hence create ECMP
  // towards node1 and node3
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj21, false, 20),
           createNextHopFromAdj(adj23, false, 20)}));

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // adjacency update (remove adjacency) for node1
  adjacencyDb1 = createAdjDb("1", {adj12_2}, 0);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  adjacencyDb3 = createAdjDb("3", {adj32}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                   .topologyChanged);

  adjacencyDb1 = createAdjDb("1", {adj12_2, adj13}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
}

//
// Test topology:
//
//  1------2
//  | \     |
//  |   \   |
//  3------4
//
// Test both IP v4 & v6
// 1,2,3,4 are simply meshed with each other with 1 parallet links
//
class SimpleRingMeshTopologyFixture
    : public ::testing::TestWithParam<
          std::tuple<bool, std::optional<thrift::PrefixType>>> {
 public:
  SimpleRingMeshTopologyFixture() : v4Enabled(std::get<0>(GetParam())) {}

 protected:
  void
  CustomSetUp() {
    std::string nodeName("1");
    spfSolver = std::make_unique<SpfSolver>(nodeName, v4Enabled);
    adjacencyDb1 = createAdjDb("1", {adj12, adj13, adj14}, 1);
    adjacencyDb2 = createAdjDb("2", {adj21, adj23, adj24}, 2);
    adjacencyDb3 = createAdjDb("3", {adj31, adj32, adj34}, 3);
    adjacencyDb4 = createAdjDb("4", {adj41, adj42, adj43}, 4);

    areaLinkStates.emplace(
        kTestingAreaName, LinkState(kTestingAreaName, nodeName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);

    EXPECT_EQ(
        LinkState::LinkStateChange(false, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb4, kTestingAreaName));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;

    auto bgp1 = v4Enabled ? bgpAddr1V4 : bgpAddr1;
    auto bgp2 = v4Enabled ? bgpAddr2V4 : bgpAddr2;
    auto bgp3 = v4Enabled ? bgpAddr3V4 : bgpAddr3;
    auto bgp4 = v4Enabled ? bgpAddr4V4 : bgpAddr4;

    updatePrefixDatabase(prefixState, pdb1);
    updatePrefixDatabase(prefixState, pdb2);
    updatePrefixDatabase(prefixState, pdb3);
    updatePrefixDatabase(prefixState, pdb4);
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  std::unique_ptr<SpfSolver> spfSolver;
  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;
};

//
// Test topology:
//
//  1------2
//  |      |
//  |      |
//  3------4
//
// Test both IP v4 & v6
//
class SimpleRingTopologyFixture
    : public ::testing::TestWithParam<
          std::tuple<bool, std::optional<thrift::PrefixType>>> {
 public:
  SimpleRingTopologyFixture() : v4Enabled(std::get<0>(GetParam())) {}

 protected:
  void
  CustomSetUp() {
    std::string nodeName("1");
    spfSolver = std::make_unique<SpfSolver>(nodeName, v4Enabled);
    adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
    adjacencyDb2 = createAdjDb("2", {adj21, adj24}, 2);
    adjacencyDb3 = createAdjDb("3", {adj31, adj34}, 3);
    adjacencyDb4 = createAdjDb("4", {adj42, adj43}, 4);

    areaLinkStates.emplace(
        kTestingAreaName, LinkState(kTestingAreaName, nodeName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);

    EXPECT_EQ(
        LinkState::LinkStateChange(false, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb4, kTestingAreaName));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;

    auto bgp1 = v4Enabled ? bgpAddr1V4 : bgpAddr1;
    auto bgp2 = v4Enabled ? bgpAddr2V4 : bgpAddr2;
    auto bgp3 = v4Enabled ? bgpAddr3V4 : bgpAddr3;
    auto bgp4 = v4Enabled ? bgpAddr4V4 : bgpAddr4;

    updatePrefixDatabase(prefixState, pdb1);
    updatePrefixDatabase(prefixState, pdb2);
    updatePrefixDatabase(prefixState, pdb3);
    updatePrefixDatabase(prefixState, pdb4);
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  std::unique_ptr<SpfSolver> spfSolver;
  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;

  void
  verifyRouteInUpdateNoDelete(
      std::string nodeName, int32_t mplsLabel, const DecisionRouteDb& compDb) {
    // verify route DB change in node 1.
    auto deltaRoutes = compDb.calculateUpdate(
        spfSolver->buildRouteDb(nodeName, areaLinkStates, prefixState).value());

    EXPECT_EQ(deltaRoutes.mplsRoutesToUpdate.count(mplsLabel), 1);
    EXPECT_EQ(deltaRoutes.mplsRoutesToDelete.size(), 0);
  }
};

INSTANTIATE_TEST_CASE_P(
    SimpleRingTopologyInstance,
    SimpleRingTopologyFixture,
    ::testing::Values(
        std::make_tuple(true, std::nullopt),
        std::make_tuple(false, std::nullopt),
        std::make_tuple(true, thrift::PrefixType::BGP),
        std::make_tuple(false, thrift::PrefixType::BGP)));

/*
 * Verify SpfSolver finds the shortest path
 */
TEST_P(SimpleRingTopologyFixture, ShortestPathTest) {
  CustomSetUp();
  fb303::fbData->resetAllData();
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  EXPECT_EQ(12, routeMap.size());

  // validate router 1
  const auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.spf_runs.count"), 4);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops(
          {createNextHopFromAdj(adj12, v4Enabled, 20),
           createNextHopFromAdj(adj13, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops(
          {createNextHopFromAdj(adj21, v4Enabled, 20),
           createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 20),
           createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops(
          {createNextHopFromAdj(adj42, v4Enabled, 20),
           createNextHopFromAdj(adj43, v4Enabled, 20)}));
}

//
// Use the same topology, but test multi-path routing
//
TEST_P(SimpleRingTopologyFixture, MultiPathTest) {
  CustomSetUp();
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  EXPECT_EQ(12, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops(
          {createNextHopFromAdj(adj12, v4Enabled, 20),
           createNextHopFromAdj(adj13, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops(
          {createNextHopFromAdj(adj21, v4Enabled, 20),
           createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 20),
           createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops(
          {createNextHopFromAdj(adj42, v4Enabled, 20),
           createNextHopFromAdj(adj43, v4Enabled, 20)}));
}

//
// attach nodes to outside world, e.g., POP
// verify all non-POP nodes find their closest POPs
//
TEST_P(SimpleRingTopologyFixture, AttachedNodesTest) {
  CustomSetUp();
  // Advertise default prefixes from node-1 and node-4
  auto defaultRoutePrefix = v4Enabled ? "0.0.0.0/0" : "::/0";
  auto defaultRoute = toIpPrefix(defaultRoutePrefix);
  auto localPrefixDb1 = createPrefixDb(
      "1", {createPrefixEntry(addr1), createPrefixEntry(defaultRoute)});
  auto localPrefixDb4 = createPrefixDb(
      "4", {createPrefixEntry(addr4), createPrefixEntry(defaultRoute)});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, localPrefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, localPrefixDb4).empty());

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) + 2 (default routes) = 14
  EXPECT_EQ(14, routeMap.size());

  // validate router 1
  // no default route boz it's attached
  // i.e., spfSolver(false), bcoz we set node 1 to be "1" distance away from the
  // dummy node and its neighbors are all further away, thus there is no route
  // to the dummy node
  EXPECT_EQ(0, routeMap.count({"1", defaultRoutePrefix}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", defaultRoutePrefix)],
      NextHops(
          {createNextHopFromAdj(adj21, v4Enabled, 10),
           createNextHopFromAdj(adj24, v4Enabled, 10)}));

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", defaultRoutePrefix)],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 10),
           createNextHopFromAdj(adj34, v4Enabled, 10)}));

  // validate router 4
  // no default route boz it's attached
  EXPECT_EQ(0, routeMap.count({"4", defaultRoutePrefix}));
}

//
// Verify overload bit setting of a node's adjacency DB with multipath
// enabled. Make node-3 and node-2 overloaded and verify routes.
// It will disconnect node-1 with node-4 but rests should be reachable
//
TEST_P(SimpleRingTopologyFixture, OverloadNodeTest) {
  CustomSetUp();
  adjacencyDb2.isOverloaded() = true;
  adjacencyDb3.isOverloaded() = true;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 2 + 3 + 3 + 2 = 10
  EXPECT_EQ(10, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)})); // No LFA
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops(
          {createNextHopFromAdj(adj21, v4Enabled, 20),
           createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 20),
           createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
}

//
// Verify overload bit setting of individual adjacencies with multipath
// enabled. node-3 will get disconnected
//
TEST_P(SimpleRingTopologyFixture, OverloadLinkTest) {
  CustomSetUp();
  adjacencyDb3.adjacencies()[0].isOverloaded() = true; // make adj31 overloaded
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  EXPECT_EQ(12, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 30)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));

  // validate router 3
  // no routes for router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 30)}));

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20)}));

  // Now also make adj34 overloaded which will disconnect the node-3
  adjacencyDb3.adjacencies()[1].isOverloaded() = true;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 2 + 2 + 0 + 2 = 6
  EXPECT_EQ(6, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));

  // validate router 3 - nothing to validate

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20)}));
}

/*
 * Test topology: ring with parallel adjacencies, *x* denotes metric
 *    ---*11*---
 *   /          \
 *  1----*11*----2
 *  |\          /|
 *  | ---*20*--- |
 * *11*         *11*
 *  |            |
 *  | ---*11*--- |
 *  |/          \|
 *  3----*20*----4
 *  \          /
 *   ---*20*---
 */
class ParallelAdjRingTopologyFixture
    : public ::testing::TestWithParam<std::optional<thrift::PrefixType>> {
 public:
  ParallelAdjRingTopologyFixture() = default;

 protected:
  void
  CustomSetUp() {
    std::string nodeName("1");
    spfSolver = std::make_unique<SpfSolver>(nodeName, false);
    // R1 -> R2
    adj12_1 =
        createAdjacency("2", "2/1", "1/1", "fe80::2:1", "192.168.2.1", 11, 201);
    adj12_2 =
        createAdjacency("2", "2/2", "1/2", "fe80::2:2", "192.168.2.2", 11, 202);
    adj12_3 =
        createAdjacency("2", "2/3", "1/3", "fe80::2:3", "192.168.2.3", 20, 203);
    // R1 -> R3
    adj13_1 =
        createAdjacency("3", "3/1", "1/1", "fe80::3:1", "192.168.3.1", 11, 301);

    // R2 -> R1
    adj21_1 =
        createAdjacency("1", "1/1", "2/1", "fe80::1:1", "192.168.1.1", 11, 101);
    adj21_2 =
        createAdjacency("1", "1/2", "2/2", "fe80::1:2", "192.168.1.2", 11, 102);
    adj21_3 =
        createAdjacency("1", "1/3", "2/3", "fe80::1:3", "192.168.1.3", 20, 103);
    // R2 -> R4
    adj24_1 =
        createAdjacency("4", "4/1", "2/1", "fe80::4:1", "192.168.4.1", 11, 401);

    // R3 -> R1
    adj31_1 =
        createAdjacency("1", "1/1", "3/1", "fe80::1:1", "192.168.1.1", 11, 101);
    // R3 -> R4
    adj34_1 =
        createAdjacency("4", "4/1", "3/1", "fe80::4:1", "192.168.4.1", 11, 401);
    adj34_2 =
        createAdjacency("4", "4/2", "3/2", "fe80::4:2", "192.168.4.2", 20, 402);
    adj34_3 =
        createAdjacency("4", "4/3", "3/3", "fe80::4:3", "192.168.4.3", 20, 403);

    // R4 -> R2
    adj42_1 =
        createAdjacency("2", "2/1", "4/1", "fe80::2:1", "192.168.2.1", 11, 201);
    adj43_1 =
        createAdjacency("3", "3/1", "4/1", "fe80::3:1", "192.168.3.1", 11, 301);
    adj43_2 =
        createAdjacency("3", "3/2", "4/2", "fe80::3:2", "192.168.3.2", 20, 302);
    adj43_3 =
        createAdjacency("3", "3/3", "4/3", "fe80::3:3", "192.168.3.3", 20, 303);

    adjacencyDb1 = createAdjDb("1", {adj12_1, adj12_2, adj12_3, adj13_1}, 1);

    adjacencyDb2 = createAdjDb("2", {adj21_1, adj21_2, adj21_3, adj24_1}, 2);

    adjacencyDb3 = createAdjDb("3", {adj31_1, adj34_1, adj34_2, adj34_3}, 3);

    adjacencyDb4 = createAdjDb("4", {adj42_1, adj43_1, adj43_2, adj43_3}, 4);

    // Adjacency db's
    areaLinkStates.emplace(
        kTestingAreaName, LinkState(kTestingAreaName, nodeName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);
    EXPECT_FALSE(
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
            .topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
            .topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
            .topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb4, kTestingAreaName)
            .topologyChanged);

    // Prefix db's
    updatePrefixDatabase(prefixState, prefixDb1);
    updatePrefixDatabase(prefixState, prefixDb2);
    updatePrefixDatabase(prefixState, prefixDb3);
    updatePrefixDatabase(prefixState, prefixDb4);
  }

  thrift::Adjacency adj12_1, adj12_2, adj12_3, adj13_1, adj21_1, adj21_2,
      adj21_3, adj24_1, adj31_1, adj34_1, adj34_2, adj34_3, adj42_1, adj43_1,
      adj43_2, adj43_3;
  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  std::unique_ptr<SpfSolver> spfSolver;
  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;
};

TEST_F(ParallelAdjRingTopologyFixture, ShortestPathTest) {
  CustomSetUp();
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  EXPECT_EQ(12, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 22),
           createNextHopFromAdj(adj13_1, false, 22),
           createNextHopFromAdj(adj12_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 11),
           createNextHopFromAdj(adj12_1, false, 11)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops(
          {createNextHopFromAdj(adj21_2, false, 22),
           createNextHopFromAdj(adj21_1, false, 22),
           createNextHopFromAdj(adj24_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj21_2, false, 11),
           createNextHopFromAdj(adj21_1, false, 11)}));

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj31_1, false, 22),
           createNextHopFromAdj(adj34_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11)}));

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj42_1, false, 22),
           createNextHopFromAdj(adj43_1, false, 22)}));
}

//
// Use the same topology, but test multi-path routing
//
TEST_F(ParallelAdjRingTopologyFixture, MultiPathTest) {
  CustomSetUp();
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  EXPECT_EQ(12, routeMap.size());

  // validate router 1
  // adj "2/3" is also selected in spite of large metric
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops(
          {createNextHopFromAdj(adj12_1, false, 22),
           createNextHopFromAdj(adj12_2, false, 22),
           createNextHopFromAdj(adj13_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj12_1, false, 11),
           createNextHopFromAdj(adj12_2, false, 11)}));

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops(
          {createNextHopFromAdj(adj21_1, false, 22),
           createNextHopFromAdj(adj21_2, false, 22),
           createNextHopFromAdj(adj24_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj21_1, false, 11),
           createNextHopFromAdj(adj21_2, false, 11)}));

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj31_1, false, 22),
           createNextHopFromAdj(adj34_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11)}));

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj42_1, false, 22),
           createNextHopFromAdj(adj43_1, false, 22)}));
}

//
// Test topology:
//
//  n * n grid
// A box m has up to 4 interfaces named 0/1, 0/2, 0/3, and 0/4
//                       m + n
//                         |
//                        0/4
//                         |
//         m-1 ----0/3---- m ----0/1---- m + 1
//                         |
//                        0/2
//                         |
//                       m - n

// add adjacencies to neighbor at grid(i, j)
void
addAdj(
    int i,
    int j,
    string ifName,
    vector<thrift::Adjacency>& adjs,
    int n,
    string otherIfName) {
  if (i < 0 || i >= n || j < 0 || j >= n) {
    return;
  }

  auto neighbor = i * n + j;
  adjs.emplace_back(createThriftAdjacency(
      fmt::format("{}", neighbor),
      ifName,
      fmt::format("fe80::{}", neighbor),
      fmt::format("192.168.{}.{}", neighbor / 256, neighbor % 256),
      1,
      100001 + neighbor /* adjacency-label */,
      false /* overload-bit */,
      100,
      10000 /* timestamp */,
      1 /* weight */,
      otherIfName));
}

string
nodeToPrefixV6(int node) {
  return fmt::format("::ffff:10.1.{}.{}/128", node / 256, node % 256);
}

void
createGrid(LinkState& linkState, PrefixState& prefixState, int n) {
  LOG(INFO) << "grid: " << n << " by " << n;
  // confined bcoz of min("fe80::{}", "192.168.{}.{}", "::ffff:10.1.{}.{}")
  EXPECT_TRUE(n * n < 10000) << "n is too large";

  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      auto node = i * n + j;
      auto nodeName = fmt::format("{}", node);

      // adjacency
      vector<thrift::Adjacency> adjs;
      addAdj(i, j + 1, "0/1", adjs, n, "0/3");
      addAdj(i - 1, j, "0/2", adjs, n, "0/4");
      addAdj(i, j - 1, "0/3", adjs, n, "0/1");
      addAdj(i + 1, j, "0/4", adjs, n, "0/2");
      auto adjacencyDb = createAdjDb(nodeName, adjs, node + 1);
      linkState.updateAdjacencyDatabase(adjacencyDb, kTestingAreaName);

      // prefix
      auto addrV6 = toIpPrefix(nodeToPrefixV6(node));
      updatePrefixDatabase(
          prefixState, createPrefixDb(nodeName, {createPrefixEntry(addrV6)}));
    }
  }
}

class GridTopologyFixture : public ::testing::TestWithParam<int> {
 public:
  GridTopologyFixture() : spfSolver(nodeName, false /* disable v4 */) {}

 protected:
  void
  SetUp() override {
    n = GetParam();
    areaLinkStates.emplace(
        kTestingAreaName, LinkState(kTestingAreaName, kTestingNodeName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);
    createGrid(linkState, prefixState, n);
  }

  // n * n grid
  int n;
  std::string nodeName{"1"};
  SpfSolver spfSolver;

  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;
};

INSTANTIATE_TEST_CASE_P(
    GridTopology, GridTopologyFixture, ::testing::Range(2, 17, 2));

// distance from node a to b in the grid n*n of unit link cost
int
gridDistance(int a, int b, int n) {
  int x_a = a % n, x_b = b % n;
  int y_a = a / n, y_b = b / n;

  return std::abs(x_a - x_b) + std::abs(y_a - y_b);
}

TEST_P(GridTopologyFixture, ShortestPathTest) {
  vector<string> allNodes;
  for (int i = 0; i < n * n; ++i) {
    allNodes.push_back(fmt::format("{}", i));
  }

  auto routeMap = getRouteMap(spfSolver, allNodes, areaLinkStates, prefixState);

  // unicastRoutes => n^2 * (n^2 - 1)
  // Total => n^4 - n^2
  EXPECT_EQ(n * n * n * n - n * n, routeMap.size());

  int src{0}, dst{0};
  NextHops nextHops;
  // validate route
  // 1) from corner to corner
  // primary diagnal
  src = 0;
  dst = n * n - 1;
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops = routeMap[make_pair(fmt::format("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric());

  // secondary diagnal
  src = n - 1;
  dst = n * (n - 1);
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops = routeMap[make_pair(fmt::format("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric());
  src = 0;
  dst = folly::Random::rand32() % (n * n - 1) + 1;
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops = routeMap[make_pair(fmt::format("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric());
  while ((dst = folly::Random::rand32() % (n * n)) == src) {
  }
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops = routeMap[make_pair(fmt::format("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric());
}

// measure SPF execution time for large networks
TEST(GridTopology, StressTest) {
  if (!FLAGS_stress_test) {
    return;
  }
  std::string nodeName("1");
  SpfSolver spfSolver(nodeName, false, true /* enable best route selection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, kTestingNodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  createGrid(linkState, prefixState, 99);
  spfSolver.buildRouteDb("523", areaLinkStates, prefixState);
}

namespace openr {
// FRIEND_TESTs must be in the same namespace as the class being accessed

TEST(SpfSolverUnitTest, GetReachablePrefixEntriesTest) {
  std::string nodeName("local");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable best route selection */,
      false /* disable v4_over_v6 */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));

  PrefixEntries allPrefixEntries;

  auto ribPrefix1 = createPrefixEntry(addr1, thrift::PrefixType::RIB);
  ribPrefix1.area_stack() = {"Area1", "Area2"};

  auto ribPrefix2 = createPrefixEntry(addr1, thrift::PrefixType::RIB);
  ribPrefix2.area_stack() = {"Area3"};

  allPrefixEntries.emplace(
      std::make_pair(nodeName, "other-area"),
      std::make_shared<thrift::PrefixEntry>(ribPrefix1));

  allPrefixEntries.emplace(
      std::make_pair("other-node", kTestingAreaName),
      std::make_shared<thrift::PrefixEntry>(ribPrefix2));

  auto [prefixEntries, localPrefixConsidered] =
      spfSolver.getReachablePrefixEntries(
          nodeName, areaLinkStates, allPrefixEntries);

  // no locally originated prefix is found
  EXPECT_FALSE(localPrefixConsidered);
  // ribPrefix1 is kept: It is in a different area "other-area"
  // ribPrefix2 is removed: It is in the same area (kTestingAreaName) while
  // SPF result cannot find the node (other-node)
  EXPECT_EQ(1, prefixEntries.size());

  auto configPrefix = createPrefixEntry(addr1, thrift::PrefixType::CONFIG);
  allPrefixEntries.emplace(
      std::make_pair(nodeName, kTestingAreaName),
      std::make_shared<thrift::PrefixEntry>(configPrefix));

  // One locally generated route is considered
  std::tie(prefixEntries, localPrefixConsidered) =
      spfSolver.getReachablePrefixEntries(
          nodeName, areaLinkStates, allPrefixEntries);
  EXPECT_TRUE(localPrefixConsidered);
}

} // namespace openr
