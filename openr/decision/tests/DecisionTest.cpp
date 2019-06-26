/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>

#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Random.h>
#include <folly/futures/Promise.h>
#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/decision/Decision.h>

DEFINE_bool(stress_test, false, "pass this to run the stress test");

using namespace std;
using namespace openr;
using namespace testing;

using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;

namespace {
/// R1 -> R2, R3
const auto adj12 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
const auto adj13 =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 10, 100003);
const auto adj12_old_1 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 1000021);
const auto adj12_old_2 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 20, 1000022);
const auto adj13_old =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 10, 1000031);
// R2 -> R1, R3, R4
const auto adj21 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);
const auto adj21_old_1 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 1000011);
const auto adj21_old_2 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 20, 1000012);
const auto adj23 =
    createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 10, 100003);
const auto adj24 =
    createAdjacency("4", "2/4", "4/2", "fe80::4", "192.168.0.4", 10, 100004);
// R3 -> R1, R2, R4
const auto adj31 =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 10, 100001);
const auto adj31_old =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 10, 1000011);
const auto adj32 =
    createAdjacency("2", "3/2", "2/3", "fe80::2", "192.168.0.2", 10, 100002);
const auto adj34 =
    createAdjacency("4", "3/4", "4/3", "fe80::4", "192.168.0.4", 10, 100004);
// R4 -> R2, R3
const auto adj42 =
    createAdjacency("2", "4/2", "2/4", "fe80::2", "192.168.0.2", 10, 100002);
const auto adj43 =
    createAdjacency("3", "4/3", "3/4", "fe80::3", "192.168.0.3", 10, 100003);

const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto addr4 = toIpPrefix("::ffff:10.4.4.4/128");
const auto addr5 = toIpPrefix("::ffff:10.4.4.5/128");
const auto addr6 = toIpPrefix("::ffff:10.4.4.6/128");
const auto addr1V4 = toIpPrefix("10.1.1.1/32");
const auto addr2V4 = toIpPrefix("10.2.2.2/32");
const auto addr3V4 = toIpPrefix("10.3.3.3/32");
const auto addr4V4 = toIpPrefix("10.4.4.4/32");

const auto prefixDb1 = createPrefixDb("1", {createPrefixEntry(addr1)});
const auto prefixDb2 = createPrefixDb("2", {createPrefixEntry(addr2)});
const auto prefixDb3 = createPrefixDb("3", {createPrefixEntry(addr3)});
const auto prefixDb4 = createPrefixDb("4", {createPrefixEntry(addr4)});
const auto prefixDb1V4 = createPrefixDb("1", {createPrefixEntry(addr1V4)});
const auto prefixDb2V4 = createPrefixDb("2", {createPrefixEntry(addr2V4)});
const auto prefixDb3V4 = createPrefixDb("3", {createPrefixEntry(addr3V4)});
const auto prefixDb4V4 = createPrefixDb("4", {createPrefixEntry(addr4V4)});

const thrift::MplsAction labelPopAction{
    createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP)};
const thrift::MplsAction labelPhpAction{
    createMplsAction(thrift::MplsActionCode::PHP)};
const thrift::MplsAction labelSwapAction1{
    createMplsAction(thrift::MplsActionCode::SWAP, 1)};
const thrift::MplsAction labelSwapAction2{
    createMplsAction(thrift::MplsActionCode::SWAP, 2)};
const thrift::MplsAction labelSwapAction3{
    createMplsAction(thrift::MplsActionCode::SWAP, 3)};
const thrift::MplsAction labelSwapAction4{
    createMplsAction(thrift::MplsActionCode::SWAP, 4)};
const thrift::MplsAction labelSwapAction5{
    createMplsAction(thrift::MplsActionCode::SWAP, 5)};

const thrift::NextHopThrift labelPopNextHop{
    apache::thrift::FRAGILE,
    toBinaryAddress(folly::IPAddressV6("::")),
    0 /* weight */,
    labelPopAction,
    0 /* metric */};

// timeout to wait until decision debounce
// (i.e. spf recalculation, route rebuild) finished
const std::chrono::milliseconds debounceTimeout{500};

thrift::PrefixDatabase
getPrefixDbWithKspfAlgo(thrift::PrefixDatabase const& prefixDb) {
  thrift::PrefixDatabase newPrefixDb = prefixDb;
  for (auto& p : newPrefixDb.prefixEntries) {
    p.forwardingType = thrift::PrefixForwardingType::SR_MPLS;
    p.forwardingAlgorithm = thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
  }
  return newPrefixDb;
}

thrift::NextHopThrift
createNextHopFromAdj(
    thrift::Adjacency adj,
    bool isV4,
    int32_t metric,
    folly::Optional<thrift::MplsAction> mplsAction = folly::none) {
  return createNextHop(
      isV4 ? adj.nextHopV4 : adj.nextHopV6,
      adj.ifName,
      metric,
      std::move(mplsAction));
}

// Note: use unordered_set bcoz paths in a route can be in arbitrary order
using NextHops = unordered_set<thrift::NextHopThrift>;
using RouteMap = unordered_map<
    pair<string /* node name */, string /* prefix or label */>,
    NextHops>;

// Note: routeMap will be modified
void
fillRouteMap(
    const string& node,
    RouteMap& routeMap,
    const thrift::RouteDatabase& routeDb) {
  for (auto const& route : routeDb.unicastRoutes) {
    auto prefix = toString(route.dest);
    for (const auto& nextHop : route.nextHops) {
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << toString(nextHop);

      routeMap[make_pair(node, prefix)].emplace(nextHop);
    }
  }
  for (auto const& route : routeDb.mplsRoutes) {
    auto topLabelStr = std::to_string(route.topLabel);
    for (const auto& nextHop : route.nextHops) {
      VLOG(4) << "node: " << node << " label: " << topLabelStr << " -> "
              << toString(nextHop);
      routeMap[make_pair(node, topLabelStr)].emplace(nextHop);
    }
  }
}

RouteMap
getRouteMap(SpfSolver& spfSolver, const vector<string>& nodes) {
  RouteMap routeMap;

  for (string const& node : nodes) {
    auto routeDb = spfSolver.buildPaths(node);
    if (not routeDb.hasValue()) {
      continue;
    }

    EXPECT_EQ(node, routeDb->thisNodeName);
    fillRouteMap(node, routeMap, routeDb.value());
  }

  return routeMap;
}

void
validateAdjLabelRoutes(
    RouteMap const& routeMap,
    std::string const& nodeName,
    std::vector<thrift::Adjacency> const& adjs) {
  for (auto const& adj : adjs) {
    const std::pair<std::string, std::string> routeKey{
        nodeName, std::to_string(adj.adjLabel)};
    ASSERT_EQ(1, routeMap.count(routeKey));
    EXPECT_EQ(
        routeMap.at(routeKey),
        NextHops(
            {createNextHopFromAdj(adj, false, adj.metric, labelPhpAction)}));
  }
}

void
validatePopLabelRoute(
    RouteMap const& routeMap, std::string const& nodeName, int32_t nodeLabel) {
  const std::pair<std::string, std::string> routeKey{nodeName,
                                                     std::to_string(nodeLabel)};
  ASSERT_EQ(1, routeMap.count(routeKey));
  EXPECT_EQ(routeMap.at(routeKey), NextHops({labelPopNextHop}));
}

} // anonymous namespace

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
      nodeName, false /* disable v4 */, false /* disable LFA */);

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb2).first);

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));

  unordered_map<
      pair<string /* node name */, string /* ip prefix */>,
      thrift::UnicastRoute>
      routeMap;

  vector<string> allNodes = {"1", "2"};

  for (string const& node : allNodes) {
    auto routeDb = spfSolver.buildPaths(node);
    ASSERT_TRUE(routeDb.hasValue());
    EXPECT_EQ(node, routeDb->thisNodeName);
    EXPECT_EQ(0, routeDb->unicastRoutes.size());
    EXPECT_EQ(0, routeDb->mplsRoutes.size()); // No label routes
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
      nodeName, false /* disable v4 */, false /* disable LFA */);

  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));

  auto routeDb = spfSolver.buildPaths("1");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(0, routeDb->unicastRoutes.size());
  EXPECT_EQ(0, routeDb->mplsRoutes.size());
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
      nodeName, false /* disable v4 */, false /* disable LFA */);

  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb2).first);
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));

  // dump routes for both nodes, expect no routing entries

  auto routeDb = spfSolver.buildPaths("1");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(0, routeDb->unicastRoutes.size());

  routeDb = spfSolver.buildPaths("2");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("2", routeDb->thisNodeName);
  EXPECT_EQ(0, routeDb->unicastRoutes.size());
}

//
// Query route for unknown neighbor. It should return none
//
TEST(ShortestPathTest, UnknownNode) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  auto routeDb = spfSolver.buildPaths("1");
  EXPECT_FALSE(routeDb.hasValue());

  routeDb = spfSolver.buildPaths("2");
  EXPECT_FALSE(routeDb.hasValue());
}

/**
 * Test to verify prefixDatabase update
 */
TEST(SpfSolver, PrefixUpdate) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_EQ(prefixDb1, spfSolver.getPrefixDatabases().at(nodeName));

  auto prefixDb1Updated = prefixDb1;
  prefixDb1Updated.prefixEntries.at(0).type = thrift::PrefixType::BREEZE;
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1Updated));
  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb1Updated));
  EXPECT_EQ(prefixDb1Updated, spfSolver.getPrefixDatabases().at(nodeName));

  prefixDb1Updated = prefixDb1;
  prefixDb1Updated.prefixEntries.at(0).forwardingType =
      thrift::PrefixForwardingType::SR_MPLS;
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1Updated));
  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb1Updated));
  EXPECT_EQ(prefixDb1Updated, spfSolver.getPrefixDatabases().at(nodeName));
}

TEST(SpfSolver, getNodeHostLoopbacksV4) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1V4));
  std::pair<std::string, thrift::BinaryAddress> pair1(
      "1", addr1V4.prefixAddress);
  EXPECT_THAT(
      spfSolver.getNodeHostLoopbacksV4(), testing::UnorderedElementsAre(pair1));

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2V4));
  std::pair<std::string, thrift::BinaryAddress> pair2(
      "2", addr2V4.prefixAddress);
  EXPECT_THAT(
      spfSolver.getNodeHostLoopbacksV4(),
      testing::UnorderedElementsAre(pair1, pair2));

  EXPECT_TRUE(spfSolver.deletePrefixDatabase("1"));
  EXPECT_THAT(
      spfSolver.getNodeHostLoopbacksV4(), testing::UnorderedElementsAre(pair2));
}

TEST(SpfSolver, getNodeHostLoopbacksV6) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  std::pair<std::string, thrift::BinaryAddress> pair1("1", addr1.prefixAddress);
  EXPECT_THAT(
      spfSolver.getNodeHostLoopbacksV6(), testing::UnorderedElementsAre(pair1));

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));
  std::pair<std::string, thrift::BinaryAddress> pair2("2", addr2.prefixAddress);
  EXPECT_THAT(
      spfSolver.getNodeHostLoopbacksV6(),
      testing::UnorderedElementsAre(pair1, pair2));

  EXPECT_TRUE(spfSolver.deletePrefixDatabase("1"));
  EXPECT_THAT(
      spfSolver.getNodeHostLoopbacksV6(), testing::UnorderedElementsAre(pair2));
}

/**
 * Test to verify adjacencyDatabase update
 */
TEST(SpfSolver, AdjacencyUpdate) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 2);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  //
  // Feed SPF solver with R1 and R2's adjacency + prefix dbs
  //

  {
    auto res = spfSolver.updateAdjacencyDatabase(adjacencyDb1);
    EXPECT_FALSE(res.first);
    EXPECT_TRUE(res.second); // label changed for node1
  }
  {
    auto res = spfSolver.updateAdjacencyDatabase(adjacencyDb2);
    EXPECT_TRUE(res.first);
    EXPECT_FALSE(res.second);
  }
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));

  //
  // dump routes for both nodes, expect 4 route entries (1 unicast, 3 label) on
  // each (node1-label, node2-label and adjacency-label)
  //

  auto routeDb = spfSolver.buildPaths("1");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildPaths("2");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("2", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  //
  // Update adjacency database of node 1 by changing it's nexthops and verift
  // that update properly responds to the event
  //
  adjacencyDb1.adjacencies[0].nextHopV6 = toBinaryAddress("fe80::1234:b00c");
  {
    auto res = spfSolver.updateAdjacencyDatabase(adjacencyDb1);
    EXPECT_FALSE(res.first);
    EXPECT_TRUE(res.second);
  }

  //
  // dump routes for both nodes, expect 4 route entries (1 unicast, 3 label) on
  // each (node1-label, node2-label and adjacency-label)
  //

  routeDb = spfSolver.buildPaths("1");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildPaths("2");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("2", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  //
  // Update adjacency database of node 2 by changing it's nexthops and verift
  // that update properly responds to the event (no spf trigger needed)
  //
  adjacencyDb2.adjacencies[0].nextHopV6 = toBinaryAddress("fe80::5678:b00c");
  {
    auto res = spfSolver.updateAdjacencyDatabase(adjacencyDb2);
    EXPECT_FALSE(res.first);
    EXPECT_FALSE(res.second);
  }

  //
  // dump routes for both nodes, expect 4 route entries (1 unicast, 3 label) on
  // each (node1-label, node2-label and adjacency-label)
  //

  routeDb = spfSolver.buildPaths("1");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildPaths("2");
  ASSERT_TRUE(routeDb.hasValue());
  EXPECT_EQ("2", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  //
  // Change adjLabel. This should report route-attribute change only for node1
  // and not for node2's adjLabel change
  //

  adjacencyDb1.adjacencies[0].adjLabel = 111;
  {
    auto res = spfSolver.updateAdjacencyDatabase(adjacencyDb1);
    EXPECT_FALSE(res.first);
    EXPECT_TRUE(res.second);
  }

  adjacencyDb2.adjacencies[0].adjLabel = 222;
  {
    auto res = spfSolver.updateAdjacencyDatabase(adjacencyDb2);
    EXPECT_FALSE(res.first);
    EXPECT_FALSE(res.second);
  }

  //
  // Change nodeLabel. This should report route-attribute change only for node1
  // and not for node2's nodeLabel change
  adjacencyDb1.nodeLabel = 11;
  {
    auto res = spfSolver.updateAdjacencyDatabase(adjacencyDb1);
    EXPECT_FALSE(res.first);
    EXPECT_TRUE(res.second);
  }

  adjacencyDb2.nodeLabel = 22;
  {
    auto res = spfSolver.updateAdjacencyDatabase(adjacencyDb2);
    EXPECT_FALSE(res.first);
    EXPECT_FALSE(res.second);
  }
}

//
// Node-1 connects to 2 but 2 doesn't report bi-directionality
// Node-2 and Node-3 are bi-directionally connected
//
TEST(MplsRoutes, BasicTest) {
  const std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj23}, 0); // No node label
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  EXPECT_EQ(
      std::make_pair(false, true),
      spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_EQ(
      std::make_pair(false, false),
      spfSolver.updateAdjacencyDatabase(adjacencyDb1));

  EXPECT_EQ(
      std::make_pair(false, false),
      spfSolver.updateAdjacencyDatabase(adjacencyDb2));

  EXPECT_EQ(
      std::make_pair(true, false),
      spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  auto routeMap = getRouteMap(spfSolver, {"1", "2", "3"});
  EXPECT_EQ(5, routeMap.size());

  // Validate 1's routes
  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);

  // Validate 2's routes (no node label route)
  validateAdjLabelRoutes(routeMap, "2", {adj23});

  // Validate 3's routes
  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", {adj32});
}

TEST(BGPRedistribution, BasicOperation) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 0);
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb2).first);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3).first);

  thrift::PrefixDatabase prefixDb1WithBGP = prefixDb1;
  thrift::PrefixDatabase prefixDb2WithBGP = prefixDb2;

  std::string data1 = "data1", data2 = "data2";
  thrift::IpPrefix bgpPrefix1 = addr3;

  thrift::MetricVector mv1, mv2;
  int64_t numMetrics = 5;
  mv1.metrics.resize(numMetrics);
  mv2.metrics.resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    mv1.metrics[i].type = mv2.metrics[i].type = i;
    mv1.metrics[i].priority = mv2.metrics[i].priority = i;
    mv1.metrics[i].op = mv2.metrics[i].op = thrift::CompareType::WIN_IF_PRESENT;
    mv1.metrics[i].isBestPathTieBreaker = mv2.metrics[i].isBestPathTieBreaker =
        false;
    mv1.metrics[i].metric = mv2.metrics[i].metric = {i};
  }

  // only node1 advertises the BGP prefix, it will have the best path
  prefixDb1WithBGP.prefixEntries.emplace_back(
      FRAGILE,
      bgpPrefix1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      false,
      mv1);

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1WithBGP));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));

  auto routeDb = spfSolver.buildPaths("2");
  thrift::UnicastRoute route1(
      FRAGILE,
      bgpPrefix1,
      {},
      thrift::AdminDistance::EBGP,
      {createNextHop(addr1.prefixAddress)},
      thrift::PrefixType::BGP,
      data1,
      false,
      createNextHop(addr1.prefixAddress));
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(2));
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::Contains(route1));

  // add the prefix to node2 with the same metric vector. we expect the bgp
  // route to be gone since both nodes have same metric vector we can't
  // determine a best path
  prefixDb2WithBGP.prefixEntries.emplace_back(
      FRAGILE,
      bgpPrefix1,
      thrift::PrefixType::BGP,
      data2,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      false,
      mv2);
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));
  routeDb = spfSolver.buildPaths("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(1));

  // decrease the one of second node's metrics and expect to see the route
  // toward just the first
  prefixDb2WithBGP.prefixEntries.back()
      .mv.value()
      .metrics[numMetrics - 1]
      .metric.front()--;
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));
  routeDb = spfSolver.buildPaths("2");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(2));
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::Contains(route1));

  // now make 2 better
  prefixDb2WithBGP.prefixEntries.back()
      .mv.value()
      .metrics[numMetrics - 1]
      .metric.front() += 2;
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));

  thrift::UnicastRoute route2(
      FRAGILE,
      bgpPrefix1,
      {},
      thrift::AdminDistance::EBGP,
      {createNextHop(addr2.prefixAddress)},
      thrift::PrefixType::BGP,
      data2,
      false,
      createNextHop(addr2.prefixAddress));

  routeDb = spfSolver.buildPaths("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(2));
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::Contains(route2));

  // now make that a tie break for a multipath route
  prefixDb1WithBGP.prefixEntries.back()
      .mv.value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;
  prefixDb2WithBGP.prefixEntries.back()
      .mv.value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1WithBGP));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));

  // 1 and 2 will not program BGP route
  EXPECT_THAT(
      spfSolver.buildPaths("1").value().unicastRoutes, testing::SizeIs(1));

  // 3 will program the BGP route towards both
  routeDb = spfSolver.buildPaths("3");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(3));
  EXPECT_THAT(
      routeDb.value().unicastRoutes,
      testing::Contains(AllOf(
          Field(&thrift::UnicastRoute::dest, bgpPrefix1),
          Field(&thrift::UnicastRoute::data, data2),
          Field(
              &thrift::UnicastRoute::nextHops,
              testing::UnorderedElementsAre(
                  createNextHop(addr2.prefixAddress),
                  createNextHop(addr1.prefixAddress))))));

  // dicsonnect the network, each node will consider it's BGP route the best,
  // and thus not program anything
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(createAdjDb("1", {}, 0)).first);
  EXPECT_THAT(
      spfSolver.buildPaths("1").value().unicastRoutes,
      testing::AllOf(
          testing::Not(testing::Contains(route1)),
          testing::Not(testing::Contains(route2))));
  EXPECT_THAT(
      spfSolver.buildPaths("2").value().unicastRoutes,
      testing::AllOf(
          testing::Not(testing::Contains(route1)),
          testing::Not(testing::Contains(route2))));
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
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  EXPECT_EQ(
      std::make_pair(false, true),
      spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_EQ(
      std::make_pair(!partitioned, false),
      spfSolver.updateAdjacencyDatabase(adjacencyDb2));
  EXPECT_EQ(
      std::make_pair(!partitioned, false),
      spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb3));

  // route from 1 to 3
  auto routeDb = spfSolver.buildPaths("1");
  bool foundRouteV6 = false;
  bool foundRouteNodeLabel = false;
  if (routeDb.hasValue()) {
    for (auto const& route : routeDb->unicastRoutes) {
      if (route.dest == addr3) {
        foundRouteV6 = true;
        break;
      }
    }
    for (auto const& route : routeDb->mplsRoutes) {
      if (route.topLabel == 3) {
        foundRouteNodeLabel = true;
      }
    }
  }

  EXPECT_EQ(partitioned, !foundRouteV6);
  EXPECT_EQ(partitioned, !foundRouteNodeLabel);
}

INSTANTIATE_TEST_CASE_P(
    PartitionedTopologyInstance, ConnectivityTest, ::testing::Bool());

//
// Overload node test in a linear topology with shortest path calculation
//
// 1<--->2<--->3
//   10     10
//
TEST(ConnectivityTest, OverloadNodeTest) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  // Make node-2 overloaded
  adjacencyDb2.isOverloaded = true;

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb3));

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb2).first);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3).first);

  auto routeMap = getRouteMap(spfSolver, {"1", "2", "3"});

  // We only expect 4 unicast routes, 7 node label routes because node-1 and
  // node-3 are disconnected.
  // node-1 => node-2 (label + unicast)
  // node-2 => node-1, node-3 (label + unicast)
  // node-3 => node-2 (label + unicast)
  //
  // NOTE: Adjacency label route remains up regardless of overloaded status and
  // there will be 4 of them
  EXPECT_EQ(15, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops(
          {createNextHopFromAdj(adj12, false, adj12.metric, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops(
          {createNextHopFromAdj(adj21, false, adj21.metric, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops(
          {createNextHopFromAdj(adj23, false, adj23.metric, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops(
          {createNextHopFromAdj(adj32, false, adj32.metric, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);
}

//
// AdjacencyDb compatibility test in a circle topology with shortest path
// calculation
// In old version remoter interface name is not sepcified
// 1(old)<--->2(new)<--->3(new)
//     |  20         10   ^
//     |                  |
//     |                  |
//     |------------------|
TEST(ConnectivityTest, CompatibilityNodeTest) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12_old_1}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21_old_1, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32, adj31_old}, 3);

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb3));

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb2).first);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3).first);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);

  // add/update adjacency of node1 with old versions
  adjacencyDb1 = createAdjDb("1", {adj12_old_1, adj13_old}, 1);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  adjacencyDb1 = createAdjDb("1", {adj12_old_2, adj13_old}, 1);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);

  auto routeMap = getRouteMap(spfSolver, {"1", "2", "3"});

  // We only expect 6 unicast routes, 9 node label routes and 6 adjacency routes
  // node-1 => node-2, node-3
  // node-2 => node-1, node-3
  // node-3 => node-2, node-1
  EXPECT_EQ(21, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_old_2, false, 20),
                createNextHopFromAdj(adj13_old, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12_old_2, false, 20, labelPhpAction),
                createNextHopFromAdj(adj13_old, false, 20, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(
          adj13_old, false, adj13_old.metric, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops(
          {createNextHopFromAdj(adj21, false, adj21.metric, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops(
          {createNextHopFromAdj(adj23, false, adj23.metric, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops(
          {createNextHopFromAdj(adj31, false, adj31.metric, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops(
          {createNextHopFromAdj(adj32, false, adj32.metric, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // adjacency update (remove adjacency) for node1
  adjacencyDb1 = createAdjDb("1", {adj12_old_2}, 0);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  adjacencyDb3 = createAdjDb("3", {adj32}, 0);
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb3).first);

  adjacencyDb1 = createAdjDb("1", {adj12_old_2, adj13_old}, 0);
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
}

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
class SimpleRingTopologyFixture : public ::testing::TestWithParam<bool> {
 public:
  SimpleRingTopologyFixture() : v4Enabled(GetParam()) {}

 protected:
  void
  CustomSetUp(bool calculateLfas, bool useKsp2Ed) {
    std::string nodeName("1");
    spfSolver = std::make_unique<SpfSolver>(nodeName, v4Enabled, calculateLfas);
    adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
    adjacencyDb2 = createAdjDb("2", {adj21, adj24}, 2);
    adjacencyDb3 = createAdjDb("3", {adj31, adj34}, 3);
    adjacencyDb4 = createAdjDb("4", {adj42, adj43}, 4);

    EXPECT_EQ(
        std::make_pair(false, true),
        spfSolver->updateAdjacencyDatabase(adjacencyDb1));
    EXPECT_EQ(
        std::make_pair(true, false),
        spfSolver->updateAdjacencyDatabase(adjacencyDb2));
    EXPECT_EQ(
        std::make_pair(true, false),
        spfSolver->updateAdjacencyDatabase(adjacencyDb3));
    EXPECT_EQ(
        std::make_pair(true, false),
        spfSolver->updateAdjacencyDatabase(adjacencyDb4));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? getPrefixDbWithKspfAlgo(pdb1) : pdb1));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? getPrefixDbWithKspfAlgo(pdb2) : pdb2));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? getPrefixDbWithKspfAlgo(pdb3) : pdb3));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? getPrefixDbWithKspfAlgo(pdb4) : pdb4));
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  std::unique_ptr<SpfSolver> spfSolver;
};

INSTANTIATE_TEST_CASE_P(
    SimpleRingTopologyInstance, SimpleRingTopologyFixture, ::testing::Bool());

//
// Verify SpfSolver finds the shortest path
//
TEST_P(SimpleRingTopologyFixture, ShortestPathTest) {
  CustomSetUp(false /* disable LFA */, false /* useKsp2Ed */);
  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(36, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20),
                createNextHopFromAdj(adj13, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20),
                createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20),
                createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20),
                createNextHopFromAdj(adj43, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);
}

//
// Use the same topology, but test multi-path routing
//
TEST_P(SimpleRingTopologyFixture, MultiPathTest) {
  CustomSetUp(true /* multipath */, false /* useKsp2Ed */);
  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(36, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20),
                createNextHopFromAdj(adj13, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20),
                createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20),
                createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20),
                createNextHopFromAdj(adj43, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);
}

//
// Validate KSP2_ED_ECMP routes on SimpleRingTopology
//
TEST_P(SimpleRingTopologyFixture, Ksp2EdEcmp) {
  CustomSetUp(true /* multipath - ignored */, true /* useKsp2Ed */);
  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(36, routeMap.size());

  auto pushCode = thrift::MplsActionCode::PUSH;
  auto push1 = createMplsAction(pushCode, folly::none, std::vector<int32_t>{1});
  auto push2 = createMplsAction(pushCode, folly::none, std::vector<int32_t>{2});
  auto push3 = createMplsAction(pushCode, folly::none, std::vector<int32_t>{3});
  auto push4 = createMplsAction(pushCode, folly::none, std::vector<int32_t>{4});
  auto push24 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{2, 4});
  auto push34 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{3, 4});
  auto push43 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{4, 3});
  auto push13 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{1, 3});
  auto push42 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{4, 2});
  auto push12 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{1, 2});
  auto push31 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{3, 1});
  auto push21 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{2, 1});

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20, push4),
                createNextHopFromAdj(adj13, v4Enabled, 20, push4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10),
                createNextHopFromAdj(adj12, v4Enabled, 30, push34)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10),
                createNextHopFromAdj(adj13, v4Enabled, 30, push24)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10),
                createNextHopFromAdj(adj21, v4Enabled, 30, push43)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20, push3),
                createNextHopFromAdj(adj24, v4Enabled, 20, push3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10),
                createNextHopFromAdj(adj24, v4Enabled, 30, push13)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10),
                createNextHopFromAdj(adj31, v4Enabled, 30, push42)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20, push2),
                createNextHopFromAdj(adj34, v4Enabled, 20, push2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10),
                createNextHopFromAdj(adj34, v4Enabled, 30, push12)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10),
                createNextHopFromAdj(adj42, v4Enabled, 30, push31)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10),
                createNextHopFromAdj(adj43, v4Enabled, 30, push21)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20, push1),
                createNextHopFromAdj(adj43, v4Enabled, 20, push1)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);
}

//
// attach nodes to outside world, e.g., POP
// verify all non-POP nodes find their closest POPs
//
TEST_P(SimpleRingTopologyFixture, AttachedNodesTest) {
  CustomSetUp(true /* multipath */, false /* useKsp2Ed */);
  // Advertise default prefixes from node-1 and node-4
  auto defaultRoutePrefix = v4Enabled ? "0.0.0.0/0" : "::/0";
  auto defaultRoute = toIpPrefix(defaultRoutePrefix);
  auto prefixDb1 = createPrefixDb(
      "1", {createPrefixEntry(addr1), createPrefixEntry(defaultRoute)});
  auto prefixDb4 = createPrefixDb(
      "4", {createPrefixEntry(addr4), createPrefixEntry(defaultRoute)});
  EXPECT_TRUE(spfSolver->updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver->updatePrefixDatabase(prefixDb4));

  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) + 2 (default routes) = 14
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(38, routeMap.size());

  // validate router 1
  // no default route boz it's attached
  // i.e., spfSolver(false), bcoz we set node 1 to be "1" distance away from the
  // dummy node and its neighbors are all further away, thus there is no route
  // to the dummy node
  EXPECT_EQ(0, routeMap.count({"1", defaultRoutePrefix}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", defaultRoutePrefix)],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10),
                createNextHopFromAdj(adj24, v4Enabled, 10)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", defaultRoutePrefix)],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10),
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
  CustomSetUp(true /* multipath */, false /* useKsp2Ed */);
  adjacencyDb2.isOverloaded = true;
  adjacencyDb3.isOverloaded = true;
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb2).first);
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);

  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 2 + 3 + 3 + 2 = 10
  // Node label routes => 3 + 4 + 4 + 3 = 14
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(32, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)})); // No LFA
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20),
                createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20),
                createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);
}

//
// Verify overload bit setting of individual adjacencies with multipath
// enabled. node-3 will get disconnected
//
TEST_P(SimpleRingTopologyFixture, OverloadLinkTest) {
  CustomSetUp(true /* multipath */, false /* useKsp2Ed */);
  adjacencyDb3.adjacencies[0].isOverloaded = true; // make adj31 overloaded
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);

  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(36, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 30)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 30, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3
  // no routes for router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 30)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 30, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);

  // Now also make adj34 overloaded which will disconnect the node-3
  adjacencyDb3.adjacencies[1].isOverloaded = true;
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);

  routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 2 + 2 + 0 + 2 = 6
  // Node label routes => 3 * 3 + 1 = 10
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(24, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);
}

/* add this block comment to suppress multiline breaker "\"s below
//
// Test topology: ring with parallel adjacencies, *x* denotes metric
//    ---*11*---
//   /          \
//  1----*11*----2
//  |\          /|
//  | ---*20*--- |
// *11*         *11*
//  |            |
//  | ---*11*--- |
//  |/          \|
//  3----*20*----4
//   \          /
//    ---*20*---
//
*/
class ParallelAdjRingTopologyFixture : public ::testing::Test {
 public:
  ParallelAdjRingTopologyFixture() {}

 protected:
  void
  CustomSetUp(bool calculateLfas, bool useKsp2Ed) {
    std::string nodeName("1");
    spfSolver = std::make_unique<SpfSolver>(nodeName, false, calculateLfas);
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

    EXPECT_FALSE(spfSolver->updateAdjacencyDatabase(adjacencyDb1).first);
    EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb2).first);
    EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);
    EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb4).first);

    // Prefix db's

    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? getPrefixDbWithKspfAlgo(prefixDb1) : prefixDb1));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? getPrefixDbWithKspfAlgo(prefixDb2) : prefixDb2));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? getPrefixDbWithKspfAlgo(prefixDb3) : prefixDb3));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? getPrefixDbWithKspfAlgo(prefixDb4) : prefixDb4));
  }

  thrift::Adjacency adj12_1, adj12_2, adj12_3, adj13_1, adj21_1, adj21_2,
      adj21_3, adj24_1, adj31_1, adj34_1, adj34_2, adj34_3, adj42_1, adj43_1,
      adj43_2, adj43_3;
  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  std::unique_ptr<SpfSolver> spfSolver;
};

TEST_F(ParallelAdjRingTopologyFixture, ShortestPathTest) {
  CustomSetUp(false /* shortest path */, false /* useKsp2Ed */);
  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 4 = 16
  EXPECT_EQ(44, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops({createNextHopFromAdj(adj12_2, false, 22),
                createNextHopFromAdj(adj13_1, false, 22),
                createNextHopFromAdj(adj12_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12_2, false, 22, labelSwapAction4),
                createNextHopFromAdj(adj13_1, false, 22, labelSwapAction4),
                createNextHopFromAdj(adj12_1, false, 22, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_2, false, 11),
                createNextHopFromAdj(adj12_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12_2, false, 11, labelPhpAction),
                createNextHopFromAdj(adj12_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21_2, false, 22),
                createNextHopFromAdj(adj21_1, false, 22),
                createNextHopFromAdj(adj24_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21_2, false, 22, labelSwapAction3),
                createNextHopFromAdj(adj21_1, false, 22, labelSwapAction3),
                createNextHopFromAdj(adj24_1, false, 22, labelSwapAction3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21_2, false, 11),
                createNextHopFromAdj(adj21_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21_2, false, 11, labelPhpAction),
                createNextHopFromAdj(adj21_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22),
                createNextHopFromAdj(adj34_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, labelSwapAction2),
                createNextHopFromAdj(adj34_1, false, 22, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22),
                createNextHopFromAdj(adj43_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22, labelSwapAction1),
                createNextHopFromAdj(adj43_1, false, 22, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);
}

//
// Use the same topology, but test multi-path routing
//
TEST_F(ParallelAdjRingTopologyFixture, MultiPathTest) {
  CustomSetUp(true /* multipath */, false /* useKsp2Ed */);
  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 4 = 16
  EXPECT_EQ(44, routeMap.size());

  // validate router 1

  // adj "2/3" is also selected in spite of large metric
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops({createNextHopFromAdj(adj12_1, false, 22),
                createNextHopFromAdj(adj12_2, false, 22),
                createNextHopFromAdj(adj12_3, false, 31),
                createNextHopFromAdj(adj13_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12_1, false, 22, labelSwapAction4),
                createNextHopFromAdj(adj12_2, false, 22, labelSwapAction4),
                createNextHopFromAdj(adj12_3, false, 31, labelSwapAction4),
                createNextHopFromAdj(adj13_1, false, 22, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11),
                createNextHopFromAdj(adj12_2, false, 11),
                createNextHopFromAdj(adj12_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11, labelPhpAction),
                createNextHopFromAdj(adj12_2, false, 11, labelPhpAction),
                createNextHopFromAdj(adj12_3, false, 20, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21_1, false, 22),
                createNextHopFromAdj(adj21_2, false, 22),
                createNextHopFromAdj(adj21_3, false, 31),
                createNextHopFromAdj(adj24_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21_1, false, 22, labelSwapAction3),
                createNextHopFromAdj(adj21_2, false, 22, labelSwapAction3),
                createNextHopFromAdj(adj21_3, false, 31, labelSwapAction3),
                createNextHopFromAdj(adj24_1, false, 22, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21_1, false, 11),
                createNextHopFromAdj(adj21_2, false, 11),
                createNextHopFromAdj(adj21_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21_1, false, 11, labelPhpAction),
                createNextHopFromAdj(adj21_2, false, 11, labelPhpAction),
                createNextHopFromAdj(adj21_3, false, 20, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11),
                createNextHopFromAdj(adj34_2, false, 20),
                createNextHopFromAdj(adj34_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11, labelPhpAction),
                createNextHopFromAdj(adj34_2, false, 20, labelPhpAction),
                createNextHopFromAdj(adj34_3, false, 20, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22),
                createNextHopFromAdj(adj34_1, false, 22),
                createNextHopFromAdj(adj34_2, false, 31),
                createNextHopFromAdj(adj34_3, false, 31)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, labelSwapAction2),
                createNextHopFromAdj(adj34_1, false, 22, labelSwapAction2),
                createNextHopFromAdj(adj34_2, false, 31, labelSwapAction2),
                createNextHopFromAdj(adj34_3, false, 31, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11),
                createNextHopFromAdj(adj43_2, false, 20),
                createNextHopFromAdj(adj43_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11, labelPhpAction),
                createNextHopFromAdj(adj43_2, false, 20, labelPhpAction),
                createNextHopFromAdj(adj43_3, false, 20, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22),
                createNextHopFromAdj(adj43_1, false, 22),
                createNextHopFromAdj(adj43_2, false, 31),
                createNextHopFromAdj(adj43_3, false, 31)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22, labelSwapAction1),
                createNextHopFromAdj(adj43_1, false, 22, labelSwapAction1),
                createNextHopFromAdj(adj43_2, false, 31, labelSwapAction1),
                createNextHopFromAdj(adj43_3, false, 31, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);
}

//
// Use the same topology, but test KSP2_ED_ECMP routing
//
TEST_F(ParallelAdjRingTopologyFixture, Ksp2EdEcmp) {
  CustomSetUp(true /* multipath, ignored */, true /* useKsp2Ed */);

  auto pushCode = thrift::MplsActionCode::PUSH;
  auto push1 = createMplsAction(pushCode, folly::none, std::vector<int32_t>{1});
  auto push2 = createMplsAction(pushCode, folly::none, std::vector<int32_t>{2});
  auto push3 = createMplsAction(pushCode, folly::none, std::vector<int32_t>{3});
  auto push4 = createMplsAction(pushCode, folly::none, std::vector<int32_t>{4});
  auto push34 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{3, 4});
  auto push43 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{4, 3});
  auto push12 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{1, 2});
  auto push21 =
      createMplsAction(pushCode, folly::none, std::vector<int32_t>{2, 1});

  //
  // Verify parallel link case between node-1 and node-2
  auto routeMap = getRouteMap(*spfSolver, {"1"});

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11),
                createNextHopFromAdj(adj12_2, false, 11),
                createNextHopFromAdj(adj12_3, false, 20)}));

  //
  // Bring down adj12_2 and adj34_2 to make our nexthop validations easy
  // Then validate routing table of all the nodes
  //

  adjacencyDb1.adjacencies.at(1).isOverloaded = true;
  adjacencyDb3.adjacencies.at(2).isOverloaded = true;
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);

  routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 3 = 16
  EXPECT_EQ(44, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops({createNextHopFromAdj(adj12_1, false, 22, push4),
                createNextHopFromAdj(adj13_1, false, 22, push4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11),
                createNextHopFromAdj(adj12_1, false, 33, push34)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11),
                createNextHopFromAdj(adj12_3, false, 20)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11),
                createNextHopFromAdj(adj21_1, false, 33, push43)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21_1, false, 22, push3),
                createNextHopFromAdj(adj24_1, false, 22, push3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21_1, false, 11),
                createNextHopFromAdj(adj21_3, false, 20)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11),
                createNextHopFromAdj(adj34_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, push2),
                createNextHopFromAdj(adj34_1, false, 22, push2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11),
                createNextHopFromAdj(adj34_1, false, 33, push12)}));

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11),
                createNextHopFromAdj(adj43_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11),
                createNextHopFromAdj(adj43_1, false, 33, push21)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22, push1),
                createNextHopFromAdj(adj43_1, false, 22, push1)}));
}

/**
 * Topology
 *        R2 - - - - - - R4
 *      //   \   10      /
 *    // 10   \        /
 *  //         \10   / 20
 * R1            \  /
 *   \           /
 *     \ 10    /  \
 *       \   /      \
 *        R3 - - - - R5
 *              10
 *
 * Node-4, Node-5 announces the default route. We validates following
 * - IP2MPLS routes for prefixes announced from each node
 * - MPLS routes for each node
 * - No adjacency routes were accounted
 */
TEST(DecisionTest, Ip2MplsRoutes) {
  std::string nodeName("1");
  auto spfSolver = std::make_unique<SpfSolver>(nodeName, false, true);

  // R1
  auto adj12_1 =
      createAdjacency("2", "2/1", "1/1", "fe80::2", "192.168.1.2", 10, 0);
  auto adj12_2 =
      createAdjacency("2", "2/2", "1/2", "fe80::2", "192.168.1.2", 10, 0);
  auto adj13 =
      createAdjacency("3", "3/1", "1/1", "fe80::3", "192.168.1.3", 10, 0);
  // R2
  auto adj21_1 =
      createAdjacency("1", "1/1", "2/1", "fe80::1", "192.168.1.1", 10, 0);
  auto adj21_2 =
      createAdjacency("1", "1/2", "2/2", "fe80::1", "192.168.1.1", 10, 0);
  auto adj24 =
      createAdjacency("4", "4/1", "2/1", "fe80::4", "192.168.1.4", 10, 0);
  auto adj25 =
      createAdjacency("5", "5/1", "2/1", "fe80::5", "192.168.1.5", 10, 0);
  // R3
  auto adj31 =
      createAdjacency("1", "1/1", "3/1", "fe80::1", "192.168.1.1", 10, 0);
  auto adj34 =
      createAdjacency("4", "4/1", "3/1", "fe80::4", "192.168.1.4", 20, 0);
  auto adj35 =
      createAdjacency("5", "5/1", "3/1", "fe80::5", "192.168.1.5", 10, 0);
  // R4
  auto adj42 =
      createAdjacency("2", "2/1", "4/1", "fe80::2", "192.168.1.2", 10, 0);
  auto adj43 =
      createAdjacency("3", "3/1", "4/1", "fe80::3", "192.168.1.3", 20, 0);
  // R5
  auto adj52 =
      createAdjacency("2", "2/1", "5/1", "fe80::2", "192.168.1.2", 10, 0);
  auto adj53 =
      createAdjacency("3", "3/1", "5/1", "fe80::3", "192.168.1.3", 10, 0);

  auto adjacencyDb1 = createAdjDb("1", {adj12_1, adj12_2, adj13}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21_1, adj21_2, adj24, adj25}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj31, adj34, adj35}, 3);
  auto adjacencyDb4 = createAdjDb("4", {adj42, adj43}, 4);
  auto adjacencyDb5 = createAdjDb("5", {adj52, adj53}, 5);

  // Adjacency db's
  EXPECT_FALSE(spfSolver->updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb2).first);
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb4).first);
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb5).first);

  // Prefix db's
  const auto defaultPrefixV6 = toIpPrefix("::/0");
  const auto prefixDb1_ = createPrefixDb(
      "1",
      {createPrefixEntry(
          addr1,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_TRUE(spfSolver->updatePrefixDatabase(prefixDb1_));
  const auto prefixDb2_ = createPrefixDb(
      "2",
      {createPrefixEntry(
          addr2,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_TRUE(spfSolver->updatePrefixDatabase(prefixDb2_));
  const auto prefixDb3_ = createPrefixDb(
      "3",
      {createPrefixEntry(
          addr3,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_TRUE(spfSolver->updatePrefixDatabase(prefixDb3_));
  const auto prefixDb4_ = createPrefixDb(
      "4",
      {createPrefixEntry(
          defaultPrefixV6,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_TRUE(spfSolver->updatePrefixDatabase(prefixDb4_));
  const auto prefixDb5_ = createPrefixDb(
      "5",
      {createPrefixEntry(
          defaultPrefixV6,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_TRUE(spfSolver->updatePrefixDatabase(prefixDb5_));

  // Some actions
  auto const labelPush1 = createMplsAction(
      thrift::MplsActionCode::PUSH, folly::none, std::vector<int32_t>{1});
  auto const labelPush2 = createMplsAction(
      thrift::MplsActionCode::PUSH, folly::none, std::vector<int32_t>{2});
  auto const labelPush3 = createMplsAction(
      thrift::MplsActionCode::PUSH, folly::none, std::vector<int32_t>{3});
  auto const labelPush4 = createMplsAction(
      thrift::MplsActionCode::PUSH, folly::none, std::vector<int32_t>{4});
  auto const labelPush5 = createMplsAction(
      thrift::MplsActionCode::PUSH, folly::none, std::vector<int32_t>{5});

  //
  // Get route-map
  //
  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4", "5"});

  // Unicast routes => 15 (5 * 3)
  // Node label routes => 5 * 5 = 25
  // Adj label routes => 0
  EXPECT_EQ(40, routeMap.size());

  // Validate router-1

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 10),
                createNextHopFromAdj(adj12_2, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", "::/0")],
      NextHops({createNextHopFromAdj(adj13, false, 30, labelPush4),
                createNextHopFromAdj(adj13, false, 20, labelPush5),
                createNextHopFromAdj(adj12_2, false, 20, labelPush4),
                createNextHopFromAdj(adj12_2, false, 20, labelPush5),
                createNextHopFromAdj(adj12_1, false, 20, labelPush4),
                createNextHopFromAdj(adj12_1, false, 20, labelPush5)}));

  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12_1, false, 10, labelPhpAction),
                createNextHopFromAdj(adj12_2, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12_1, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj12_2, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 30, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb5.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12_1, false, 20, labelSwapAction5),
                createNextHopFromAdj(adj12_2, false, 20, labelSwapAction5),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction5)}));

  // Validate router-2
  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21_1, false, 10),
                createNextHopFromAdj(adj21_2, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21_1, false, 20, labelPush3),
                createNextHopFromAdj(adj21_2, false, 20, labelPush3),
                createNextHopFromAdj(adj25, false, 20, labelPush3),
                createNextHopFromAdj(adj24, false, 30, labelPush3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", "::/0")],
      NextHops({createNextHopFromAdj(adj24, false, 10),
                createNextHopFromAdj(adj25, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21_1, false, 10, labelPhpAction),
                createNextHopFromAdj(adj21_2, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21_1, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj21_2, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj25, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 30, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb5.nodeLabel))],
      NextHops({createNextHopFromAdj(adj25, false, 10, labelPhpAction)}));

  // Validate router-3
  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10),
                createNextHopFromAdj(adj34, false, 40, labelPush1)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelPush2),
                createNextHopFromAdj(adj35, false, 20, labelPush2),
                createNextHopFromAdj(adj34, false, 30, labelPush2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", "::/0")],
      NextHops({createNextHopFromAdj(adj34, false, 20),
                createNextHopFromAdj(adj35, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction),
                createNextHopFromAdj(adj34, false, 40, labelSwapAction1)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj35, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 30, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 20, labelPhpAction),
                createNextHopFromAdj(adj31, false, 30, labelSwapAction4),
                createNextHopFromAdj(adj35, false, 30, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb5.nodeLabel))],
      NextHops({createNextHopFromAdj(adj35, false, 10, labelPhpAction),
                createNextHopFromAdj(adj34, false, 40, labelSwapAction5)}));

  // Validate router-4

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelPush1),
                createNextHopFromAdj(adj43, false, 30, labelPush1)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42, false, 10),
                createNextHopFromAdj(adj43, false, 40, labelPush2)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43, false, 20),
                createNextHopFromAdj(adj42, false, 30, labelPush3)}));

  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 30, labelSwapAction1)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction),
                createNextHopFromAdj(adj43, false, 40, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43, false, 20, labelPhpAction),
                createNextHopFromAdj(adj42, false, 30, labelSwapAction3)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb5.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction5),
                createNextHopFromAdj(adj43, false, 30, labelSwapAction5)}));

  // Validate router-5
  validatePopLabelRoute(routeMap, "5", adjacencyDb5.nodeLabel);

  EXPECT_EQ(
      routeMap[make_pair("5", toString(addr1))],
      NextHops({createNextHopFromAdj(adj52, false, 20, labelPush1),
                createNextHopFromAdj(adj53, false, 20, labelPush1)}));
  EXPECT_EQ(
      routeMap[make_pair("5", toString(addr2))],
      NextHops({createNextHopFromAdj(adj52, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("5", toString(addr3))],
      NextHops({createNextHopFromAdj(adj53, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("5", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj52, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj53, false, 20, labelSwapAction1)}));
  EXPECT_EQ(
      routeMap[make_pair("5", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj52, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("5", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj53, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("5", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj52, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj53, false, 30, labelSwapAction4)}));
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
  adjs.emplace_back(thrift::Adjacency(
      FRAGILE,
      folly::sformat("{}", neighbor),
      ifName,
      toBinaryAddress(folly::IPAddress(folly::sformat("fe80::{}", neighbor))),
      toBinaryAddress(folly::IPAddress(
          folly::sformat("192.168.{}.{}", neighbor / 256, neighbor % 256))),
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
  return folly::sformat("::ffff:10.1.{}.{}/128", node / 256, node % 256);
}

void
createGrid(SpfSolver& spfSolver, int n) {
  LOG(INFO) << "grid: " << n << " by " << n;
  // confined bcoz of min("fe80::{}", "192.168.{}.{}", "::ffff:10.1.{}.{}")
  EXPECT_TRUE(n * n < 10000) << "n is too large";

  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      auto node = i * n + j;
      auto nodeName = folly::sformat("{}", node);

      // adjacency
      vector<thrift::Adjacency> adjs;
      addAdj(i, j + 1, "0/1", adjs, n, "0/3");
      addAdj(i - 1, j, "0/2", adjs, n, "0/4");
      addAdj(i, j - 1, "0/3", adjs, n, "0/1");
      addAdj(i + 1, j, "0/4", adjs, n, "0/2");
      auto adjacencyDb = createAdjDb(nodeName, adjs, node + 1);
      spfSolver.updateAdjacencyDatabase(adjacencyDb);

      // prefix
      auto addrV6 = toIpPrefix(nodeToPrefixV6(node));
      spfSolver.updatePrefixDatabase(
          createPrefixDb(nodeName, {createPrefixEntry(addrV6)}));
    }
  }
}

class GridTopologyFixture : public ::testing::TestWithParam<int> {
 public:
  GridTopologyFixture() : spfSolver(nodeName, false, false) {}

 protected:
  void
  SetUp() override {
    n = GetParam();
    createGrid(spfSolver, n);
  }

  // n * n grid
  int n;
  std::string nodeName{"1"};
  SpfSolver spfSolver;
};

INSTANTIATE_TEST_CASE_P(
    GridTopology, GridTopologyFixture, ::testing::Range(2, 17, 2));

// distance from node a to b in the grid n*n of unit link cost
int
gridDistance(int a, int b, int n) {
  int x_a = a % n, x_b = b % n;
  int y_a = a / n, y_b = b / n;

  return abs(x_a - x_b) + abs(y_a - y_b);
}

TEST_P(GridTopologyFixture, ShortestPathTest) {
  vector<string> allNodes;
  for (int i = 0; i < n * n; ++i) {
    allNodes.push_back(folly::sformat("{}", i));
  }

  auto routeMap = getRouteMap(spfSolver, allNodes);

  // unicastRoutes => n^2 * (n^2 - 1)
  // node label routes => n^2 * n^2
  // adj label routes => 2 * 2 * n * (n - 1) (each link is reported twice)
  // Total => 2n^4 + 3n^2 - 4n
  EXPECT_EQ(2 * n * n * n * n + 3 * n * n - 4 * n, routeMap.size());

  int src{0}, dst{0};
  NextHops nextHops;
  // validate route
  // 1) from corner to corner
  // primary diagnal
  src = 0;
  dst = n * n - 1;
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), nextHops.begin()->metric);

  // secondary diagnal
  src = n - 1;
  dst = n * (n - 1);
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), nextHops.begin()->metric);

  // 2) from origin (i.e., node 0) to random inner node
  src = 0;
  dst = folly::Random::rand32() % (n * n - 1) + 1;
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), nextHops.begin()->metric);

  // 3) from one random node to another
  src = folly::Random::rand32() % (n * n);
  while ((dst = folly::Random::rand32() % (n * n)) == src) {
  }
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), nextHops.begin()->metric);
}

// measure SPF execution time for large networks
TEST(GridTopology, StressTest) {
  if (!FLAGS_stress_test) {
    return;
  }
  std::string nodeName("1");
  SpfSolver spfSolver(nodeName, false, true);
  createGrid(spfSolver, 99);
  spfSolver.buildPaths("523");
}

//
// Start the decision thread and simulate KvStore communications
// Expect proper RouteDatabase publications to appear
//
class DecisionTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    kvStorePub.bind(fbzmq::SocketUrl{"inproc://kvStore-pub"});
    kvStoreRep.bind(fbzmq::SocketUrl{"inproc://kvStore-rep"});

    decision = make_shared<Decision>(
        "1", /* node name */
        true, /* enable v4 */
        true, /* computeLfaPaths */
        false, /* enableOrderedFib */
        false, /* bgpDryRun */
        AdjacencyDbMarker{"adj:"},
        PrefixDbMarker{"prefix:"},
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(500),
        folly::none,
        KvStoreLocalCmdUrl{"inproc://kvStore-rep"},
        KvStoreLocalPubUrl{"inproc://kvStore-pub"},
        std::string{"inproc://decision-rep"},
        DecisionPubUrl{"inproc://decision-pub"},
        MonitorSubmitUrl{"inproc://monitor-rep"},
        zeromqContext);

    decisionThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Decision thread starting";
      decision->run();
      LOG(INFO) << "Decision thread finishing";
    });
    decision->waitUntilRunning();

    const int hwm = 1000;
    decisionPub.setSockOpt(ZMQ_RCVHWM, &hwm, sizeof(hwm)).value();
    decisionPub.setSockOpt(ZMQ_SUBSCRIBE, "", 0).value();
    decisionPub.connect(fbzmq::SocketUrl{"inproc://decision-pub"});
    decisionReq.connect(fbzmq::SocketUrl{"inproc://decision-rep"});

    // Make initial sync request with empty route-db
    replyInitialSyncReq(thrift::Publication());
    // Make request from decision to ensure that sockets are ready for use!
    dumpRouteDatabase(decisionReq, {"random-node"}, serializer);
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping the decision thread";
    decision->stop();
    decisionThread->join();
  }

  //
  // member methods
  //

  std::unordered_map<std::string, thrift::RouteDatabase>
  dumpRouteDatabase(
      fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>& decisionReq,
      const vector<string>& allNodes,
      const apache::thrift::CompactSerializer& serializer) {
    std::unordered_map<std::string, thrift::RouteDatabase> routeMap;

    for (string const& node : allNodes) {
      decisionReq.sendThriftObj(
          thrift::DecisionRequest(
              FRAGILE, thrift::DecisionCommand::ROUTE_DB_GET, node),
          serializer);

      auto maybeReply =
          decisionReq.recvThriftObj<thrift::DecisionReply>(serializer);
      EXPECT_FALSE(maybeReply.hasError());
      const auto& routeDb = maybeReply.value().routeDb;
      EXPECT_EQ(node, routeDb.thisNodeName);

      routeMap[node] = routeDb;
      VLOG(4) << "---";
    }

    return routeMap;
  }

  thrift::RouteDatabase
  recvMyRouteDb(
      fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT>& decisionPub,
      const string& /* myNodeName */,
      const apache::thrift::CompactSerializer& serializer) {
    auto maybeRouteDb =
        decisionPub.recvThriftObj<thrift::RouteDatabase>(serializer);
    EXPECT_FALSE(maybeRouteDb.hasError());
    auto routeDb = maybeRouteDb.value();
    return routeDb;
  }

  void
  replyInitialSyncReq(const thrift::Publication& publication) {
    // receive the request for initial routeDb sync
    auto maybeDumpReq =
        kvStoreRep.recvThriftObj<thrift::KvStoreRequest>(serializer);
    EXPECT_FALSE(maybeDumpReq.hasError());
    auto dumpReq = maybeDumpReq.value();
    EXPECT_EQ(thrift::Command::KEY_DUMP, dumpReq.cmd);

    // send back routeDb reply
    kvStoreRep.sendThriftObj(publication, serializer);
  }

  // publish routeDb
  void
  sendKvPublication(const thrift::Publication& publication) {
    kvStorePub.sendThriftObj(publication, serializer);
  }

  // helper function
  thrift::Value
  createAdjValue(
      const string& node,
      int64_t version,
      const vector<thrift::Adjacency>& adjs) {
    return thrift::Value(
        FRAGILE,
        version,
        "originator-1",
        fbzmq::util::writeThriftObjStr(createAdjDb(node, adjs, 0), serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  thrift::Value
  createPrefixValue(
      const string& node,
      int64_t version,
      const vector<thrift::IpPrefix>& prefixes) {
    vector<thrift::PrefixEntry> prefixEntries;
    for (const auto& prefix : prefixes) {
      prefixEntries.emplace_back(createPrefixEntry(prefix));
    }
    return thrift::Value(
        FRAGILE,
        version,
        "originator-1",
        fbzmq::util::writeThriftObjStr(
            createPrefixDb(node, prefixEntries), serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  std::unordered_map<std::string, int64_t>
  getCountersMap() {
    folly::Promise<std::unordered_map<std::string, int64_t>> promise;
    auto future = promise.getFuture();
    decision->runInEventLoop([this, p = std::move(promise)]() mutable noexcept {
      p.setValue(decision->getCounters());
    });
    return std::move(future).get();
  }

  //
  // member variables
  //

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  CompactSerializer serializer{};

  // ZMQ context for IO processing
  fbzmq::Context zeromqContext{};

  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> kvStorePub{zeromqContext};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> kvStoreRep{zeromqContext};
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> decisionPub{zeromqContext};
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> decisionReq{zeromqContext};

  // KvStore owned by this wrapper.
  std::shared_ptr<Decision> decision{nullptr};

  // Thread in which KvStore will be running.
  std::unique_ptr<std::thread> decisionThread{nullptr};
};

// The following topology is used:
//
// 1---2---3
//
// We upload the link 1---2 with the initial sync and later publish
// the 2---3 link information. We then request the full routing dump
// from the decision process via respective socket.
//

TEST_F(DecisionTestFixture, BasicOperations) {
  //
  // publish the link state info to KvStore
  //

  auto publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);
  auto routeDb = recvMyRouteDb(decisionPub, "1", serializer);
  EXPECT_EQ(1, routeDb.unicastRoutes.size());
  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));
  //
  // publish the link state info to KvStore via the KvStore pub socket
  // we simulate adding a new router R3
  //

  // Some tricks here; we need to bump the time-stamp on router 2's data, so
  // it can override existing; for router 3 we publish new key-value

  publication = thrift::Publication(
      FRAGILE,
      {{"adj:3", createAdjValue("3", 1, {adj32})},
       {"adj:2", createAdjValue("2", 3, {adj21, adj23})},
       {"adj:4", createAdjValue("4", 1, {})}, // No adjacencies
       {"prefix:3", createPrefixValue("3", 1, {addr3})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);

  // validate routers

  // receive my local Decision routeDb publication
  routeDb = recvMyRouteDb(decisionPub, "1" /* node name */, serializer);
  EXPECT_EQ(2, routeDb.unicastRoutes.size());
  fillRouteMap("1", routeMap, routeDb);
  // 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj12, false, 20)}));

  // dump other nodes' routeDB
  auto routeDbMap = dumpRouteDatabase(decisionReq, {"2", "3"}, serializer);
  EXPECT_EQ(2, routeDbMap["2"].unicastRoutes.size());
  EXPECT_EQ(2, routeDbMap["3"].unicastRoutes.size());
  for (auto kv : routeDbMap) {
    fillRouteMap(kv.first, routeMap, kv.second);
  }

  // 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));

  // 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj32, false, 20)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));

  // remove 3
  publication = thrift::Publication(
      FRAGILE,
      thrift::KeyVals{},
      {"adj:3", "prefix:3", "adj:4"} /* expired keys */,
      {},
      {},
      "");

  sendKvPublication(publication);
  routeDb = recvMyRouteDb(decisionPub, "1" /* node name */, serializer);
  EXPECT_EQ(1, routeDb.unicastRoutes.size());
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));
}

// The following topology is used:
//
//         100
//  1--- ---------- 2
//   \_           _/
//      \_ ____ _/
//          800

// We upload parallel link 1---2 with the initial sync and later bring down the
// one with lower metric. We then verify updated route database is received
//

TEST_F(DecisionTestFixture, ParallelLinks) {
  auto adj12_1 =
      createAdjacency("2", "1/2-1", "2/1-1", "fe80::2", "192.168.0.2", 100, 0);
  auto adj12_2 =
      createAdjacency("2", "1/2-2", "2/1-2", "fe80::2", "192.168.0.2", 800, 0);
  auto adj21_1 =
      createAdjacency("1", "2/1-1", "1/2-1", "fe80::1", "192.168.0.1", 100, 0);
  auto adj21_2 =
      createAdjacency("1", "2/1-2", "1/2-2", "fe80::1", "192.168.0.1", 800, 0);

  auto publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj12_1, adj12_2})},
       {"adj:2", createAdjValue("2", 1, {adj21_1, adj21_2})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);
  auto routeDb = recvMyRouteDb(decisionPub, "1", serializer);
  EXPECT_EQ(1, routeDb.unicastRoutes.size());
  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 100),
                createNextHopFromAdj(adj12_2, false, 800)}));

  publication = thrift::Publication(
      FRAGILE, {{"adj:2", createAdjValue("2", 2, {adj21_2})}}, {}, {}, {}, "");

  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDb = recvMyRouteDb(decisionPub, "1" /* node name */, serializer);
  EXPECT_EQ(1, routeDb.unicastRoutes.size());
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_2, false, 800)}));

  // restore the original state
  publication = thrift::Publication(
      FRAGILE,
      {{"adj:2", createAdjValue("2", 2, {adj21_1, adj21_2})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDb = recvMyRouteDb(decisionPub, "1" /* node name */, serializer);
  EXPECT_EQ(1, routeDb.unicastRoutes.size());
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 100),
                createNextHopFromAdj(adj12_2, false, 800)}));

  // overload the least cost link
  auto adj21_1_overloaded = adj21_1;
  adj21_1_overloaded.isOverloaded = true;

  publication = thrift::Publication(
      FRAGILE,
      {{"adj:2", createAdjValue("2", 2, {adj21_1_overloaded, adj21_2})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDb = recvMyRouteDb(decisionPub, "1" /* node name */, serializer);
  EXPECT_EQ(1, routeDb.unicastRoutes.size());
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_2, false, 800)}));
}

// The following topology is used:
//
// 1---2---3---4
//
// We upload the link 1---2 with the initial sync and later publish
// the 2---3 & 3---4 link information. We expect it to trigger SPF only once.
//
TEST_F(DecisionTestFixture, PubDebouncing) {
  //
  // publish the link state info to KvStore
  //

  auto publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      "");

  auto counters = getCountersMap();
  EXPECT_EQ(0, counters["decision.path_build_runs.count.0"]);
  EXPECT_EQ(0, counters["decision.route_build_runs.count.0"]);
  sendKvPublication(publication);

  /* sleep override */
  // wait for SPF to finish
  std::this_thread::sleep_for(debounceTimeout / 2);
  // validate SPF after initial sync, no rebouncing here
  counters = getCountersMap();
  EXPECT_EQ(1, counters["decision.path_build_runs.count.0"]);
  EXPECT_EQ(1, counters["decision.route_build_runs.count.0"]);

  //
  // publish the link state info to KvStore via the KvStore pub socket
  // we simulate adding a new router R3
  //

  // Some tricks here; we need to bump the time-stamp on router 2's data, so
  // it can override existing; for router 3 we publish new key-value
  publication = thrift::Publication(
      FRAGILE,
      {{"adj:3", createAdjValue("3", 1, {adj32})},
       {"adj:2", createAdjValue("2", 3, {adj21, adj23})},
       {"prefix:3", createPrefixValue("3", 1, {addr3})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);

  // we simulate adding a new router R4

  // Some tricks here; we need to bump the time-stamp on router 3's data, so
  // it can override existing;

  publication = thrift::Publication(
      FRAGILE,
      {{"adj:4", createAdjValue("4", 1, {adj43})},
       {"adj:3", createAdjValue("3", 5, {adj32, adj34})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);

  /* sleep override */
  // wait for debouncing to kick in
  std::this_thread::sleep_for(debounceTimeout);
  // validate SPF
  counters = getCountersMap();
  EXPECT_EQ(2, counters["decision.path_build_runs.count.0"]);
  EXPECT_EQ(2, counters["decision.route_build_runs.count.0"]);
  //
  // Only publish prefix updates
  //
  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 1, {addr4})}},
      {},
      {},
      {},
      "");
  sendKvPublication(publication);

  /* sleep override */
  // wait for route rebuilding to finish
  std::this_thread::sleep_for(debounceTimeout / 2);
  counters = getCountersMap();
  EXPECT_EQ(2, counters["decision.path_build_runs.count.0"]);
  EXPECT_EQ(3, counters["decision.route_build_runs.count.0"]);

  //
  // publish adj updates right after prefix updates
  // Decision is supposed to only trigger spf recalculation

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 2, {addr4, addr5})}},
      {},
      {},
      {},
      "");
  sendKvPublication(publication);

  publication = thrift::Publication(
      FRAGILE, {{"adj:2", createAdjValue("2", 5, {adj21})}}, {}, {}, {}, "");
  sendKvPublication(publication);

  /* sleep override */
  // wait for SPF to finish
  std::this_thread::sleep_for(debounceTimeout);
  counters = getCountersMap();
  EXPECT_EQ(3, counters["decision.path_build_runs.count.0"]);
  EXPECT_EQ(4, counters["decision.route_build_runs.count.0"]);

  //
  // publish multiple prefix updates in a row
  // Decision is supposed to process prefix update only once

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 5, {addr4})}},
      {},
      {},
      {},
      "");
  sendKvPublication(publication);

  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 7, {addr4, addr6})}},
      {},
      {},
      {},
      "");
  sendKvPublication(publication);

  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 8, {addr4, addr5, addr6})}},
      {},
      {},
      {},
      "");
  sendKvPublication(publication);

  /* sleep override */
  // wait for route rebuilding to finish
  std::this_thread::sleep_for(debounceTimeout);
  counters = getCountersMap();
  EXPECT_EQ(3, counters["decision.path_build_runs.count.0"]);
  // only 1 request shall be processed
  EXPECT_EQ(5, counters["decision.route_build_runs.count.0"]);
}

//
// Send unrelated key-value pairs to Decision
// Make sure they do not trigger SPF runs, but rather ignored
//
TEST_F(DecisionTestFixture, NoSpfOnIrrelevantPublication) {
  //
  // publish the link state info to KvStore, but use different markers
  // those must be ignored by the decision module
  //

  auto publication = thrift::Publication(
      FRAGILE,
      {{"adj2:1", createAdjValue("1", 1, {adj12})},
       {"adji2:2", createAdjValue("2", 1, {adj21})},
       {"prefix2:1", createPrefixValue("1", 1, {addr1})},
       {"prefix2:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      "");

  auto counters = getCountersMap();
  EXPECT_EQ(0, counters["decision.path_build_runs.count.0"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // make sure the counter did not increment
  counters = getCountersMap();
  EXPECT_EQ(0, counters["decision.path_build_runs.count.0"]);
}

//
// Send duplicate key-value pairs to Decision
// Make sure subsquent duplicates are ignored.
//
TEST_F(DecisionTestFixture, NoSpfOnDuplicatePublication) {
  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //

  auto const publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      "");

  auto counters = getCountersMap();
  EXPECT_EQ(0, counters["decision.path_build_runs.count.0"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // make sure counter is incremented
  counters = getCountersMap();
  EXPECT_EQ(1, counters["decision.path_build_runs.count.0"]);

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // make sure counter is not incremented
  counters = getCountersMap();
  EXPECT_EQ(1, counters["decision.path_build_runs.count.0"]);
}

/**
 * Loop-alternate path testing. Topology is described as follows
 *          10
 *  node1 --------- node2
 *    \_           _/
 *  8   \_ node3 _/  9
 *
 */
TEST_F(DecisionTestFixture, LoopFreeAlternatePaths) {
  // Note: local copy overwriting global ones, to be changed in this test
  auto adj12 =
      createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 0);
  auto adj13 =
      createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 8, 0);
  auto adj21 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 0);
  auto adj23 =
      createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 9, 0);
  auto adj31 =
      createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 8, 0);
  auto adj32 =
      createAdjacency("2", "3/2", "2/3", "fe80::2", "192.168.0.2", 9, 0);

  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //

  auto publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj12, adj13})},
       {"adj:2", createAdjValue("2", 1, {adj21, adj23})},
       {"adj:3", createAdjValue("3", 1, {adj31, adj32})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})},
       {"prefix:3", createPrefixValue("3", 1, {addr3})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // validate routers
  auto routeMapList =
      dumpRouteDatabase(decisionReq, {"1", "2", "3"}, serializer);
  RouteMap routeMap;
  for (auto kv : routeMapList) {
    fillRouteMap(kv.first, routeMap, kv.second);
  }
  // 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10),
                createNextHopFromAdj(adj13, false, 17)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj12, false, 19),
                createNextHopFromAdj(adj13, false, 8)}));

  // 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10),
                createNextHopFromAdj(adj23, false, 17)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21, false, 18),
                createNextHopFromAdj(adj23, false, 9)}));

  // 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 8),
                createNextHopFromAdj(adj32, false, 19)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj31, false, 18),
                createNextHopFromAdj(adj32, false, 9)}));

  /**
   * Increase the distance between node-1 and node-2 to 100. Now we won't have
   * loop free alternate path from node3 to either of node-1 or node-2.
   *
   *          100
   *  node1 --------- node2
   *    \_           _/
   *  8   \_ node3 _/  9
   */
  adj12.metric = 100;
  adj21.metric = 100;
  publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 2, {adj12, adj13})},
       {"adj:2", createAdjValue("2", 2, {adj21, adj23})}},
      {},
      {},
      {},
      "");

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // Query new information
  // validate routers
  routeMapList = dumpRouteDatabase(decisionReq, {"1", "2", "3"}, serializer);
  routeMap.clear();
  for (auto kv : routeMapList) {
    fillRouteMap(kv.first, routeMap, kv.second);
  }

  // 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 100),
                createNextHopFromAdj(adj13, false, 17)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj12, false, 109),
                createNextHopFromAdj(adj13, false, 8)}));

  // 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 100),
                createNextHopFromAdj(adj23, false, 17)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21, false, 108),
                createNextHopFromAdj(adj23, false, 9)}));

  // 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 8)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 9)}));
}

/**
 * Test to verify route calculation when a prefix is advertised from more than
 * one node.
 *
 *
 *  node4(p4)
 *     |
 *   5 |
 *     |         10
 *  node1(p1) --------- node2(p2)
 *     |
 *     | 10
 *     |
 *  node3(p2)
 */
TEST_F(DecisionTestFixture, DuplicatePrefixes) {
  // Note: local copy overwriting global ones, to be changed in this test
  auto adj14 =
      createAdjacency("4", "1/4", "4/1", "fe80::4", "192.168.0.4", 5, 0);
  auto adj41 =
      createAdjacency("1", "4/1", "1/4", "fe80::1", "192.168.0.1", 5, 0);
  auto adj12 =
      createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 0);
  auto adj21 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 0);

  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  auto publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj14, adj12, adj13})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"adj:3", createAdjValue("3", 1, {adj31})},
       {"adj:4", createAdjValue("4", 1, {adj41})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})},
       // node3 has same address w/ node2
       {"prefix:3", createPrefixValue("3", 1, {addr2})},
       {"prefix:4", createPrefixValue("4", 1, {addr4})}},
      {},
      {},
      {},
      "");

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // Query new information
  // validate routers
  auto routeMapList =
      dumpRouteDatabase(decisionReq, {"1", "2", "3", "4"}, serializer);
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  RouteMap routeMap;
  for (auto kv : routeMapList) {
    fillRouteMap(kv.first, routeMap, kv.second);
  }

  // 1
  EXPECT_EQ(2, routeMapList["1"].unicastRoutes.size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10),
                createNextHopFromAdj(adj13, false, 10)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].unicastRoutes.size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].unicastRoutes.size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].unicastRoutes.size());
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj41, false, 15)}));

  /**
   * Increase the distance between node-1 and node-2 to 100. Now we on node-1
   * will reflect weights into nexthops and FIB will not do multipath
   *
   *  node4(p4)
   *     |
   *   5 |
   *     |         100
   *  node1(p1) --------- node2(p2)
   *     |
   *     | 10
   *     |
   *  node3(p2)
   */
  adj12.metric = 100;
  adj21.metric = 100;

  publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 2, {adj12, adj13, adj14})},
       {"adj:2", createAdjValue("2", 2, {adj21, adj23})}},
      {},
      {},
      {},
      "");

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // Query new information
  // validate routers
  routeMapList =
      dumpRouteDatabase(decisionReq, {"1", "2", "3", "4"}, serializer);
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  routeMap.clear();
  for (auto kv : routeMapList) {
    fillRouteMap(kv.first, routeMap, kv.second);
  }

  // 1
  EXPECT_EQ(2, routeMapList["1"].unicastRoutes.size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj13, false, 10),
                createNextHopFromAdj(adj12, false, 100)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].unicastRoutes.size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 100)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].unicastRoutes.size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].unicastRoutes.size());
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj41, false, 15)}));
}

/**
 * Tests reliability of Decision SUB socket. We overload SUB socket with lot
 * of messages and make sure none of them are lost. We make decision compute
 * routes for a large network topology taking good amount of CPU time. We
 * do not try to validate routes here instead we validate messages processed
 * by decision and message sent by us.
 *
 * Topology consists of 1000 nodes linear where node-i connects to 3 nodes
 * before it and 3 nodes after it.
 *
 */
TEST_F(DecisionTestFixture, DecisionSubReliability) {
  thrift::Publication initialPub;

  // Create full topology
  for (int i = 1; i <= 1000; i++) {
    const std::string src = folly::to<std::string>(i);

    // Create prefixDb value
    const auto addr = toIpPrefix(folly::sformat("face:cafe:babe::{}/128", i));
    initialPub.keyVals.emplace(
        folly::sformat("prefix:{}", i), createPrefixValue(src, 1, {addr}));

    // Create adjDb value
    vector<thrift::Adjacency> adjs;
    for (int j = std::max(1, i - 3); j <= std::min(1000, i + 3); j++) {
      if (i == j)
        continue;
      const std::string dst = folly::to<std::string>(j);
      auto adj = createAdjacency(
          dst,
          folly::sformat("{}/{}", src, dst),
          folly::sformat("{}/{}", dst, src),
          folly::sformat("fe80::{}", dst),
          "192.168.0.1" /* unused */,
          10 /* metric */,
          0 /* adj label */);
      adjs.emplace_back(std::move(adj));
    }
    initialPub.keyVals.emplace(
        folly::sformat("adj:{}", src), createAdjValue(src, 1, adjs));
  }

  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  sendKvPublication(initialPub);

  //
  // Hammer Decision with lot of duplicate publication for 2 * ThrottleTimeout
  // We want to ensure that we hammer Decision for atleast once during it's
  // SPF run. This will cause lot of pending publications on Decision. This
  // is not going to cause any SPF computation
  //
  thrift::Publication duplicatePub;
  duplicatePub.keyVals["prefix:1"] = initialPub.keyVals.at("prefix:1");
  int64_t totalSent = 0;
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (diff > (3 * debounceTimeout)) {
      LOG(INFO) << "Hammered decision with " << totalSent
                << " updates. Stopping";
      break;
    }
    ++totalSent;
    sendKvPublication(duplicatePub);
  }

  // Receive RouteUpdate from Decision
  auto routes1 = recvMyRouteDb(decisionPub, "1", serializer);
  EXPECT_EQ(999, routes1.unicastRoutes.size()); // Route to all nodes except
                                                // mine
  //
  // Wait until all pending updates are finished
  //
  std::this_thread::sleep_for(std::chrono::seconds(5));

  //
  // Advertise prefix update. Decision gonna take some
  // good amount of time to process this last update (as it has many queued
  // updates).
  //
  thrift::Publication newPub;
  auto newAddr = toIpPrefix("face:b00c:babe::1/128");
  newPub.keyVals["prefix:1"] = createPrefixValue("1", 2, {newAddr});
  LOG(INFO) << "Advertising prefix update";
  sendKvPublication(newPub);
  // Receive RouteUpdate from Decision
  auto routes2 = recvMyRouteDb(decisionPub, "1", serializer);
  EXPECT_EQ(999, routes2.unicastRoutes.size()); // Route to all nodes except
                                                // mine
  //
  // Verify counters information
  //

  const int64_t adjUpdateCnt = 1000 /* initial */;
  const int64_t prefixUpdateCnt = totalSent + 1000 /* initial */ + 1 /* end */;
  auto counters = getCountersMap();
  EXPECT_EQ(1, counters["decision.path_build_runs.count.0"]);
  EXPECT_EQ(adjUpdateCnt, counters["decision.adj_db_update.count.0"]);
  EXPECT_EQ(prefixUpdateCnt, counters["decision.prefix_db_update.count.0"]);
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
