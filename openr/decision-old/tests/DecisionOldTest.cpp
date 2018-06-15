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
#include <gmock/gmock.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>
#include <openr/decision-old/DecisionOld.h>

DEFINE_bool(stress_test, false, "pass this to run the stress test");

using namespace std;
using namespace openr;
using namespace testing;

using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;

namespace {
/// R1 -> R2, R3
const auto adj12 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 0);
const auto adj13 =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 10, 0);
const auto adj12_old_1 =
    createAdjacency("2", "1/2", "", "fe80::2", "192.168.0.2", 10, 0);
const auto adj12_old_2 =
    createAdjacency("2", "1/2", "", "fe80::2", "192.168.0.2", 20, 0);
const auto adj13_old =
    createAdjacency("3", "1/3", "", "fe80::3", "192.168.0.3", 10, 0);
// R2 -> R1, R3, R4
const auto adj21 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 0);
const auto adj21_old_1 =
    createAdjacency("1", "2/1", "", "fe80::1", "192.168.0.1", 10, 0);
const auto adj21_old_2 =
    createAdjacency("1", "2/1", "", "fe80::1", "192.168.0.1", 20, 0);
const auto adj23 =
    createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 10, 0);
const auto adj24 =
    createAdjacency("4", "2/4", "4/2", "fe80::4", "192.168.0.4", 10, 0);
// R3 -> R1, R2, R4
const auto adj31 =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 10, 0);
const auto adj31_old =
    createAdjacency("1", "3/1", "", "fe80::1", "192.168.0.1", 10, 0);
const auto adj32 =
    createAdjacency("2", "3/2", "2/3", "fe80::2", "192.168.0.2", 10, 0);
const auto adj34 =
    createAdjacency("4", "3/4", "4/3", "fe80::4", "192.168.0.4", 10, 0);
// R4 -> R2, R3
const auto adj42 =
    createAdjacency("2", "4/2", "2/4", "fe80::2", "192.168.0.2", 10, 0);
const auto adj43 =
    createAdjacency("3", "4/3", "3/4", "fe80::3", "192.168.0.3", 10, 0);

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

const auto prefixDb1 =
    createPrefixDb("1", {{FRAGILE, addr1, thrift::PrefixType::LOOPBACK, {}}});
const auto prefixDb2 =
    createPrefixDb("2", {{FRAGILE, addr2, thrift::PrefixType::LOOPBACK, {}}});
const auto prefixDb3 =
    createPrefixDb("3", {{FRAGILE, addr3, thrift::PrefixType::LOOPBACK, {}}});
const auto prefixDb4 =
    createPrefixDb("4", {{FRAGILE, addr4, thrift::PrefixType::LOOPBACK, {}}});
const auto prefixDb1V4 =
    createPrefixDb("1", {{FRAGILE, addr1V4, thrift::PrefixType::LOOPBACK, {}}});
const auto prefixDb2V4 =
    createPrefixDb("2", {{FRAGILE, addr2V4, thrift::PrefixType::LOOPBACK, {}}});
const auto prefixDb3V4 =
    createPrefixDb("3", {{FRAGILE, addr3V4, thrift::PrefixType::LOOPBACK, {}}});
const auto prefixDb4V4 =
    createPrefixDb("4", {{FRAGILE, addr4V4, thrift::PrefixType::LOOPBACK, {}}});

// timeout to wait until decision debounce
// (i.e. spf recalculation, route rebuild) finished
const std::chrono::milliseconds debounceTimeout{500};

using NextHop = pair<string /* ifname */, folly::IPAddress /* nexthop ip */>;
// Note: use unordered_set bcoz paths in a route can be in arbitrary order
using NextHops =
    unordered_set<pair<NextHop /* nexthop */, int32_t /* path metric */>>;
using RouteMap = unordered_map<
    pair<string /* node name */, string /* ip prefix */>,
    NextHops>;

// disable V4 by default
NextHop
toNextHop(thrift::Adjacency adj, bool isV4 = false) {
  return {adj.ifName, toIPAddress(isV4 ? adj.nextHopV4 : adj.nextHopV6)};
}

// Note: routeMap will be modified
void
fillRouteMap(
    const string& node,
    RouteMap& routeMap,
    const thrift::RouteDatabase& routeDb) {
  for (auto const& route : routeDb.routes) {
    auto prefix = toString(route.prefix);
    for (const auto& path : route.paths) {
      const auto nextHop = toIPAddress(path.nextHop);
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << path.ifName << " : " << nextHop << " (" << path.metric << ")";

      routeMap[make_pair(node, prefix)].insert(
          {{path.ifName, nextHop}, path.metric});
    }
  }
}

RouteMap
getRouteMap(
    SpfSolverOld& spfSolver, bool isMultipath, const vector<string>& nodes) {
  RouteMap routeMap;

  for (string const& node : nodes) {
    auto routeDb = isMultipath ? spfSolver.buildMultiPaths(node)
                               : spfSolver.buildShortestPaths(node);
    EXPECT_EQ(node, routeDb.thisNodeName);

    fillRouteMap(node, routeMap, routeDb);
    VLOG(4) << "---";
  }

  return routeMap;
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

  SpfSolverOld spfSolver(false /* disable v4 */);

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb2));

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));

  unordered_map<
      pair<string /* node name */, string /* ip prefix */>,
      thrift::Route>
      routeMap;

  vector<string> allNodes = {"1", "2"};

  for (string const& node : allNodes) {
    auto routeDb = spfSolver.buildShortestPaths(node);
    EXPECT_EQ(node, routeDb.thisNodeName);
    EXPECT_EQ(0, routeDb.routes.size());
  }
}

//
// R1 and R2 are adjacent, and R1 has this declared in its
// adjacency database. However, R1 is missing the AdjDb from
// R2. It should not be able to compute path to R2 in this case.
//
TEST(ShortestPathTest, MissingNeighborAdjacencyDb) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);

  SpfSolverOld spfSolver(false /* disable v4 */);

  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb2));

  auto routeDb = spfSolver.buildShortestPaths("1");
  EXPECT_EQ("1", routeDb.thisNodeName);
  EXPECT_EQ(0, routeDb.routes.size());
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

  SpfSolverOld spfSolver(false /* disable v4 */);

  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb2));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));

  // dump routes for both nodes, expect no routing entries

  auto routeDb = spfSolver.buildShortestPaths("1");
  EXPECT_EQ("1", routeDb.thisNodeName);
  EXPECT_EQ(0, routeDb.routes.size());

  routeDb = spfSolver.buildShortestPaths("2");
  EXPECT_EQ("2", routeDb.thisNodeName);
  EXPECT_EQ(0, routeDb.routes.size());
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

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3;

  adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 0);
  if (!partitioned) {
    adjacencyDb1 = createAdjDb("1", {adj12}, 0);
    adjacencyDb3 = createAdjDb("3", {adj32}, 0);
  }

  SpfSolverOld spfSolver(false /* disable v4 */);

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_EQ(!partitioned, spfSolver.updateAdjacencyDatabase(adjacencyDb2));
  EXPECT_EQ(!partitioned, spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  EXPECT_EQ(!partitioned, spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));
  EXPECT_EQ(!partitioned, spfSolver.updatePrefixDatabase(prefixDb3));

  // route from 1 to 3
  auto routeDb = spfSolver.buildShortestPaths("1");
  bool foundRouteV6 = false;
  for (auto const& route : routeDb.routes) {
    if (route.prefix == addr3) {
      foundRouteV6 = true;
      break;
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
TEST(ConnectivityTest, OverloadNodeTest) {
  SpfSolverOld spfSolver(false /* disable v4 */);

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 0);

  // Make node-2 overloaded
  adjacencyDb2.isOverloaded = true;

  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb2));
  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb3));

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb2));
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  auto routeMap =
      getRouteMap(spfSolver, false /* shortest path */, {"1", "2", "3"});

  // We only expect 4 routes because node-1 and node-3 are disconnected.
  // node-1 => node-2
  // node-2 => node-1, node-3
  // node-3 => node-2
  EXPECT_EQ(4, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({make_pair(toNextHop(adj12, false), 10)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({make_pair(toNextHop(adj23, false), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({make_pair(toNextHop(adj21, false), 10)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({make_pair(toNextHop(adj32, false), 10)}));
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
  SpfSolverOld spfSolver(false /* disable v4 */);

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12_old_1}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21_old_1, adj23}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj32, adj31_old}, 0);

  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb2));
  EXPECT_FALSE(spfSolver.updatePrefixDatabase(prefixDb3));

  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb2));
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3));
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));

  // add/update adjacency of node1 with old versions
  adjacencyDb1 = createAdjDb("1", {adj12_old_1, adj13_old}, 0);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  adjacencyDb1 = createAdjDb("1", {adj12_old_2, adj13_old}, 0);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));

  auto routeMap =
      getRouteMap(spfSolver, false /* shortest path */, {"1", "2", "3"});

  // We only expect 4 routes node-1 and node-3 are connected
  // node-1 => node-2, node-3
  // node-2 => node-1, node-3
  // node-3 => node-2, node-1
  EXPECT_EQ(6, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({make_pair(toNextHop(adj12, false), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({make_pair(toNextHop(adj13, false), 10)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({make_pair(toNextHop(adj23, false), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({make_pair(toNextHop(adj21, false), 10)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({make_pair(toNextHop(adj32, false), 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({make_pair(toNextHop(adj31, false), 10)}));

  // adjacency update (remove adjacency) for node1
  adjacencyDb1 = createAdjDb("1", {adj12_old_2}, 0);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
  adjacencyDb3 = createAdjDb("3", {adj32}, 0);
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  adjacencyDb1 = createAdjDb("1", {adj12_old_2, adj13_old}, 0);
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
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
  SimpleRingTopologyFixture() : v4Enabled(GetParam()), spfSolver(v4Enabled) {}

 protected:
  void
  SetUp() override {
    adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 0);
    adjacencyDb2 = createAdjDb("2", {adj21, adj24}, 0);
    adjacencyDb3 = createAdjDb("3", {adj31, adj34}, 0);
    adjacencyDb4 = createAdjDb("4", {adj42, adj43}, 0);

    EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
    EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb2));
    EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3));
    EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb4));

    EXPECT_TRUE(
        spfSolver.updatePrefixDatabase(v4Enabled ? prefixDb1V4 : prefixDb1));
    EXPECT_TRUE(
        spfSolver.updatePrefixDatabase(v4Enabled ? prefixDb2V4 : prefixDb2));
    EXPECT_TRUE(
        spfSolver.updatePrefixDatabase(v4Enabled ? prefixDb3V4 : prefixDb3));
    EXPECT_TRUE(
        spfSolver.updatePrefixDatabase(v4Enabled ? prefixDb4V4 : prefixDb4));
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  SpfSolverOld spfSolver;
};

INSTANTIATE_TEST_CASE_P(
    SimpleRingTopologyInstance, SimpleRingTopologyFixture, ::testing::Bool());

//
// Verify SpfSolverOld finds the shortest path
//
TEST_P(SimpleRingTopologyFixture, ShortestPathTest) {
  auto routeMap =
      getRouteMap(spfSolver, false /* shortest path */, {"1", "2", "3", "4"});

  // validate router 1

  EXPECT_THAT(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      AnyOf(Eq(NextHops({make_pair(toNextHop(adj12, v4Enabled), 20)})),
            Eq(NextHops({make_pair(toNextHop(adj13, v4Enabled), 20)}))));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj13, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 10)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj24, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj21, v4Enabled), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj21, v4Enabled), 10)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj34, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj31, v4Enabled), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj31, v4Enabled), 10)}));

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj43, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 20)}));
}

//
// Use the same topology, but test multi-path routing
//
TEST_P(SimpleRingTopologyFixture, MultiPathTest) {
  auto routeMap =
      getRouteMap(spfSolver, true /* multipath */, {"1", "2", "3", "4"});

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 20),
                make_pair(toNextHop(adj13, v4Enabled), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj13, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 10)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj24, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj21, v4Enabled), 20),
                make_pair(toNextHop(adj24, v4Enabled), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj21, v4Enabled), 10)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj34, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj31, v4Enabled), 20),
                make_pair(toNextHop(adj34, v4Enabled), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj31, v4Enabled), 10)}));

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj43, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 20),
                make_pair(toNextHop(adj43, v4Enabled), 20)}));
}

//
// attach nodes to outside world, e.g., POP
// verify all non-POP nodes find their closest POPs
//
TEST_P(SimpleRingTopologyFixture, AttachedNodesTest) {
  // Advertise default prefixes from node-1 and node-4
  auto defaultRoutePrefix = v4Enabled ? "0.0.0.0/0" : "::/0";
  auto defaultRoute = toIpPrefix(defaultRoutePrefix);
  auto prefixDb1 = createPrefixDb(
      "1",
      {{FRAGILE, addr1, thrift::PrefixType::LOOPBACK, {}},
       {FRAGILE, defaultRoute, thrift::PrefixType::LOOPBACK, {}}});
  auto prefixDb4 = createPrefixDb(
      "4",
      {{FRAGILE, addr4, thrift::PrefixType::LOOPBACK, {}},
       {FRAGILE, defaultRoute, thrift::PrefixType::LOOPBACK, {}}});
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb4));

  auto routeMap =
      getRouteMap(spfSolver, true /* multipath */, {"1", "2", "3", "4"});

  // validate router 1
  // no default route boz it's attached
  // i.e., spfSolver(false), bcoz we set node 1 to be "1" distance away from the
  // dummy node and its neighbors are all further away, thus there is no route
  // to the dummy node
  EXPECT_EQ(0, routeMap.count({"1", defaultRoutePrefix}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", defaultRoutePrefix)],
      NextHops({make_pair(toNextHop(adj21, v4Enabled), 10),
                make_pair(toNextHop(adj24, v4Enabled), 10)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", defaultRoutePrefix)],
      NextHops({make_pair(toNextHop(adj31, v4Enabled), 10),
                make_pair(toNextHop(adj34, v4Enabled), 10)}));

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
  adjacencyDb2.isOverloaded = true;
  adjacencyDb3.isOverloaded = true;
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb2));
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  auto routeMap =
      getRouteMap(spfSolver, true /* multipath */, {"1", "2", "3", "4"});

  EXPECT_EQ(10, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj13, v4Enabled), 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 10)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj24, v4Enabled), 10)})); // No LFA
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({
          make_pair(toNextHop(adj21, v4Enabled), 20),
          make_pair(toNextHop(adj24, v4Enabled), 20),
      }));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj21, v4Enabled), 10)})); // No LFA

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj34, v4Enabled), 10)})); // No LFA
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({
          make_pair(toNextHop(adj31, v4Enabled), 20),
          make_pair(toNextHop(adj34, v4Enabled), 20),
      }));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj31, v4Enabled), 10)})); // No LFA

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj43, v4Enabled), 10)})); // No LFA
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 10)})); // No LFA
}

//
// Verify overload bit setting of individual adjacencies with multipath
// enabled. node-3 will get disconnected
//
TEST_P(SimpleRingTopologyFixture, OverloadLinkTest) {
  adjacencyDb3.adjacencies[0].isOverloaded = true; // make adj31 overloaded
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  auto routeMap =
      getRouteMap(spfSolver, true /* multipath */, {"1", "2", "3", "4"});

  EXPECT_EQ(12, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 30)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 10)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj24, v4Enabled), 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj24, v4Enabled), 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj21, v4Enabled), 10)}));

  // validate router 3
  // no routes for router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj34, v4Enabled), 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj34, v4Enabled), 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj34, v4Enabled), 30)}));

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({make_pair(toNextHop(adj43, v4Enabled), 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 20)}));

  // Now also make adj34 overloaded which will disconnect the node-3
  adjacencyDb3.adjacencies[1].isOverloaded = true;
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  routeMap = getRouteMap(spfSolver, true /* multipath */, {"1", "2", "3", "4"});
  EXPECT_EQ(6, routeMap.size()); // No routes for node-3

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj12, v4Enabled), 10)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({make_pair(toNextHop(adj24, v4Enabled), 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj21, v4Enabled), 10)}));

  // validate router 3
  // no routes for router 3

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({make_pair(toNextHop(adj42, v4Enabled), 20)}));
}

/* add this block comment to suppress multiline breaker "\"s below
//
// Test topology: ring with parallel adjacencies, *x* denotes metric
//    ---*10*---
//   /          \
//  1----*10*----2
//  |\          /|
//  | ---*20*--- |
// *10*         *10*
//  |            |
//  | ---*10*--- |
//  |/          \|
//  3----*20*----4
//   \          /
//    ---*20*---
//
*/
class ParallelAdjRingTopologyFixture : public ::testing::Test {
 public:
  ParallelAdjRingTopologyFixture() : spfSolver(false) {}

 protected:
  void
  SetUp() override {
    // R1 -> R2
    adj12_1 =
        createAdjacency("2", "2/1", "1/1", "fe80::2:1", "192.168.2.1", 10, 0);
    adj12_2 =
        createAdjacency("2", "2/2", "1/2", "fe80::2:2", "192.168.2.2", 10, 0);
    adj12_3 =
        createAdjacency("2", "2/3", "1/3", "fe80::2:3", "192.168.2.3", 20, 0);
    // R1 -> R3
    adj13_1 =
        createAdjacency("3", "3/1", "1/1", "fe80::3:1", "192.168.3.1", 10, 0);

    // R2 -> R1
    adj21_1 =
        createAdjacency("1", "1/1", "2/1", "fe80::1:1", "192.168.1.1", 10, 0);
    adj21_2 =
        createAdjacency("1", "1/2", "2/2", "fe80::1:2", "192.168.1.2", 10, 0);
    adj21_3 =
        createAdjacency("1", "1/3", "2/3", "fe80::1:3", "192.168.1.3", 20, 0);
    // R2 -> R4
    adj24_1 =
        createAdjacency("4", "4/1", "2/1", "fe80::4:1", "192.168.4.1", 10, 0);

    // R3 -> R1
    adj31_1 =
        createAdjacency("1", "1/1", "3/1", "fe80::1:1", "192.168.1.1", 10, 0);
    // R3 -> R4
    adj34_1 =
        createAdjacency("4", "4/1", "3/1", "fe80::4:1", "192.168.4.1", 10, 0);
    adj34_2 =
        createAdjacency("4", "4/2", "3/2", "fe80::4:2", "192.168.4.2", 20, 0);
    adj34_3 =
        createAdjacency("4", "4/3", "3/3", "fe80::4:3", "192.168.4.3", 20, 0);

    // R4 -> R2
    adj42_1 =
        createAdjacency("2", "2/1", "4/1", "fe80::2:1", "192.168.2.1", 10, 0);
    adj43_1 =
        createAdjacency("3", "3/1", "4/1", "fe80::3:1", "192.168.3.1", 10, 0);
    adj43_2 =
        createAdjacency("3", "3/2", "4/2", "fe80::3:2", "192.168.3.2", 20, 0);
    adj43_3 =
        createAdjacency("3", "3/3", "4/3", "fe80::3:3", "192.168.3.3", 20, 0);

    adjacencyDb1 = createAdjDb("1", {adj12_1, adj12_2, adj12_3, adj13_1}, 0);

    adjacencyDb2 = createAdjDb("2", {adj21_1, adj21_2, adj21_3, adj24_1}, 0);

    adjacencyDb3 = createAdjDb("3", {adj31_1, adj34_1, adj34_2, adj34_3}, 0);

    adjacencyDb4 = createAdjDb("4", {adj42_1, adj43_1, adj43_2, adj43_3}, 0);

    // Adjacency db's

    EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1));
    EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb2));
    EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3));
    EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb4));

    // Prefix db's

    EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
    EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));
    EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb3));
    EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb4));
  }

  thrift::Adjacency adj12_1, adj12_2, adj12_3, adj13_1, adj21_1, adj21_2,
      adj21_3, adj24_1, adj31_1, adj34_1, adj34_2, adj34_3, adj42_1, adj43_1,
      adj43_2, adj43_3;
  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  SpfSolverOld spfSolver;
};

TEST_F(ParallelAdjRingTopologyFixture, ShortestPathTest) {
  auto routeMap =
      getRouteMap(spfSolver, false /* shortest path */, {"1", "2", "3", "4"});

  // validate router 1

  EXPECT_THAT(
      routeMap[make_pair("1", toString(addr4))],
      AnyOf(Eq(NextHops({make_pair(toNextHop(adj12_2), 20)})),
            Eq(NextHops({make_pair(toNextHop(adj13_1), 20)}))));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({make_pair(toNextHop(adj13_1), 10)}));

  EXPECT_THAT(
      routeMap[make_pair("1", toString(addr2))],
      AnyOf(Eq(NextHops({make_pair(toNextHop(adj12_2), 10)})),
            Eq(NextHops({make_pair(toNextHop(adj12_1), 10)}))));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({make_pair(toNextHop(adj24_1), 10)}));

  EXPECT_THAT(
      routeMap[make_pair("2", toString(addr3))],
      AnyOf(Eq(NextHops({make_pair(toNextHop(adj21_2), 20)})),
            Eq(NextHops({make_pair(toNextHop(adj24_1), 20)}))));

  EXPECT_THAT(
      routeMap[make_pair("2", toString(addr1))],
      AnyOf(Eq(NextHops({make_pair(toNextHop(adj21_2), 10)})),
            Eq(NextHops({make_pair(toNextHop(adj21_1), 10)}))));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({make_pair(toNextHop(adj34_1), 10)}));

  EXPECT_THAT(
      routeMap[make_pair("3", toString(addr2))],
      AnyOf(Eq(NextHops({make_pair(toNextHop(adj31_1), 20)})),
            Eq(NextHops({make_pair(toNextHop(adj34_1), 20)}))));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({make_pair(toNextHop(adj31_1), 10)}));

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({make_pair(toNextHop(adj43_1), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({make_pair(toNextHop(adj42_1), 10)}));

  EXPECT_THAT(
      routeMap[make_pair("4", toString(addr1))],
      AnyOf(Eq(NextHops({make_pair(toNextHop(adj42_1), 20)})),
            Eq(NextHops({make_pair(toNextHop(adj43_1), 20)}))));
}

//
// Use the same topology, but test multi-path routing
//
TEST_F(ParallelAdjRingTopologyFixture, MultiPathTest) {
  auto routeMap =
      getRouteMap(spfSolver, true /* multipath */, {"1", "2", "3", "4"});

  // validate router 1

  // adj "2/3" is also selected in spite of large metric
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops({make_pair(toNextHop(adj12_1), 20),
                make_pair(toNextHop(adj12_2), 20),
                make_pair(toNextHop(adj12_3), 30),
                make_pair(toNextHop(adj13_1), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({make_pair(toNextHop(adj13_1), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({make_pair(toNextHop(adj12_1), 10),
                make_pair(toNextHop(adj12_2), 10),
                make_pair(toNextHop(adj12_3), 20)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({make_pair(toNextHop(adj24_1), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({make_pair(toNextHop(adj21_1), 20),
                make_pair(toNextHop(adj21_2), 20),
                make_pair(toNextHop(adj21_3), 30),
                make_pair(toNextHop(adj24_1), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({make_pair(toNextHop(adj21_1), 10),
                make_pair(toNextHop(adj21_2), 10),
                make_pair(toNextHop(adj21_3), 20)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({make_pair(toNextHop(adj34_1), 10),
                make_pair(toNextHop(adj34_2), 20),
                make_pair(toNextHop(adj34_3), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({make_pair(toNextHop(adj31_1), 20),
                make_pair(toNextHop(adj34_1), 20),
                make_pair(toNextHop(adj34_2), 30),
                make_pair(toNextHop(adj34_3), 30)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({make_pair(toNextHop(adj31_1), 10)}));

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({make_pair(toNextHop(adj43_1), 10),
                make_pair(toNextHop(adj43_2), 20),
                make_pair(toNextHop(adj43_3), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({make_pair(toNextHop(adj42_1), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({make_pair(toNextHop(adj42_1), 20),
                make_pair(toNextHop(adj43_1), 20),
                make_pair(toNextHop(adj43_2), 30),
                make_pair(toNextHop(adj43_3), 30)}));
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
addAdj(int i, int j, string ifName, vector<thrift::Adjacency>& adjs, int n) {
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
      0 /* node-label */,
      false /* overload-bit */,
      100,
      10000 /* timestamp */,
      1 /* weight */,
      "" /* otherIfName */));
}

string
nodeToPrefixV6(int node) {
  return folly::sformat("::ffff:10.1.{}.{}/128", node / 256, node % 256);
}

void
createGrid(SpfSolverOld& spfSolver, int n) {
  LOG(INFO) << "grid: " << n << " by " << n;
  // confined bcoz of min("fe80::{}", "192.168.{}.{}", "::ffff:10.1.{}.{}")
  EXPECT_TRUE(n * n < 10000) << "n is too large";

  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      auto node = i * n + j;
      auto nodeName = folly::sformat("{}", node);

      // adjacency
      vector<thrift::Adjacency> adjs;
      addAdj(i, j + 1, "0/1", adjs, n);
      addAdj(i - 1, j, "0/2", adjs, n);
      addAdj(i, j - 1, "0/3", adjs, n);
      addAdj(i + 1, j, "0/4", adjs, n);
      auto adjacencyDb = createAdjDb(nodeName, adjs, 0);
      spfSolver.updateAdjacencyDatabase(adjacencyDb);

      // prefix
      auto addrV6 = toIpPrefix(nodeToPrefixV6(node));
      spfSolver.updatePrefixDatabase(createPrefixDb(
          nodeName, {{FRAGILE, addrV6, thrift::PrefixType::LOOPBACK, {}}}));
    }
  }
}

class GridTopologyFixture : public ::testing::TestWithParam<int> {
 public:
  GridTopologyFixture() : spfSolver(false) {}

 protected:
  void
  SetUp() override {
    n = GetParam();
    createGrid(spfSolver, n);
  }

  // n * n grid
  int n;
  SpfSolverOld spfSolver;
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

  auto routeMap = getRouteMap(spfSolver, false /* shortest path */, allNodes);

  int src, dst;
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
  EXPECT_EQ(gridDistance(src, dst, n), nextHops.begin()->second);

  // secondary diagnal
  src = n - 1;
  dst = n * (n - 1);
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), nextHops.begin()->second);

  // 2) from origin (i.e., node 0) to random inner node
  src = 0;
  dst = folly::Random::rand32() % (n * n - 1) + 1;
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), nextHops.begin()->second);

  // 3) from one random node to another
  src = folly::Random::rand32() % (n * n);
  while ((dst = folly::Random::rand32() % (n * n)) == src) {
  }
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), nextHops.begin()->second);
}

// measure SPF execution time for large networks
TEST(GridTopology, StressTest) {
  if (!FLAGS_stress_test) {
    return;
  }
  SpfSolverOld spfSolver(false);
  createGrid(spfSolver, 50);
  spfSolver.buildMultiPaths("0");
}

//
// Start the decision thread and simulate KvStore communications
// Expect proper RouteDatabase publications to appear
//
class DecisionOldTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    kvStorePub.bind(fbzmq::SocketUrl{"inproc://kvStore-pub"});
    kvStoreRep.bind(fbzmq::SocketUrl{"inproc://kvStore-rep"});

    decision = make_shared<DecisionOld>(
        "1", /* node name */
        true, /* enable v4 */
        AdjacencyDbMarker{"adj:"},
        PrefixDbMarker{"prefix:"},
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(250),
        KvStoreLocalCmdUrl{"inproc://kvStore-rep"},
        KvStoreLocalPubUrl{"inproc://kvStore-pub"},
        DecisionCmdUrl{"inproc://decision-rep"},
        DecisionPubUrl{"inproc://decision-pub"},
        MonitorSubmitUrl{"inproc://monitor-rep"},
        zeromqContext);

    decisionThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "DecisionOld thread starting";
      decision->run();
      LOG(INFO) << "DecisionOld thread finishing";
    });
    decision->waitUntilRunning();

    const int hwm = 1000;
    decisionPub.setSockOpt(ZMQ_SNDHWM, &hwm, sizeof(hwm)).value();
    decisionPub.setSockOpt(ZMQ_SUBSCRIBE, "", 0).value();
    decisionPub.connect(fbzmq::SocketUrl{"inproc://decision-pub"});
    decisionReq.connect(fbzmq::SocketUrl{"inproc://decision-rep"});

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
    auto maybeDumpReq = kvStoreRep.recvThriftObj<thrift::Request>(serializer);
    EXPECT_FALSE(maybeDumpReq.hasError());
    auto dumpReq = maybeDumpReq.value();
    EXPECT_EQ(thrift::Command::KEY_DUMP, dumpReq.cmd);

    // send back routeDb reply
    kvStoreRep.sendThriftObj(publication, serializer);
  }

  // publish routeDb
  void
  publishRouteDb(const thrift::Publication& publication) {
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
      prefixEntries.emplace_back(
          FRAGILE, prefix, thrift::PrefixType::LOOPBACK, "");
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
  std::shared_ptr<DecisionOld> decision{nullptr};

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

TEST_F(DecisionOldTestFixture, BasicOperations) {
  //
  // publish the link state info to KvStore
  //

  auto publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {});

  replyInitialSyncReq(publication);
  auto routeDb = recvMyRouteDb(decisionPub, "1", serializer);
  EXPECT_EQ(1, routeDb.routes.size());
  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({make_pair(toNextHop(adj12), 10)}));

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
      {});

  publishRouteDb(publication);

  // validate routers

  // receive my local DecisionOld routeDb publication
  routeDb = recvMyRouteDb(decisionPub, "1" /* node name */, serializer);
  EXPECT_EQ(2, routeDb.routes.size());
  fillRouteMap("1", routeMap, routeDb);
  // 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({make_pair(toNextHop(adj12), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({make_pair(toNextHop(adj12), 20)}));

  // dump other nodes' routeDB
  auto routeDbMap = dumpRouteDatabase(decisionReq, {"2", "3"}, serializer);
  EXPECT_EQ(2, routeDbMap["2"].routes.size());
  EXPECT_EQ(2, routeDbMap["3"].routes.size());
  for (auto kv : routeDbMap) {
    fillRouteMap(kv.first, routeMap, kv.second);
  }

  // 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({make_pair(toNextHop(adj21), 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({make_pair(toNextHop(adj23), 10)}));

  // 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({make_pair(toNextHop(adj32), 20)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({make_pair(toNextHop(adj32), 10)}));

  // remove 3
  publication = thrift::Publication(
      FRAGILE, thrift::KeyVals{}, {"adj:3", "prefix:3"} /* expired keys */);

  publishRouteDb(publication);
  routeDb = recvMyRouteDb(decisionPub, "1" /* node name */, serializer);
  EXPECT_EQ(1, routeDb.routes.size());
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({make_pair(toNextHop(adj12), 10)}));
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

TEST_F(DecisionOldTestFixture, ParallelLinks) {
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
      {});

  replyInitialSyncReq(publication);
  auto routeDb = recvMyRouteDb(decisionPub, "1", serializer);
  EXPECT_EQ(1, routeDb.routes.size());
  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({make_pair(toNextHop(adj12_1), 100),
                make_pair(toNextHop(adj12_2), 800)}));

  publication = thrift::Publication(
      FRAGILE, {{"adj:2", createAdjValue("2", 2, {adj21_2})}}, {});

  publishRouteDb(publication);
  // receive my local DecisionOld routeDb publication
  routeDb = recvMyRouteDb(decisionPub, "1" /* node name */, serializer);
  EXPECT_EQ(1, routeDb.routes.size());
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({make_pair(toNextHop(adj12_2), 800)}));
}

// The following topology is used:
//
// 1---2---3---4
//
// We upload the link 1---2 with the initial sync and later publish
// the 2---3 & 3---4 link information. We expect it to trigger SPF only once.
//
TEST_F(DecisionOldTestFixture, PubDebouncing) {
  //
  // publish the link state info to KvStore
  //

  auto publication = thrift::Publication(
      FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {});

  auto counters = decision->getCounters();
  EXPECT_EQ(0, counters["decision.paths_build_requests.count.0"]);
  EXPECT_EQ(0, counters["decision.route_build_requests.count.0"]);
  replyInitialSyncReq(publication);

  /* sleep override */
  // wait for SPF to finish
  std::this_thread::sleep_for(debounceTimeout / 2);
  // validate SPF after initial sync, no rebouncing here
  counters = decision->getCounters();
  EXPECT_EQ(1, counters["decision.paths_build_requests.count.0"]);
  EXPECT_EQ(1, counters["decision.route_build_requests.count.0"]);

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
      {});

  publishRouteDb(publication);

  // we simulate adding a new router R4

  // Some tricks here; we need to bump the time-stamp on router 3's data, so
  // it can override existing;

  publication = thrift::Publication(
      FRAGILE,
      {{"adj:4", createAdjValue("4", 1, {adj43})},
       {"adj:3", createAdjValue("3", 5, {adj32, adj34})}},
      {});

  publishRouteDb(publication);

  /* sleep override */
  // wait for debouncing to kick in
  std::this_thread::sleep_for(debounceTimeout);
  // validate SPF
  counters = decision->getCounters();
  EXPECT_EQ(2, counters["decision.paths_build_requests.count.0"]);
  EXPECT_EQ(2, counters["decision.route_build_requests.count.0"]);

  //
  // Only publish prefix updates
  //
  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 1, {addr4})}},
      {});
  publishRouteDb(publication);

  /* sleep override */
  // wait for route rebuilding to finish
  std::this_thread::sleep_for(debounceTimeout / 2);
  counters = decision->getCounters();
  EXPECT_EQ(2, counters["decision.paths_build_requests.count.0"]);
  EXPECT_EQ(3, counters["decision.route_build_requests.count.0"]);

  //
  // publish adj updates right after prefix updates
  // DecisionOld is supposed to only trigger spf recalculation

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 2, {addr4, addr5})}},
      {});
  publishRouteDb(publication);

  publication = thrift::Publication(
      FRAGILE,
      {{"adj:2", createAdjValue("2", 5, {adj21})}},
      {});
  publishRouteDb(publication);

  /* sleep override */
  // wait for SPF to finish
  std::this_thread::sleep_for(debounceTimeout);
  counters = decision->getCounters();
  EXPECT_EQ(3, counters["decision.paths_build_requests.count.0"]);
  EXPECT_EQ(4, counters["decision.route_build_requests.count.0"]);

  //
  // publish multiple prefix updates in a row
  // DecisionOld is supposed to process prefix update only once

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 5, {addr4})}},
      {});
  publishRouteDb(publication);

  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 7, {addr4, addr6})}},
      {});
  publishRouteDb(publication);

  publication = thrift::Publication(
      FRAGILE,
      {{"prefix:4", createPrefixValue("4", 8, {addr4, addr5, addr6})}},
      {});
  publishRouteDb(publication);

  /* sleep override */
  // wait for route rebuilding to finish
  std::this_thread::sleep_for(debounceTimeout);
  counters = decision->getCounters();
  EXPECT_EQ(3, counters["decision.paths_build_requests.count.0"]);
  // only 1 request shall be processed
  EXPECT_EQ(5, counters["decision.route_build_requests.count.0"]);
}

//
// Send unrelated key-value pairs to DecisionOld
// Make sure they do not trigger SPF runs, but rather ignored
//
TEST_F(DecisionOldTestFixture, NoSpfOnIrrelevantPublication) {
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
      {});

  auto counters = decision->getCounters();
  EXPECT_EQ(0, counters["decision.paths_build_requests.count.0"]);

  replyInitialSyncReq(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // make sure the counter did not increment
  counters = decision->getCounters();
  EXPECT_EQ(0, counters["decision.paths_build_requests.count.0"]);
}

//
// Send duplicate key-value pairs to DecisionOld
// Make sure subsquent duplicates are ignored.
//
TEST_F(DecisionOldTestFixture, NoSpfOnDuplicatePublication) {
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
      {});

  auto counters = decision->getCounters();
  EXPECT_EQ(0, counters["decision.paths_build_requests.count.0"]);

  replyInitialSyncReq(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // make sure counter is incremented
  counters = decision->getCounters();
  EXPECT_EQ(1, counters["decision.paths_build_requests.count.0"]);

  // Send same publication again to DecisionOld using pub socket
  publishRouteDb(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeout);

  // make sure counter is not incremented
  counters = decision->getCounters();
  EXPECT_EQ(1, counters["decision.paths_build_requests.count.0"]);
}

/**
 * Loop-alternate path testing. Topology is described as follows
 *          10
 *  node1 --------- node2
 *    \_           _/
 *  8   \_ node3 _/  9
 *
 */
TEST_F(DecisionOldTestFixture, LoopFreeAlternatePaths) {
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
      {});

  replyInitialSyncReq(publication);

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
      NextHops(
          {make_pair(toNextHop(adj12), 10), make_pair(toNextHop(adj13), 17)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops(
          {make_pair(toNextHop(adj12), 19), make_pair(toNextHop(adj13), 8)}));

  // 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops(
          {make_pair(toNextHop(adj21), 10), make_pair(toNextHop(adj23), 17)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops(
          {make_pair(toNextHop(adj21), 18), make_pair(toNextHop(adj23), 9)}));

  // 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops(
          {make_pair(toNextHop(adj31), 8), make_pair(toNextHop(adj32), 19)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops(
          {make_pair(toNextHop(adj31), 18), make_pair(toNextHop(adj32), 9)}));

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
      {});

  // Send same publication again to DecisionOld using pub socket
  publishRouteDb(publication);

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
      NextHops(
          {make_pair(toNextHop(adj12), 100), make_pair(toNextHop(adj13), 17)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops(
          {make_pair(toNextHop(adj12), 109), make_pair(toNextHop(adj13), 8)}));

  // 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops(
          {make_pair(toNextHop(adj21), 100), make_pair(toNextHop(adj23), 17)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops(
          {make_pair(toNextHop(adj21), 108), make_pair(toNextHop(adj23), 9)}));

  // 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({make_pair(toNextHop(adj31), 8)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({make_pair(toNextHop(adj32), 9)}));
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
TEST_F(DecisionOldTestFixture, DuplicatePrefixes) {
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
      {});

  replyInitialSyncReq(publication);

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
  EXPECT_EQ(2, routeMapList["1"].routes.size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {make_pair(toNextHop(adj12), 10), make_pair(toNextHop(adj13), 10)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].routes.size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({make_pair(toNextHop(adj21), 10)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].routes.size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({make_pair(toNextHop(adj31), 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].routes.size());
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({make_pair(toNextHop(adj41), 15)}));

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
      {});

  // Send same publication again to DecisionOld using pub socket
  publishRouteDb(publication);

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
  EXPECT_EQ(2, routeMapList["1"].routes.size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {make_pair(toNextHop(adj13), 10), make_pair(toNextHop(adj12), 100)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].routes.size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({make_pair(toNextHop(adj21), 100)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].routes.size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({make_pair(toNextHop(adj31), 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].routes.size());
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops(
          {make_pair(toNextHop(adj41), 15), make_pair(toNextHop(adj41), 105)}));
}

/**
 * Tests reliability of DecisionOld SUB socket. We overload SUB socket with lot
 * of messages and make sure none of them are lost. We make decision compute
 * routes for a large network topology taking good amount of CPU time. We
 * do not try to validate routes here instead we validate messages processed
 * by decision and message sent by us.
 *
 * Topology consists of 1000 nodes linear where node-i connects to 3 nodes
 * before it and 3 nodes after it.
 *
 */
TEST_F(DecisionOldTestFixture, DecisionOldSubReliability) {
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
  replyInitialSyncReq(initialPub);

  //
  // Hammer DecisionOld with lot of duplicate publication for 2 * ThrottleTimeout
  // We want to ensure that we hammer DecisionOld for atleast once during it's
  // SPF run. This will cause lot of pending publications on DecisionOld. This
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
    publishRouteDb(duplicatePub);
  }

  // Receive RouteUpdate from DecisionOld
  auto routes1 = recvMyRouteDb(decisionPub, "1", serializer);
  EXPECT_EQ(999, routes1.routes.size()); // Route to all nodes except mine

  //
  // Wait until all pending updates are finished
  //
  std::this_thread::sleep_for(std::chrono::seconds(30));

  //
  // Advertise prefix update. DecisionOld gonna take some
  // good amount of time to process this last update (as it has many queued
  // updates).
  //
  thrift::Publication newPub;
  auto newAddr = toIpPrefix("face:b00c:babe::1/128");
  newPub.keyVals["prefix:1"] = createPrefixValue("1", 2, {newAddr});
  LOG(INFO) << "Advertising prefix update";
  publishRouteDb(newPub);
  // Receive RouteUpdate from DecisionOld
  auto routes2 = recvMyRouteDb(decisionPub, "1", serializer);
  EXPECT_EQ(999, routes2.routes.size()); // Route to all nodes except mine
  //
  // Verify counters information
  //

  const int64_t adjUpdateCnt = 1000 /* initial */;
  const int64_t prefixUpdateCnt = totalSent + 1000 /* initial */ + 1 /* end */;
  auto counters = decision->getCounters();
  EXPECT_EQ(1, counters["decision.paths_build_requests.count.0"]);
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
