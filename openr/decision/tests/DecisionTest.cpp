/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Optional.h>
#include <folly/Random.h>
#include <folly/futures/Promise.h>
#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/config/tests/Utils.h>
#include <openr/decision/Decision.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

DEFINE_bool(stress_test, false, "pass this to run the stress test");

using namespace std;
using namespace openr;
using namespace testing;

namespace fb303 = facebook::fb303;

using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;

namespace {

const auto kDefaultArea{openr::thrift::KvStore_constants::kDefaultArea()};

/// R1 -> R2, R3
const auto adj12 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
const auto adj13 =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 10, 100003);
const auto adj14 =
    createAdjacency("4", "1/4", "4/1", "fe80::4", "192.168.0.4", 10, 100004);
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
const auto adj41 =
    createAdjacency("1", "4/1", "1/4", "fe80::1", "192.168.0.1", 10, 100001);
const auto adj42 =
    createAdjacency("2", "4/2", "2/4", "fe80::2", "192.168.0.2", 10, 100002);
const auto adj43 =
    createAdjacency("3", "4/3", "3/4", "fe80::3", "192.168.0.3", 10, 100003);
// R5 -> R4
const auto adj54 =
    createAdjacency("4", "5/4", "4/5", "fe80::4", "192.168.0.4", 10, 100001);

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

const auto bgpAddr1 = toIpPrefix("2401:1::10.1.1.1/32");
const auto bgpAddr2 = toIpPrefix("2401:2::10.2.2.2/32");
const auto bgpAddr3 = toIpPrefix("2401:3::10.3.3.3/32");
const auto bgpAddr4 = toIpPrefix("2401:4::10.4.4.4/32");
const auto bgpAddr1V4 = toIpPrefix("10.11.1.1/16");
const auto bgpAddr2V4 = toIpPrefix("10.22.2.2/16");
const auto bgpAddr3V4 = toIpPrefix("10.33.3.3/16");
const auto bgpAddr4V4 = toIpPrefix("10.43.4.4/16");

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

const thrift::NextHopThrift labelPopNextHop{createNextHop(
    toBinaryAddress(folly::IPAddressV6("::")),
    std::nullopt /* ifName */,
    0 /* metric */,
    labelPopAction,
    false /* useNonShortestRoute */)};

// timeout to wait until decision debounce
// (i.e. spf recalculation, route rebuild) finished
const std::chrono::milliseconds debounceTimeoutMin{10};
const std::chrono::milliseconds debounceTimeoutMax{500};

thrift::PrefixDatabase
createPrefixDbWithKspfAlgo(
    thrift::PrefixDatabase const& prefixDb,
    std::optional<thrift::PrefixType> prefixType = std::nullopt,
    std::optional<thrift::IpPrefix> prefix = std::nullopt,
    std::optional<uint32_t> prependLabel = std::nullopt) {
  thrift::PrefixDatabase newPrefixDb = prefixDb;

  for (auto& p : newPrefixDb.prefixEntries) {
    p.forwardingType = thrift::PrefixForwardingType::SR_MPLS;
    p.forwardingAlgorithm = thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    if (prefixType.has_value() and
        (prefixType.value() == thrift::PrefixType::BGP) and
        (not prefix.has_value())) {
      p.type = thrift::PrefixType::BGP;
      p.mv_ref() = thrift::MetricVector();
    }
  }

  if (prefix.has_value()) {
    thrift::PrefixEntry entry;
    entry.prefix = prefix.value();
    entry.forwardingType = thrift::PrefixForwardingType::SR_MPLS;
    entry.forwardingAlgorithm = thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    entry.mv_ref() = thrift::MetricVector();
    entry.type = thrift::PrefixType::BGP;
    if (prependLabel.has_value()) {
      entry.prependLabel_ref() = prependLabel.value();
    }
    newPrefixDb.prefixEntries.push_back(entry);
    return newPrefixDb;
  }

  return newPrefixDb;
}

thrift::NextHopThrift
createNextHopFromAdj(
    thrift::Adjacency adj,
    bool isV4,
    int32_t metric,
    std::optional<thrift::MplsAction> mplsAction = std::nullopt,
    bool useNonShortestRoute = false,
    const std::string& area = kDefaultArea) {
  return createNextHop(
      isV4 ? adj.nextHopV4 : adj.nextHopV6,
      adj.ifName,
      metric,
      std::move(mplsAction),
      useNonShortestRoute,
      area);
}

// Note: use unordered_set bcoz paths in a route can be in arbitrary order
using NextHops = unordered_set<thrift::NextHopThrift>;
using RouteMap = unordered_map<
    pair<string /* node name */, string /* prefix or label */>,
    NextHops>;

using PrefixRoutes = unordered_map<
    pair<string /* node name */, string /* prefix or label */>,
    thrift::UnicastRoute>;

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
    auto routeDb = spfSolver.buildRouteDb(node);
    if (not routeDb.has_value()) {
      continue;
    }

    EXPECT_EQ(node, routeDb->thisNodeName);
    fillRouteMap(node, routeMap, routeDb.value());
  }

  return routeMap;
}

// Note: routeMap will be modified
void
fillPrefixRoutes(
    const string& node,
    PrefixRoutes& prefixRoutes,
    const thrift::RouteDatabase& routeDb) {
  for (auto const& route : routeDb.unicastRoutes) {
    auto prefix = toString(route.dest);
    prefixRoutes[make_pair(node, prefix)] = route;
  }
}

PrefixRoutes
getUnicastRoutes(SpfSolver& spfSolver, const vector<string>& nodes) {
  PrefixRoutes prefixRoutes;

  for (string const& node : nodes) {
    auto routeDb = spfSolver.buildRouteDb(node);
    if (not routeDb.has_value()) {
      continue;
    }

    EXPECT_EQ(node, routeDb->thisNodeName);
    fillPrefixRoutes(node, prefixRoutes, routeDb.value());
  }

  return prefixRoutes;
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

void
printRouteDb(const std::optional<thrift::RouteDatabase>& routeDb) {
  for (const auto ucRoute : routeDb.value().unicastRoutes) {
    LOG(INFO) << "dest: " << toString(ucRoute.dest);
    if (ucRoute.adminDistance_ref().has_value()) {
      LOG(INFO) << "ad_dis: "
                << static_cast<int>(ucRoute.adminDistance_ref().value());
    }
    if (ucRoute.prefixType_ref().has_value()) {
      LOG(INFO) << "prefix_type: "
                << static_cast<int>(ucRoute.prefixType_ref().value());
    }

    LOG(INFO) << "doNotInstall: " << ucRoute.doNotInstall;

    for (const auto nh : ucRoute.nextHops) {
      LOG(INFO) << "nexthops: " << toString(nh);
    }
    if (ucRoute.bestNexthop_ref().has_value()) {
      const auto nh = ucRoute.bestNexthop_ref().value();
      LOG(INFO) << "best next hop: " << toString(nh);
    }
  }
}

} // anonymous namespace

//
// This test aims to verify counter reporting from Decision module
//
TEST(SpfSolver, Counters) {
  SpfSolver spfSolver(
      "1" /* nodeName */,
      true /* enableV4 */,
      false /* computeLfaPaths */,
      false /* enableOrderedFib */,
      false /* bgpDryRun */,
      true /* bgpUseIgpMetric */);

  // Verifiy some initial/default counters
  {
    spfSolver.updateGlobalCounters();
    const auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(counters.at("decision.num_nodes"), 1);
  }

  // Node1 connects to 2/3, Node2 connects to 1, Node3 connects to 1
  // Node2 has partial adjacency
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 3 << 20); // invalid mpls label
  auto adjacencyDb4 = createAdjDb("4", {}, 4);
  auto adjacencyDb5 = createAdjDb("5", {adj54}, 5); // Disconnected node
  spfSolver.updateAdjacencyDatabase(adjacencyDb1);
  spfSolver.updateAdjacencyDatabase(adjacencyDb2);
  spfSolver.updateAdjacencyDatabase(adjacencyDb3);
  spfSolver.updateAdjacencyDatabase(adjacencyDb4);

  // Node1 and Node2 has both v4/v6 loopbacks, Node3 has only V6
  auto mplsPrefixEntry1 = createPrefixEntry( // Incompatible forwarding type
      toIpPrefix("10.1.0.0/16"),
      thrift::PrefixType::LOOPBACK,
      "",
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP);
  auto bgpPrefixEntry1 = createPrefixEntry( // Missing loopback
      toIpPrefix("10.2.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.2.0.0/16",
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      std::nullopt,
      thrift::MetricVector{} /* empty metric vector */);
  auto bgpPrefixEntry2 = createPrefixEntry( // Missing metric vector
      toIpPrefix("10.3.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.3.0.0/16",
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      std::nullopt,
      std::nullopt /* missing metric vector */);
  const auto prefixDb1 = createPrefixDb(
      "1", {createPrefixEntry(addr1), createPrefixEntry(addr1V4)});
  const auto prefixDb2 = createPrefixDb(
      "2", {createPrefixEntry(addr2), createPrefixEntry(addr2V4)});
  const auto prefixDb3 = createPrefixDb(
      "3", {createPrefixEntry(addr3), bgpPrefixEntry1, mplsPrefixEntry1});
  const auto prefixDb4 =
      createPrefixDb("4", {createPrefixEntry(addr4), bgpPrefixEntry2});
  spfSolver.updatePrefixDatabase(prefixDb1);
  spfSolver.updatePrefixDatabase(prefixDb2);
  spfSolver.updatePrefixDatabase(prefixDb3);
  spfSolver.updatePrefixDatabase(prefixDb4);

  // Perform SPF run to generate some counters
  const auto routeDb = spfSolver.buildRouteDb("1");
  for (const auto& uniRoute : routeDb.value().unicastRoutes) {
    EXPECT_NE(toString(uniRoute.dest), "10.1.0.0/16");
  }

  // Verify counters
  spfSolver.updateGlobalCounters();
  const auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.num_partial_adjacencies"), 1);
  EXPECT_EQ(counters.at("decision.num_complete_adjacencies"), 2);
  EXPECT_EQ(counters.at("decision.num_nodes"), 3);
  EXPECT_EQ(counters.at("decision.num_prefixes"), 9);
  EXPECT_EQ(counters.at("decision.num_nodes_v4_loopbacks"), 2);
  EXPECT_EQ(counters.at("decision.num_nodes_v6_loopbacks"), 4);
  EXPECT_EQ(counters.at("decision.no_route_to_prefix.count.60"), 1);
  EXPECT_EQ(counters.at("decision.missing_loopback_addr.sum.60"), 1);
  EXPECT_EQ(counters.at("decision.incompatible_forwarding_type.count.60"), 1);
  EXPECT_EQ(counters.at("decision.skipped_unicast_route.count.60"), 1);
  EXPECT_EQ(counters.at("decision.skipped_mpls_route.count.60"), 1);
  EXPECT_EQ(counters.at("decision.no_route_to_label.count.60"), 1);

  // fully disconnect node 2
  spfSolver.updateAdjacencyDatabase(createAdjDb("1", {adj13}, 1));
  spfSolver.updateGlobalCounters();
  EXPECT_EQ(
      fb303::fbData->getCounters().at("decision.num_partial_adjacencies"), 0);
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
    auto routeDb = spfSolver.buildRouteDb(node);
    ASSERT_TRUE(routeDb.has_value());
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

  auto routeDb = spfSolver.buildRouteDb("1");
  ASSERT_TRUE(routeDb.has_value());
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

  auto routeDb = spfSolver.buildRouteDb("1");
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(0, routeDb->unicastRoutes.size());

  routeDb = spfSolver.buildRouteDb("2");
  ASSERT_TRUE(routeDb.has_value());
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

  auto routeDb = spfSolver.buildRouteDb("1");
  EXPECT_FALSE(routeDb.has_value());

  routeDb = spfSolver.buildRouteDb("2");
  EXPECT_FALSE(routeDb.has_value());
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
    EXPECT_TRUE(res.second);
  }
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));

  //
  // dump routes for both nodes, expect 4 route entries (1 unicast, 3 label) on
  // each (node1-label, node2-label and adjacency-label)
  //

  auto routeDb = spfSolver.buildRouteDb("1");
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildRouteDb("2");
  ASSERT_TRUE(routeDb.has_value());
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

  routeDb = spfSolver.buildRouteDb("1");
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildRouteDb("2");
  ASSERT_TRUE(routeDb.has_value());
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
    EXPECT_TRUE(res.second);
  }

  //
  // dump routes for both nodes, expect 4 route entries (1 unicast, 3 label) on
  // each (node1-label, node2-label and adjacency-label)
  //

  routeDb = spfSolver.buildRouteDb("1");
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ("1", routeDb->thisNodeName);
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildRouteDb("2");
  ASSERT_TRUE(routeDb.has_value());
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
    EXPECT_TRUE(res.second);
  }

  // Change nodeLabel.
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
    EXPECT_TRUE(res.second);
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
      std::make_pair(true, true),
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
  prefixDb1WithBGP.prefixEntries.push_back(createPrefixEntry(
      bgpPrefix1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      false,
      mv1,
      std::nullopt));

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1WithBGP));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));

  auto routeDb = spfSolver.buildRouteDb("2");
  thrift::UnicastRoute route1(
      FRAGILE,
      bgpPrefix1,
      thrift::AdminDistance::EBGP,
      {createNextHopFromAdj(adj21, false, adj21.metric)},
      thrift::PrefixType::BGP,
      data1,
      false,
      {createNextHop(addr1.prefixAddress)});
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(2));
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::Contains(route1));

  // add the prefix to node2 with the same metric vector. we expect the bgp
  // route to be gone since both nodes have same metric vector we can't
  // determine a best path
  prefixDb2WithBGP.prefixEntries.push_back(createPrefixEntry(
      bgpPrefix1,
      thrift::PrefixType::BGP,
      data2,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      false,
      mv2,
      std::nullopt));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));
  routeDb = spfSolver.buildRouteDb("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(1));

  // decrease the one of second node's metrics and expect to see the route
  // toward just the first
  prefixDb2WithBGP.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .metric.front()--;
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));
  routeDb = spfSolver.buildRouteDb("2");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(2));
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::Contains(route1));

  // now make 2 better
  prefixDb2WithBGP.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .metric.front() += 2;
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));

  thrift::UnicastRoute route2(
      FRAGILE,
      bgpPrefix1,
      thrift::AdminDistance::EBGP,
      {createNextHopFromAdj(adj12, false, adj12.metric)},
      thrift::PrefixType::BGP,
      data2,
      false,
      createNextHop(addr2.prefixAddress));

  routeDb = spfSolver.buildRouteDb("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(2));
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::Contains(route2));

  // now make that a tie break for a multipath route
  prefixDb1WithBGP.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;
  prefixDb2WithBGP.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1WithBGP));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBGP));

  // 1 and 2 will not program BGP route
  EXPECT_THAT(
      spfSolver.buildRouteDb("1").value().unicastRoutes, testing::SizeIs(1));

  // 3 will program the BGP route towards both
  routeDb = spfSolver.buildRouteDb("3");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(3));
  EXPECT_THAT(
      routeDb.value().unicastRoutes,
      testing::Contains(AllOf(
          Field(&thrift::UnicastRoute::dest, bgpPrefix1),
          Truly([&data2](auto i) { return i.data_ref() == data2; }),
          Field(
              &thrift::UnicastRoute::nextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj31, false, 10))))));

  // dicsonnect the network, each node will consider it's BGP route the best,
  // and thus not program anything
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(createAdjDb("1", {}, 0)).first);
  EXPECT_THAT(
      spfSolver.buildRouteDb("1").value().unicastRoutes,
      testing::AllOf(
          testing::Not(testing::Contains(route1)),
          testing::Not(testing::Contains(route2))));
  EXPECT_THAT(
      spfSolver.buildRouteDb("2").value().unicastRoutes,
      testing::AllOf(
          testing::Not(testing::Contains(route1)),
          testing::Not(testing::Contains(route2))));
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
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* enableV4 */,
      false /* computeLfaPaths */,
      false /* enableOrderedFib */,
      false /* bgpDryRun */,
      true /* bgpUseIgpMetric */);

  //
  // Create BGP prefix
  //
  thrift::MetricVector metricVector;
  int64_t numMetrics = 5;
  metricVector.metrics.resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    metricVector.metrics[i].type = i;
    metricVector.metrics[i].priority = i;
    metricVector.metrics[i].op = thrift::CompareType::WIN_IF_PRESENT;
    metricVector.metrics[i].isBestPathTieBreaker = (i == numMetrics - 1);
    metricVector.metrics[i].metric = {i};
  }
  const std::string data1{"data1"};
  const auto bgpPrefix2 = createPrefixEntry(
      addr1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      false,
      metricVector);
  // Make tie breaking metric different
  metricVector.metrics.at(4).metric = {100}; // Make it different
  const auto bgpPrefix3 = createPrefixEntry(
      addr1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      false,
      metricVector);

  //
  // Setup adjacencies
  //
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 0);
  EXPECT_FALSE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb2).first);
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb3).first);

  //
  // Update prefix databases
  //
  auto prefixDb2WithBgp =
      createPrefixDb("2", {createPrefixEntry(addr2), bgpPrefix2});
  auto prefixDb3WithBgp =
      createPrefixDb("3", {createPrefixEntry(addr3), bgpPrefix3});
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2WithBgp));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb3WithBgp));

  //
  // Step-1 prefix1 -> {node2, node3}
  //
  auto routeDb = spfSolver.buildRouteDb("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(3));
  EXPECT_THAT(
      routeDb.value().unicastRoutes,
      testing::Contains(AllOf(
          Field(&thrift::UnicastRoute::dest, addr1),
          Truly([&data1](auto i) { return i.data_ref() == data1; }),
          Field(
              &thrift::UnicastRoute::nextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10),
                  createNextHopFromAdj(adj13, false, 10))))));

  //
  // Increase cost towards node3 to 20; prefix -> {node2}
  //
  adjacencyDb1.adjacencies[1].metric = 20;
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  routeDb = spfSolver.buildRouteDb("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(3));
  EXPECT_THAT(
      routeDb.value().unicastRoutes,
      testing::Contains(AllOf(
          Field(&thrift::UnicastRoute::dest, addr1),
          Truly([&data1](auto i) { return i.data_ref() == data1; }),
          Field(
              &thrift::UnicastRoute::nextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10))))));

  //
  // mark link towards node2 as drained; prefix1 -> {node3}
  // No route towards addr2 (node2's loopback)
  //
  adjacencyDb1.adjacencies[0].isOverloaded = true;
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  routeDb = spfSolver.buildRouteDb("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(2));
  EXPECT_THAT(
      routeDb.value().unicastRoutes,
      testing::Contains(AllOf(
          Field(&thrift::UnicastRoute::dest, addr1),
          Truly([&data1](auto i) { return i.data_ref() == data1; }),
          Field(
              &thrift::UnicastRoute::nextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj13, false, 20))))));

  //
  // Set cost towards node2 to 20 (still drained); prefix1 -> {node3}
  // No route towards addr2 (node2's loopback)
  //
  adjacencyDb1.adjacencies[0].metric = 20;
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  routeDb = spfSolver.buildRouteDb("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(2));
  EXPECT_THAT(
      routeDb.value().unicastRoutes,
      testing::Contains(AllOf(
          Field(&thrift::UnicastRoute::dest, addr1),
          Truly([&data1](auto i) { return i.data_ref() == data1; }),
          Field(
              &thrift::UnicastRoute::nextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj13, false, 20))))));

  //
  // Undrain link; prefix1 -> {node2, node3}
  //
  adjacencyDb1.adjacencies[0].isOverloaded = false;
  EXPECT_TRUE(spfSolver.updateAdjacencyDatabase(adjacencyDb1).first);
  routeDb = spfSolver.buildRouteDb("1");
  EXPECT_THAT(routeDb.value().unicastRoutes, testing::SizeIs(3));
  EXPECT_THAT(
      routeDb.value().unicastRoutes,
      testing::Contains(AllOf(
          Field(&thrift::UnicastRoute::dest, addr1),
          Truly([&data1](auto i) { return i.data_ref() == data1; }),
          Field(
              &thrift::UnicastRoute::nextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 20),
                  createNextHopFromAdj(adj13, false, 20))))));
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
      std::make_pair(!partitioned, true),
      spfSolver.updateAdjacencyDatabase(adjacencyDb2));
  EXPECT_EQ(
      std::make_pair(!partitioned, true),
      spfSolver.updateAdjacencyDatabase(adjacencyDb3));

  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb1));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb2));
  EXPECT_TRUE(spfSolver.updatePrefixDatabase(prefixDb3));

  // route from 1 to 3
  auto routeDb = spfSolver.buildRouteDb("1");
  bool foundRouteV6 = false;
  bool foundRouteNodeLabel = false;
  if (routeDb.has_value()) {
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
  CustomSetUp(
      bool calculateLfas,
      bool useKsp2Ed,
      std::optional<thrift::PrefixType> prefixType = std::nullopt,
      bool createNewBgpRoute = false) {
    std::string nodeName("1");
    spfSolver = std::make_unique<SpfSolver>(nodeName, v4Enabled, calculateLfas);
    adjacencyDb1 = createAdjDb("1", {adj12, adj13, adj14}, 1);
    adjacencyDb2 = createAdjDb("2", {adj21, adj23, adj24}, 2);
    adjacencyDb3 = createAdjDb("3", {adj31, adj32, adj34}, 3);
    adjacencyDb4 = createAdjDb("4", {adj41, adj42, adj43}, 4);

    EXPECT_EQ(
        std::make_pair(false, true),
        spfSolver->updateAdjacencyDatabase(adjacencyDb1));
    EXPECT_EQ(
        std::make_pair(true, true),
        spfSolver->updateAdjacencyDatabase(adjacencyDb2));
    EXPECT_EQ(
        std::make_pair(true, true),
        spfSolver->updateAdjacencyDatabase(adjacencyDb3));
    EXPECT_EQ(
        std::make_pair(true, true),
        spfSolver->updateAdjacencyDatabase(adjacencyDb4));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;

    auto bgp1 = v4Enabled ? bgpAddr1V4 : bgpAddr1;
    auto bgp2 = v4Enabled ? bgpAddr2V4 : bgpAddr2;
    auto bgp3 = v4Enabled ? bgpAddr3V4 : bgpAddr3;
    auto bgp4 = v4Enabled ? bgpAddr4V4 : bgpAddr4;

    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb1,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp1)
                                    : std::nullopt)
            : pdb1));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb2,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp2)
                                    : std::nullopt)
            : pdb2));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb3,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp3)
                                    : std::nullopt)
            : pdb3));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb4,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp4)
                                    : std::nullopt)
            : pdb4));
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  std::unique_ptr<SpfSolver> spfSolver;
};

INSTANTIATE_TEST_CASE_P(
    SimpleRingMeshTopologyInstance,
    SimpleRingMeshTopologyFixture,
    ::testing::Values(
        std::make_tuple(true, std::nullopt),
        std::make_tuple(false, std::nullopt),
        std::make_tuple(true, thrift::PrefixType::BGP),
        std::make_tuple(false, thrift::PrefixType::BGP)));

TEST_P(SimpleRingMeshTopologyFixture, Ksp2EdEcmp) {
  CustomSetUp(
      false /* multipath - ignored */,
      true /* useKsp2Ed */,
      std::get<1>(GetParam()));
  auto routeMap = getRouteMap(*spfSolver, {"1"});

  auto pushCode = thrift::MplsActionCode::PUSH;
  auto push1 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1});
  auto push2 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2});
  auto push3 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3});
  auto push4 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4});
  auto push24 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 4});
  auto push34 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3, 4});
  auto push43 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4, 3});
  auto push13 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1, 3});
  auto push42 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4, 2});
  auto push12 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1, 2});
  auto push31 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3, 1});
  auto push21 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 1});

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj14, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj12, v4Enabled, 20, push4, true),
                createNextHopFromAdj(adj13, v4Enabled, 20, push4, true)}));

  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj14, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj12, v4Enabled, 20, push3, true),
                createNextHopFromAdj(adj14, v4Enabled, 20, push3, true)}));
  // EXPECT_EQ(
  //     routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
  //     NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));
  //
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj13, v4Enabled, 20, push2, true),
                createNextHopFromAdj(adj14, v4Enabled, 20, push2, true)}));
  // EXPECT_EQ(
  //     routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
  //     NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  adjacencyDb3.isOverloaded = true;
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);
  routeMap = getRouteMap(*spfSolver, {"1"});

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj14, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj12, v4Enabled, 20, push4, true)}));
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
class SimpleRingTopologyFixture
    : public ::testing::TestWithParam<
          std::tuple<bool, std::optional<thrift::PrefixType>>> {
 public:
  SimpleRingTopologyFixture() : v4Enabled(std::get<0>(GetParam())) {}

 protected:
  void
  CustomSetUp(
      bool calculateLfas,
      bool useKsp2Ed,
      std::optional<thrift::PrefixType> prefixType = std::nullopt,
      bool createNewBgpRoute = false) {
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
        std::make_pair(true, true),
        spfSolver->updateAdjacencyDatabase(adjacencyDb2));
    EXPECT_EQ(
        std::make_pair(true, true),
        spfSolver->updateAdjacencyDatabase(adjacencyDb3));
    EXPECT_EQ(
        std::make_pair(true, true),
        spfSolver->updateAdjacencyDatabase(adjacencyDb4));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;

    auto bgp1 = v4Enabled ? bgpAddr1V4 : bgpAddr1;
    auto bgp2 = v4Enabled ? bgpAddr2V4 : bgpAddr2;
    auto bgp3 = v4Enabled ? bgpAddr3V4 : bgpAddr3;
    auto bgp4 = v4Enabled ? bgpAddr4V4 : bgpAddr4;

    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb1,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp1)
                                    : std::nullopt)
            : pdb1));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb2,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp2)
                                    : std::nullopt)
            : pdb2));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb3,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp3)
                                    : std::nullopt)
            : pdb3));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb4,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp4)
                                    : std::nullopt)
            : pdb4));
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  std::unique_ptr<SpfSolver> spfSolver;

  void
  verifyRouteInUpdateNoDelete(
      std::string nodeName, int32_t mplsLabel, thrift::RouteDatabase& compDb) {
    // verify route DB change in node 1.
    auto routeDb1 = spfSolver->buildRouteDb(nodeName).value();
    std::sort(compDb.mplsRoutes.begin(), compDb.mplsRoutes.end());
    std::sort(compDb.unicastRoutes.begin(), compDb.unicastRoutes.end());
    std::sort(routeDb1.mplsRoutes.begin(), routeDb1.mplsRoutes.end());
    std::sort(routeDb1.unicastRoutes.begin(), routeDb1.unicastRoutes.end());
    auto deltaRoutes = findDeltaRoutes(routeDb1, compDb);

    int find = 0;
    for (const auto& mplsRoute : deltaRoutes.mplsRoutesToUpdate) {
      if (mplsRoute.topLabel == mplsLabel) {
        find++;
      }
    }
    EXPECT_EQ(find, 1);
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

//
// Verify SpfSolver finds the shortest path
//
TEST_P(SimpleRingTopologyFixture, ShortestPathTest) {
  CustomSetUp(false /* disable LFA */, false /* useKsp2Ed */);
  fb303::fbData->resetAllData();
  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(36, routeMap.size());

  // validate router 1
  const auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.spf_runs.count"), 4);
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
// Verify duplicate mpls routes case
// let two nodes announcing same mpls label. Verify that the one with higher
// name value would win.
// change one node to use a different mpls label. verify routes gets programmed
// and no withdraw happened.
//
TEST_P(SimpleRingTopologyFixture, DuplicateMplsRoutes) {
  CustomSetUp(false /* disable LFA */, false /* useKsp2Ed */);
  fb303::fbData->resetAllData();
  // make node1's mpls label same as node2.
  adjacencyDb1.nodeLabel = 2;
  spfSolver->updateAdjacencyDatabase(adjacencyDb1);

  // verify route DB change in node 1, 2 ,3.
  // verify that only one route to mpls lable 1 is installed in all nodes
  thrift::RouteDatabase emptyRouteDb;
  emptyRouteDb.thisNodeName = "1";
  verifyRouteInUpdateNoDelete("1", 2, emptyRouteDb);

  emptyRouteDb.thisNodeName = "2";
  verifyRouteInUpdateNoDelete("2", 2, emptyRouteDb);

  emptyRouteDb.thisNodeName = "3";
  verifyRouteInUpdateNoDelete("3", 2, emptyRouteDb);

  auto counters = fb303::fbData->getCounters();
  // verify the counters to be 3 because each node will noticed a duplicate
  // for mpls label 1.
  EXPECT_EQ(counters.at("decision.duplicate_node_label.count.60"), 3);

  auto compDb1 = spfSolver->buildRouteDb("1").value();
  auto compDb2 = spfSolver->buildRouteDb("2").value();
  auto compDb3 = spfSolver->buildRouteDb("3").value();

  counters = fb303::fbData->getCounters();
  // now the counter should be 6, becasue we called buildRouteDb 3 times.
  EXPECT_EQ(counters.at("decision.duplicate_node_label.count.60"), 6);

  // change nodelabel of node 1 to be 1. Now each node has it's own
  // mpls label, there should be no duplicate.
  // verify that there is an update entry for mpls route to label 1.
  // verify that no withdrawals of mpls routes to label 1.
  adjacencyDb1.nodeLabel = 1;
  spfSolver->updateAdjacencyDatabase(adjacencyDb1);
  verifyRouteInUpdateNoDelete("1", 2, compDb1);

  verifyRouteInUpdateNoDelete("2", 2, compDb2);

  verifyRouteInUpdateNoDelete("3", 2, compDb3);

  // because there is no duplicate anymore, so that counter should keep as 6.
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.duplicate_node_label.count.60"), 6);
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
  CustomSetUp(
      true /* multipath - ignored */,
      true /* useKsp2Ed */,
      std::get<1>(GetParam()));
  fb303::fbData->resetAllData();
  auto routeMap = getRouteMap(*spfSolver, {"1", "2", "3", "4"});

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(36, routeMap.size());

  const auto counters = fb303::fbData->getCounters();
  // 4 + 4 * 3 peer per node (clean runs are memoized, 2nd runs  with linksTo
  // ignore are not so we redo for each neighbor)
  EXPECT_EQ(counters.at("decision.spf_runs.count"), 16);
  auto pushCode = thrift::MplsActionCode::PUSH;
  auto push1 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1});
  auto push2 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2});
  auto push3 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3});
  auto push4 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4});
  auto push24 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 4});
  auto push34 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3, 4});
  auto push43 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4, 3});
  auto push13 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1, 3});
  auto push42 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4, 2});
  auto push12 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1, 2});
  auto push31 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3, 1});
  auto push21 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 1});

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20, push4, true),
                createNextHopFromAdj(adj13, v4Enabled, 20, push4, true)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj12, v4Enabled, 30, push34, true)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj13, v4Enabled, 30, push24, true)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", adjacencyDb1.nodeLabel);
  validateAdjLabelRoutes(routeMap, "1", adjacencyDb1.adjacencies);

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj21, v4Enabled, 30, push43, true)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20, push3, true),
                createNextHopFromAdj(adj24, v4Enabled, 20, push3, true)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj24, v4Enabled, 30, push13, true)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", adjacencyDb2.nodeLabel);
  validateAdjLabelRoutes(routeMap, "2", adjacencyDb2.adjacencies);

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj31, v4Enabled, 30, push42, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb4.nodeLabel))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20, push2, true),
                createNextHopFromAdj(adj34, v4Enabled, 20, push2, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj34, v4Enabled, 30, push12, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", adjacencyDb3.nodeLabel);
  validateAdjLabelRoutes(routeMap, "3", adjacencyDb3.adjacencies);

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj42, v4Enabled, 30, push31, true)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb3.nodeLabel))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10, std::nullopt, true),
                createNextHopFromAdj(adj43, v4Enabled, 30, push21, true)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb2.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20, push1, true),
                createNextHopFromAdj(adj43, v4Enabled, 20, push1, true)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(adjacencyDb1.nodeLabel))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", adjacencyDb4.nodeLabel);
  validateAdjLabelRoutes(routeMap, "4", adjacencyDb4.adjacencies);

  // this is to test corner cases for traceEdgeDisjointPaths algorithm.
  // In this example, node 3 is overloaded, and link between node 1 and node 2
  // are overloaded. In such case, there is no route from node 1 to node 2 and 4
  adjacencyDb1.adjacencies[0].isOverloaded = true;
  adjacencyDb3.isOverloaded = true;
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);
  routeMap = getRouteMap(*spfSolver, {"1"});

  EXPECT_EQ(
      routeMap.find(make_pair("1", toString(v4Enabled ? addr4V4 : addr4))),
      routeMap.end());

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops(
          {createNextHopFromAdj(adj13, v4Enabled, 10, std::nullopt, true)}));

  EXPECT_EQ(
      routeMap.find(make_pair("1", toString(v4Enabled ? addr2V4 : addr2))),
      routeMap.end());
}

TEST_P(SimpleRingTopologyFixture, Ksp2EdEcmpForBGP) {
  CustomSetUp(
      true /* multipath - ignored */,
      true /* useKsp2Ed */,
      thrift::PrefixType::BGP,
      true);

  fb303::fbData->resetAllData();
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

  auto prefixDBs = spfSolver->getPrefixDatabases();
  auto prefixDBOne = prefixDBs["1"];
  auto prefixDBTwo = prefixDBs["2"];

  // set metric vector for two prefixes in two nodes to be same
  prefixDBOne.prefixEntries[1].mv_ref() = mv1;
  prefixDBTwo.prefixEntries.push_back(prefixDBOne.prefixEntries[1]);
  // only node 1 is announcing the prefix with prependLabel.
  // node 2 is announcing the prefix without prependLabel.
  prefixDBOne.prefixEntries[1].prependLabel_ref() = 60000;

  spfSolver->updatePrefixDatabase(prefixDBOne);
  spfSolver->updatePrefixDatabase(prefixDBTwo);

  auto routeMap = getRouteMap(*spfSolver, {"3"});

  const auto counters = fb303::fbData->getCounters();
  // run 2 spfs for all peers
  EXPECT_EQ(counters.at("decision.spf_runs.count"), 6);
  auto pushCode = thrift::MplsActionCode::PUSH;
  auto push2 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2});
  auto push12 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1, 2});

  auto push2AndPrependLabel =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{60000, 2});
  auto push12AndPrependLabel = createMplsAction(
      pushCode, std::nullopt, std::vector<int32_t>{60000, 1, 2});

  auto pushPrependLabel =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{60000});

  // in router 3, we are expecting addr1 to be null because it is a bgp route
  // with same metric vector from two nodes
  // in current setup, if we don't setup tie break to be true, in case of tie,
  // we counld not determine which node to be best dest node, hence we will not
  // program routes to it.
  EXPECT_EQ(
      routeMap.find(
          make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))),
      routeMap.end());

  // decrease mv for the second node, now router 3 should point to 1
  prefixDBTwo.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .metric.front()--;

  // change data to some special case for verification
  prefixDBOne.prefixEntries.back().data = "123";
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  spfSolver->updatePrefixDatabase(prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"});
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 10, pushPrependLabel, true),
           createNextHopFromAdj(
               adj34, v4Enabled, 30, push12AndPrependLabel, true)}));

  auto route = getUnicastRoutes(
      *spfSolver,
      {"3"})[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))];

  EXPECT_EQ(
      route.bestNexthop_ref().value(),
      createNextHop(
          v4Enabled ? addr1V4.prefixAddress : addr1.prefixAddress,
          std::nullopt,
          0,
          std::nullopt,
          false));

  // increase mv for the second node by 2, now router 3 should point to 2
  prefixDBTwo.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .metric.front() += 2;
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  routeMap = getRouteMap(*spfSolver, {"3"});
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20, push2, true),
                createNextHopFromAdj(adj34, v4Enabled, 20, push2, true)}));

  route = getUnicastRoutes(
      *spfSolver,
      {"3"})[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))];

  EXPECT_EQ(
      route.bestNexthop_ref(),
      createNextHop(
          v4Enabled ? addr2V4.prefixAddress : addr2.prefixAddress,
          std::nullopt,
          0,
          std::nullopt,
          false));

  // set the tie breaker to be true. in this case, both nodes will be selected
  prefixDBTwo.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;

  prefixDBOne.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  spfSolver->updatePrefixDatabase(prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"});

  // createNextHopFromAdj(adj34, v4Enabled, 30, push12, true) getting ignored
  // because in kspf, we will ignore second shortest path if it starts with
  // one of shortest path for anycast prefix
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20, push2, true),
                createNextHopFromAdj(adj34, v4Enabled, 20, push2, true),
                createNextHopFromAdj(
                    adj31, v4Enabled, 10, pushPrependLabel, true)}));

  route = getUnicastRoutes(
      *spfSolver,
      {"3"})[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))];
  const auto bestNextHop1 = createNextHop(
      v4Enabled ? addr1V4.prefixAddress : addr1.prefixAddress,
      std::nullopt,
      0,
      std::nullopt,
      false);

  const auto bestNextHop2 = createNextHop(
      v4Enabled ? addr2V4.prefixAddress : addr2.prefixAddress,
      std::nullopt,
      0,
      std::nullopt,
      false);
  EXPECT_THAT(
      route.bestNexthop_ref().value(), AnyOf(bestNextHop1, bestNextHop2));

  if (route.bestNexthop_ref() == bestNextHop1) {
    EXPECT_EQ(route.data_ref(), "123");
  } else {
    EXPECT_EQ(route.data_ref(), "");
  }
  EXPECT_EQ(route.prefixType_ref(), thrift::PrefixType::BGP);

  // verify on node 1. From node 1 point of view, both node 1 and node 2 are
  // are annoucing the prefix. So it will program 3 nexthops.
  // 1: recursively resolved MPLS nexthops of prependLabel
  // 2: shortest path to node 2.
  // 3: second shortest path node 2.
  thrift::StaticRoutes staticRoutes;
  int32_t staticMplsRouteLabel = 60000;
  // insert the new nexthop to mpls static routes cache
  thrift::NextHopThrift nh;
  nh.address = toBinaryAddress("1.1.1.1");
  nh.mplsAction_ref() = createMplsAction(thrift::MplsActionCode::PHP);
  thrift::MplsRoute staticMplsRoute;
  staticMplsRoute.topLabel = staticMplsRouteLabel;
  staticMplsRoute.nextHops.emplace_back(nh);
  thrift::RouteDatabaseDelta routesDelta;
  routesDelta.mplsRoutesToUpdate = {staticMplsRoute};
  spfSolver->pushRoutesDeltaUpdates(routesDelta);
  spfSolver->processStaticRouteUpdates();

  routeMap = getRouteMap(*spfSolver, {"1"});
  // NOTE: 60000 is the static MPLS route on node 2 which prevent routing loop.
  auto push24 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 4});
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops(
          {createNextHop(
               toBinaryAddress("1.1.1.1"),
               std::nullopt,
               0,
               std::nullopt,
               true /* useNonShortestRoute */),
           createNextHopFromAdj(adj13, v4Enabled, 30, push24, true),
           createNextHopFromAdj(adj12, v4Enabled, 10, std::nullopt, true)}));
}

TEST_P(SimpleRingTopologyFixture, Ksp2EdEcmpForBGP123) {
  CustomSetUp(
      true /* multipath - ignored */,
      true /* useKsp2Ed */,
      thrift::PrefixType::BGP,
      true);

  fb303::fbData->resetAllData();
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

  auto prefixDBs = spfSolver->getPrefixDatabases();
  auto prefixDBOne = prefixDBs["1"];
  auto prefixDBTwo = prefixDBs["2"];

  // set metric vector for two prefixes in two nodes to be same
  prefixDBOne.prefixEntries[1].mv_ref() = mv1;
  prefixDBTwo.prefixEntries.push_back(prefixDBOne.prefixEntries[1]);
  prefixDBOne.prefixEntries[1].prependLabel_ref() = 60000;

  // increase mv for the second node by 2, now router 3 should point to 2
  prefixDBTwo.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .metric.front() += 1;

  // set the tie breaker to be true. in this case, both nodes will be selected
  prefixDBTwo.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;

  prefixDBOne.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;

  spfSolver->updatePrefixDatabase(prefixDBOne);
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  auto pushCode = thrift::MplsActionCode::PUSH;

  // verify on node 1. From node 1 point of view, both node 1 and node 2 are
  // are annoucing the prefix. So it will program 3 nexthops.
  // 1: recursively resolved MPLS nexthops of prependLabel
  // 2: shortest path to node 2.
  // 3: second shortest path node 2.
  thrift::StaticRoutes staticRoutes;
  int32_t staticMplsRouteLabel = 60000;
  // insert the new nexthop to mpls static routes cache
  thrift::NextHopThrift nh;
  nh.address = toBinaryAddress("1.1.1.1");
  nh.mplsAction_ref() = createMplsAction(thrift::MplsActionCode::PHP);
  thrift::MplsRoute staticMplsRoute;
  staticMplsRoute.topLabel = staticMplsRouteLabel;
  staticMplsRoute.nextHops.emplace_back(nh);
  thrift::RouteDatabaseDelta routesDelta;
  routesDelta.mplsRoutesToUpdate = {staticMplsRoute};
  spfSolver->pushRoutesDeltaUpdates(routesDelta);
  spfSolver->processStaticRouteUpdates();

  auto routeMap = getRouteMap(*spfSolver, {"1"});
  // NOTE: 60000 is the static MPLS route on node 2 which prevent routing loop.
  auto push24 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 4});
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops(
          {createNextHop(
               toBinaryAddress("1.1.1.1"),
               std::nullopt,
               0,
               std::nullopt,
               true /* useNonShortestRoute */),
           createNextHopFromAdj(adj13, v4Enabled, 30, push24, true),
           createNextHopFromAdj(adj12, v4Enabled, 10, std::nullopt, true)}));

  prefixDBOne.prefixEntries[1].minNexthop_ref() = 3;
  spfSolver->updatePrefixDatabase(prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"1"});

  EXPECT_EQ(
      routeMap.find(
          make_pair("1", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))),
      routeMap.end());
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
class ParallelAdjRingTopologyFixture
    : public ::testing::TestWithParam<std::optional<thrift::PrefixType>> {
 public:
  ParallelAdjRingTopologyFixture() {}

 protected:
  void
  CustomSetUp(
      bool calculateLfas,
      bool useKsp2Ed,
      std::optional<thrift::PrefixType> prefixType = std::nullopt) {
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
        useKsp2Ed ? createPrefixDbWithKspfAlgo(prefixDb1, prefixType)
                  : prefixDb1));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? createPrefixDbWithKspfAlgo(prefixDb2, prefixType)
                  : prefixDb2));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? createPrefixDbWithKspfAlgo(prefixDb3, prefixType)
                  : prefixDb3));
    EXPECT_TRUE(spfSolver->updatePrefixDatabase(
        useKsp2Ed ? createPrefixDbWithKspfAlgo(prefixDb4, prefixType)
                  : prefixDb4));
  }

  thrift::Adjacency adj12_1, adj12_2, adj12_3, adj13_1, adj21_1, adj21_2,
      adj21_3, adj24_1, adj31_1, adj34_1, adj34_2, adj34_3, adj42_1, adj43_1,
      adj43_2, adj43_3;
  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  std::unique_ptr<SpfSolver> spfSolver;
};

INSTANTIATE_TEST_CASE_P(
    ParallelAdjRingTopologyInstance,
    ParallelAdjRingTopologyFixture,
    ::testing::Values(std::nullopt, thrift::PrefixType::BGP));

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
TEST_P(ParallelAdjRingTopologyFixture, Ksp2EdEcmp) {
  std::optional<thrift::PrefixType> prefixType = GetParam();
  CustomSetUp(true /* multipath, ignored */, true /* useKsp2Ed */, prefixType);

  auto pushCode = thrift::MplsActionCode::PUSH;
  auto push1 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1});
  auto push2 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2});
  auto push3 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3});
  auto push4 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4});
  auto push34 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3, 4});
  auto push43 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4, 3});
  auto push12 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1, 2});
  auto push21 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 1});

  // Verify parallel link case between node-1 and node-2
  auto routeMap = getRouteMap(*spfSolver, {"1"});

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj12_2, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj12_3, false, 20, std::nullopt, true)}));

  auto prefixDBs = spfSolver->getPrefixDatabases();
  auto prefixDBFour = prefixDBs["4"];

  // start to test minNexthop feature by injecting an ondeman prefix with
  // threshold to be 4 at the beginning.
  auto newPrefix = createPrefixEntry(
      bgpAddr1,
      thrift::PrefixType::LOOPBACK,
      "",
      thrift::PrefixForwardingType::SR_MPLS,
      thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP,
      std::nullopt,
      std::nullopt,
      std::make_optional<int64_t>(4));

  prefixDBFour.prefixEntries.push_back(newPrefix);
  spfSolver->updatePrefixDatabase(prefixDBFour);
  routeMap = getRouteMap(*spfSolver, {"1"});

  // in theory, kspf will choose adj12_2, adj13_1 as nexthops,
  // but since we set threhold to be 4, this route will get ignored.
  EXPECT_EQ(routeMap.find(make_pair("1", toString(bgpAddr1))), routeMap.end());

  // updating threshold hold to be 2.
  // becasue we use edge disjoint algorithm, we should expect adj12_2 and
  // adj13_1 as nexthop
  prefixDBFour.prefixEntries.pop_back();
  apache::thrift::fromFollyOptional(
      newPrefix.minNexthop_ref(), folly::make_optional<int64_t>(2));
  prefixDBFour.prefixEntries.push_back(newPrefix);
  spfSolver->updatePrefixDatabase(prefixDBFour);
  routeMap = getRouteMap(*spfSolver, {"1"});
  EXPECT_EQ(
      routeMap[make_pair("1", toString(bgpAddr1))],
      NextHops({createNextHopFromAdj(adj12_2, false, 22, push4, true),
                createNextHopFromAdj(adj13_1, false, 22, push4, true)}));

  for (const auto& nexthop : routeMap[make_pair("1", toString(bgpAddr1))]) {
    LOG(INFO) << (toString(nexthop));
  }

  // Let node 3 announcing same prefix with limit 4. In this case,
  // threshold should be 4 instead of 2. And the ip is an anycast from node
  // 3 and 4. so nexthops should be adj12_2, adj13_1(shortes to node 4)
  // and adj13_1 shortest to node 3. The second shortes are all eliminated
  // becasue of purging logic we have for any cast ip.
  auto prefixDBThr = prefixDBs["3"];
  apache::thrift::fromFollyOptional(
      newPrefix.minNexthop_ref(), folly::make_optional<int64_t>(4));
  prefixDBThr.prefixEntries.push_back(newPrefix);
  spfSolver->updatePrefixDatabase(prefixDBThr);
  routeMap = getRouteMap(*spfSolver, {"1"});

  EXPECT_EQ(routeMap.find(make_pair("1", toString(bgpAddr1))), routeMap.end());

  // Revert the setup to normal state
  prefixDBFour.prefixEntries.pop_back();
  prefixDBThr.prefixEntries.pop_back();
  spfSolver->updatePrefixDatabase(prefixDBFour);
  spfSolver->updatePrefixDatabase(prefixDBThr);

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
      NextHops({createNextHopFromAdj(adj12_1, false, 22, push4, true),
                createNextHopFromAdj(adj13_1, false, 22, push4, true)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj12_1, false, 33, push34, true)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj12_3, false, 20, std::nullopt, true)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj21_1, false, 33, push43, true)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21_1, false, 22, push3, true),
                createNextHopFromAdj(adj24_1, false, 22, push3, true)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj21_3, false, 20, std::nullopt, true)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj34_3, false, 20, std::nullopt, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, push2, true),
                createNextHopFromAdj(adj34_1, false, 22, push2, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj34_1, false, 33, push12, true)}));

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj43_3, false, 20, std::nullopt, true)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj43_1, false, 33, push21, true)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22, push1, true),
                createNextHopFromAdj(adj43_1, false, 22, push1, true)}));
}

TEST_P(ParallelAdjRingTopologyFixture, Ksp2EdEcmpForBGP) {
  CustomSetUp(
      true /* multipath, ignored */,
      true /* useKsp2Ed */,
      thrift::PrefixType::BGP);

  auto pushCode = thrift::MplsActionCode::PUSH;
  auto push1 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1});
  auto push2 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2});
  auto push3 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3});
  auto push4 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4});
  auto push34 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{3, 4});
  auto push43 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{4, 3});
  auto push12 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{1, 2});
  auto push21 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 1});

  //
  // Verify parallel link case between node-1 and node-2
  auto routeMap = getRouteMap(*spfSolver, {"1"});

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj12_2, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj12_3, false, 20, std::nullopt, true)}));

  //
  // Bring down adj12_2 and adj34_2 to make our nexthop validations easy
  // Then validate routing table of all the nodes
  //

  adjacencyDb1.adjacencies.at(1).isOverloaded = true;
  adjacencyDb3.adjacencies.at(2).isOverloaded = true;
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb1).first);
  EXPECT_TRUE(spfSolver->updateAdjacencyDatabase(adjacencyDb3).first);

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

  auto prefixDBs = spfSolver->getPrefixDatabases();
  auto prefixDBOne = prefixDBs["1"];
  auto prefixDBTwo = prefixDBs["2"];

  // set metric vector for addr1 prefixes in two nodes to be same
  prefixDBOne.prefixEntries[0].mv_ref() = mv1;
  prefixDBTwo.prefixEntries.push_back(prefixDBOne.prefixEntries[0]);

  spfSolver->updatePrefixDatabase(prefixDBOne);
  spfSolver->updatePrefixDatabase(prefixDBTwo);

  routeMap = getRouteMap(*spfSolver, {"3"});

  // validate router 3
  EXPECT_EQ(routeMap.find(make_pair("3", toString(addr1))), routeMap.end());

  // decrease mv for the second node, now router 3 should point to 1
  // also set the threshold hold on non-best node to be 4. Threshold
  // on best node is 2. In such case, we should allow the route to be
  // programmed and announced.
  prefixDBTwo.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .metric.front()--;
  prefixDBTwo.prefixEntries.back().minNexthop_ref() = 4;
  prefixDBOne.prefixEntries.back().minNexthop_ref() = 2;
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  spfSolver->updatePrefixDatabase(prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"});

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, std::nullopt, true),
                createNextHopFromAdj(adj34_1, false, 33, push12, true)}));

  // node 1 is the preferred node. Set threshold on Node 1 to be 4.
  // threshold on  node 2 is 2. In such case, threshold should be respected
  // and we should not program/annouce any routes
  prefixDBTwo.prefixEntries.back().minNexthop_ref() = 2;
  prefixDBOne.prefixEntries.back().minNexthop_ref() = 4;
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  spfSolver->updatePrefixDatabase(prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"});

  // validate router 3
  EXPECT_EQ(routeMap.find(make_pair("3", toString(addr1))), routeMap.end());
  // reset min nexthop to rest of checks
  prefixDBTwo.prefixEntries.back().minNexthop_ref().reset();
  prefixDBOne.prefixEntries.back().minNexthop_ref().reset();
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  spfSolver->updatePrefixDatabase(prefixDBOne);

  // decrease mv for the second node, now router 3 should point to 2
  prefixDBTwo.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .metric.front() += 2;
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  routeMap = getRouteMap(*spfSolver, {"3"});

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, push2, true),
                createNextHopFromAdj(adj34_1, false, 22, push2, true)}));

  // set the tie breaker to be true. in this case, both nodes will be selected
  prefixDBTwo.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;

  prefixDBOne.prefixEntries.back()
      .mv_ref()
      .value()
      .metrics[numMetrics - 1]
      .isBestPathTieBreaker = true;
  spfSolver->updatePrefixDatabase(prefixDBTwo);
  spfSolver->updatePrefixDatabase(prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"});

  // createNextHopFromAdj(adj34, v4Enabled, 30, push12, true) getting ignored
  // because in kspf, we will ignore second shortest path if it starts with
  // one of shortest path for anycast prefix
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, push2, true),
                createNextHopFromAdj(adj34_1, false, 22, push2, true),
                createNextHopFromAdj(adj31_1, false, 11, std::nullopt, true)}));
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
      thrift::MplsActionCode::PUSH, std::nullopt, std::vector<int32_t>{1});
  auto const labelPush2 = createMplsAction(
      thrift::MplsActionCode::PUSH, std::nullopt, std::vector<int32_t>{2});
  auto const labelPush3 = createMplsAction(
      thrift::MplsActionCode::PUSH, std::nullopt, std::vector<int32_t>{3});
  auto const labelPush4 = createMplsAction(
      thrift::MplsActionCode::PUSH, std::nullopt, std::vector<int32_t>{4});
  auto const labelPush5 = createMplsAction(
      thrift::MplsActionCode::PUSH, std::nullopt, std::vector<int32_t>{5});

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
  adjs.emplace_back(createThriftAdjacency(
      folly::sformat("{}", neighbor),
      ifName,
      folly::sformat("fe80::{}", neighbor),
      folly::sformat("192.168.{}.{}", neighbor / 256, neighbor % 256),
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
  spfSolver.buildRouteDb("523");
}

//
// Start the decision thread and simulate KvStore communications
// Expect proper RouteDatabase publications to appear
//
class DecisionTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    auto tConfig = getBasicOpenrConfig("1");
    config = std::make_shared<Config>(tConfig);

    decision = make_shared<Decision>(
        config,
        true, /* computeLfaPaths */
        false, /* bgpDryRun */
        debounceTimeoutMin,
        debounceTimeoutMax,
        kvStoreUpdatesQueue.getReader(),
        staticRoutesUpdateQueue.getReader(),
        routeUpdatesQueue,
        zeromqContext);

    decisionThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Decision thread starting";
      decision->run();
      LOG(INFO) << "Decision thread finishing";
    });
    decision->waitUntilRunning();
  }

  void
  TearDown() override {
    kvStoreUpdatesQueue.close();
    staticRoutesUpdateQueue.close();

    LOG(INFO) << "Stopping the decision thread";
    decision->stop();
    decisionThread->join();
    LOG(INFO) << "Decision thread got stopped";
  }

  //
  // member methods
  //

  std::unordered_map<std::string, thrift::RouteDatabase>
  dumpRouteDb(const vector<string>& allNodes) {
    std::unordered_map<std::string, thrift::RouteDatabase> routeMap;

    for (string const& node : allNodes) {
      auto resp = decision->getDecisionRouteDb(node).get();
      EXPECT_TRUE(resp);
      EXPECT_EQ(node, resp->thisNodeName);
      routeMap[node] = std::move(*resp);
    }

    return routeMap;
  }

  thrift::RouteDatabaseDelta
  recvMyRouteDb(
      const string& /* myNodeName */,
      // TODO: Remove unused argument serializer
      const apache::thrift::CompactSerializer& /* serializer */) {
    auto maybeRouteDb = routeUpdatesQueueReader.get();
    EXPECT_FALSE(maybeRouteDb.hasError());
    auto routeDbDelta = maybeRouteDb.value();
    return routeDbDelta;
  }

  // publish routeDb
  void
  sendKvPublication(const thrift::Publication& publication) {
    kvStoreUpdatesQueue.push(publication);
  }

  void
  sendStaticRoutesUpdate(const thrift::RouteDatabaseDelta& publication) {
    staticRoutesUpdateQueue.push(publication);
  }

  // helper function
  thrift::Value
  createAdjValue(
      const string& node,
      int64_t version,
      const vector<thrift::Adjacency>& adjs,
      bool overloaded = false,
      int32_t nodeId = 0) {
    auto adjDB = createAdjDb(node, adjs, nodeId);
    adjDB.isOverloaded = overloaded;
    return thrift::Value(
        FRAGILE,
        version,
        "originator-1",
        fbzmq::util::writeThriftObjStr(adjDB, serializer),
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
    return createThriftValue(
        version,
        node,
        fbzmq::util::writeThriftObjStr(
            createPrefixDb(node, prefixEntries), serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  std::unordered_map<std::string, thrift::Value>
  createPerPrefixKeyValue(
      const string& node,
      int64_t version,
      const vector<thrift::IpPrefix>& prefixes) {
    std::unordered_map<std::string, thrift::Value> keyVal{};
    for (const auto& prefix : prefixes) {
      const auto prefixKey = PrefixKey(
          node,
          folly::IPAddress::createNetwork(toString(prefix)),
          thrift::KvStore_constants::kDefaultArea());
      keyVal[prefixKey.getPrefixKey()] = createThriftValue(
          version,
          node,
          fbzmq::util::writeThriftObjStr(
              createPrefixDb(node, {createPrefixEntry(prefix)}), serializer),
          Constants::kTtlInfinity /* ttl */,
          0 /* ttl version */,
          0 /* hash */);
    }
    return keyVal;
  }

  /**
   * Check whether two RouteDatabaseDeltas to be equal
   */
  bool
  checkEqualRoutesDelta(
      thrift::RouteDatabaseDelta& lhs, thrift::RouteDatabaseDelta& rhs) {
    std::sort(
        lhs.unicastRoutesToUpdate.begin(), lhs.unicastRoutesToUpdate.end());
    std::sort(
        rhs.unicastRoutesToUpdate.begin(), rhs.unicastRoutesToUpdate.end());

    std::sort(
        lhs.unicastRoutesToDelete.begin(), lhs.unicastRoutesToDelete.end());
    std::sort(
        rhs.unicastRoutesToDelete.begin(), rhs.unicastRoutesToDelete.end());

    return lhs.unicastRoutesToUpdate == rhs.unicastRoutesToUpdate &&
        lhs.unicastRoutesToDelete == rhs.unicastRoutesToDelete;
  }

  //
  // member variables
  //

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  CompactSerializer serializer{};

  // ZMQ context for IO processing
  fbzmq::Context zeromqContext{};

  std::shared_ptr<Config> config;
  messaging::ReplicateQueue<thrift::Publication> kvStoreUpdatesQueue;
  messaging::ReplicateQueue<thrift::RouteDatabaseDelta> staticRoutesUpdateQueue;
  messaging::ReplicateQueue<thrift::RouteDatabaseDelta> routeUpdatesQueue;
  messaging::RQueue<thrift::RouteDatabaseDelta> routeUpdatesQueueReader{
      routeUpdatesQueue.getReader()};

  // Decision owned by this wrapper.
  std::shared_ptr<Decision> decision{nullptr};

  // Thread in which decision will be running.
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

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue("2", 1, {adj21}, false, 2)},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      std::string(""));
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  auto routeDbDelta = recvMyRouteDb("1", serializer);
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  // self mpls route, node 2 mpls route and adj12 label route
  EXPECT_EQ(3, routeDbDelta.mplsRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());

  auto routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes.begin(), routeDb.unicastRoutes.end());
  std::sort(routeDb.mplsRoutes.begin(), routeDb.mplsRoutes.end());

  auto routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));

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

  publication = createThriftPublication(
      {{"adj:3", createAdjValue("3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue("2", 3, {adj21, adj23}, false, 2)},
       {"adj:4", createAdjValue("4", 1, {}, false, 4)}, // No adjacencies
       {"prefix:3", createPrefixValue("3", 1, {addr3})}},
      {},
      {},
      {},
      std::string(""));
  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes.begin(), routeDbBefore.unicastRoutes.end());
  std::sort(routeDbBefore.mplsRoutes.begin(), routeDbBefore.mplsRoutes.end());
  sendKvPublication(publication);
  // validate routers

  // receive my local Decision routeDbDelta publication
  routeDbDelta = recvMyRouteDb("1" /* node name */, serializer);
  // only expect to add a route to addr3
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(routeDbDelta.unicastRoutesToUpdate[0].dest, addr3);
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());

  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes.begin(), routeDb.unicastRoutes.end());
  std::sort(routeDb.mplsRoutes.begin(), routeDb.mplsRoutes.end());
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  fillRouteMap("1", routeMap, routeDb);
  // 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj12, false, 20)}));

  // dump other nodes' routeDB
  auto routeDbMap = dumpRouteDb({"2", "3"});
  EXPECT_EQ(2, routeDbMap["2"].unicastRoutes.size());
  EXPECT_EQ(2, routeDbMap["3"].unicastRoutes.size());
  for (auto& [key, value] : routeDbMap) {
    fillRouteMap(key, routeMap, value);
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
  publication = createThriftPublication(
      thrift::KeyVals{},
      {"adj:3", "prefix:3", "adj:4"} /* expired keys */,
      {},
      {},
      std::string(""));

  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes.begin(), routeDbBefore.unicastRoutes.end());
  std::sort(routeDbBefore.mplsRoutes.begin(), routeDbBefore.mplsRoutes.end());

  sendKvPublication(publication);
  routeDbDelta = recvMyRouteDb("1" /* node name */, serializer);
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToDelete.size());
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToDelete.size());
  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes.begin(), routeDb.unicastRoutes.end());
  std::sort(routeDb.mplsRoutes.begin(), routeDb.mplsRoutes.end());

  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  publication = createThriftPublication(
      {{"adj:3", createAdjValue("3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue("2", 4, {adj21, adj23}, false, 2)},
       {"prefix:3", createPrefixValue("3", 1, {addr3})}},
      {},
      {},
      {},
      std::string(""));
  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes.begin(), routeDbBefore.unicastRoutes.end());
  std::sort(routeDbBefore.mplsRoutes.begin(), routeDbBefore.mplsRoutes.end());
  sendKvPublication(publication);
  // validate routers

  // receive my local Decision routeDbDelta publication
  routeDbDelta = recvMyRouteDb("1" /* node name */, serializer);
  // only expect to add a route to addr3
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(routeDbDelta.unicastRoutesToUpdate[0].dest, addr3);
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.size());

  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes.begin(), routeDb.unicastRoutes.end());
  std::sort(routeDb.mplsRoutes.begin(), routeDb.mplsRoutes.end());
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));

  // construct new static mpls route add
  thrift::RouteDatabaseDelta input;
  input.thisNodeName = "1";
  thrift::NextHopThrift nh, nh1, nh2;
  nh.address = toBinaryAddress(folly::IPAddressV6("::1"));
  nh1.address = toBinaryAddress(folly::IPAddressV6("::2"));
  nh.mplsAction_ref() =
      createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
  nh1.mplsAction_ref() =
      createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
  thrift::MplsRoute route;
  route.topLabel = 32011;
  route.nextHops = {nh};
  input.mplsRoutesToUpdate.push_back(route);

  // update 32011 and make sure only that is updated
  sendStaticRoutesUpdate(input);
  auto routesDelta = routeUpdatesQueueReader.get().value();
  // consume an empty routes update because of reachability change.
  routeUpdatesQueueReader.get().value();
  routesDelta.perfEvents_ref().reset();
  EXPECT_EQ(routesDelta, input);

  // update another routes and make sure only that is updated
  route.topLabel = 32012;
  input.mplsRoutesToUpdate = {route};
  sendStaticRoutesUpdate(input);
  routesDelta = routeUpdatesQueueReader.get().value();
  // consume an empty routes update because of reachability change.
  routeUpdatesQueueReader.get().value();
  routesDelta.perfEvents_ref().reset();
  EXPECT_EQ(routesDelta, input);

  auto staticRoutes =
      decision->getDecisionStaticRoutes().wait().value()->mplsRoutes;
  EXPECT_EQ(staticRoutes.size(), 2);
  EXPECT_THAT(staticRoutes[32012], testing::UnorderedElementsAreArray({nh}));
  EXPECT_THAT(staticRoutes[32011], testing::UnorderedElementsAreArray({nh}));

  // test our consolidating logic, we first update 32011 then delete 32011
  // making sure only delete for 32011 is emitted.
  route.topLabel = 32011;
  route.nextHops = {nh, nh1};
  input.mplsRoutesToUpdate = {route};
  input.mplsRoutesToDelete.clear();
  sendStaticRoutesUpdate(input);

  input.mplsRoutesToUpdate.clear();
  input.mplsRoutesToDelete = {32011};
  sendStaticRoutesUpdate(input);

  routesDelta = routeUpdatesQueueReader.get().value();
  // consume an empty routes update because of reachability change.
  routeUpdatesQueueReader.get().value();
  routesDelta.perfEvents_ref().reset();
  EXPECT_EQ(routesDelta.mplsRoutesToDelete[0], 32011);
  EXPECT_EQ(routesDelta.mplsRoutesToUpdate.size(), 0);

  staticRoutes = decision->getDecisionStaticRoutes().wait().value()->mplsRoutes;
  EXPECT_EQ(staticRoutes.size(), 1);
  EXPECT_THAT(staticRoutes[32012], testing::UnorderedElementsAreArray({nh}));

  // test our consolidating logic, we first delete 32012 then update 32012
  // making sure only update for 32012 is emitted.
  input.mplsRoutesToUpdate.clear();
  input.mplsRoutesToDelete = {32012};
  sendStaticRoutesUpdate(input);

  route.topLabel = 32012;
  route.nextHops = {nh, nh1};
  input.mplsRoutesToUpdate = {route};
  input.mplsRoutesToDelete.clear();
  sendStaticRoutesUpdate(input);

  routesDelta = routeUpdatesQueueReader.get().value();
  // consume an empty routes update because of reachability change.
  routeUpdatesQueueReader.get().value();
  routesDelta.perfEvents_ref().reset();
  EXPECT_EQ(routesDelta, input);

  staticRoutes = decision->getDecisionStaticRoutes().wait().value()->mplsRoutes;
  EXPECT_EQ(staticRoutes.size(), 1);
  EXPECT_THAT(
      staticRoutes[32012], testing::UnorderedElementsAreArray({nh, nh1}));

  EXPECT_THAT(
      dumpRouteDb({"1"})["1"].mplsRoutes,
      Contains(createMplsRoute(32012, {nh, nh1})));
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

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12_1, adj12_2})},
       {"adj:2", createAdjValue("2", 1, {adj21_1, adj21_2})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      std::string(""));
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  auto routeDbDelta = recvMyRouteDb("1", serializer);
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  auto routeDb = dumpRouteDb({"1"})["1"];
  auto routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 100),
                createNextHopFromAdj(adj12_2, false, 800)}));

  publication = createThriftPublication(
      {{"adj:2", createAdjValue("2", 2, {adj21_2})}},
      {},
      {},
      {},
      std::string(""));

  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvMyRouteDb("1" /* node name */, serializer);
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_2, false, 800)}));

  // restore the original state
  publication = createThriftPublication(
      {{"adj:2", createAdjValue("2", 2, {adj21_1, adj21_2})}},
      {},
      {},
      {},
      std::string(""));
  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvMyRouteDb("1" /* node name */, serializer);
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 100),
                createNextHopFromAdj(adj12_2, false, 800)}));

  // overload the least cost link
  auto adj21_1_overloaded = adj21_1;
  adj21_1_overloaded.isOverloaded = true;

  publication = createThriftPublication(
      {{"adj:2", createAdjValue("2", 2, {adj21_1_overloaded, adj21_2})}},
      {},
      {},
      {},
      std::string(""));
  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvMyRouteDb("1" /* node name */, serializer);
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
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

  fb303::fbData->resetAllData();
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      std::string(""));

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);
  EXPECT_EQ(0, counters["decision.route_build_runs.count"]);

  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  // validate SPF after initial sync, no rebouncing here
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters["decision.spf_runs.count"]);
  EXPECT_EQ(1, counters["decision.route_build_runs.count"]);

  //
  // publish the link state info to KvStore via the KvStore pub socket
  // we simulate adding a new router R3
  //

  // Some tricks here; we need to bump the time-stamp on router 2's data, so
  // it can override existing; for router 3 we publish new key-value
  publication = createThriftPublication(
      {{"adj:3", createAdjValue("3", 1, {adj32})},
       {"adj:2", createAdjValue("2", 3, {adj21, adj23})},
       {"prefix:3", createPrefixValue("3", 1, {addr3})}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);

  // we simulate adding a new router R4

  // Some tricks here; we need to bump the time-stamp on router 3's data, so
  // it can override existing;

  publication = createThriftPublication(
      {{"adj:4", createAdjValue("4", 1, {adj43})},
       {"adj:3", createAdjValue("3", 5, {adj32, adj34})}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(4, counters["decision.spf_runs.count"]);
  EXPECT_EQ(2, counters["decision.route_build_runs.count"]);

  //
  // Only publish prefix updates
  //
  publication = createThriftPublication(
      {{"prefix:4", createPrefixValue("4", 1, {addr4})}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(4, counters["decision.spf_runs.count"]);
  EXPECT_EQ(3, counters["decision.route_build_runs.count"]);

  //
  // publish adj updates right after prefix updates
  // Decision is supposed to only trigger spf recalculation

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = createThriftPublication(
      {{"prefix:4", createPrefixValue("4", 2, {addr4, addr5})}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);

  publication = createThriftPublication(
      {{"adj:2", createAdjValue("2", 5, {adj21})}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(6, counters["decision.spf_runs.count"]);
  EXPECT_EQ(4, counters["decision.route_build_runs.count"]);

  //
  // publish multiple prefix updates in a row
  // Decision is supposed to process prefix update only once

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = createThriftPublication(
      {{"prefix:4", createPrefixValue("4", 5, {addr4})}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);

  publication = createThriftPublication(
      {{"prefix:4", createPrefixValue("4", 7, {addr4, addr6})}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);

  publication = createThriftPublication(
      {{"prefix:4", createPrefixValue("4", 8, {addr4, addr5, addr6})}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(6, counters["decision.spf_runs.count"]);
  // only 1 request shall be processed
  EXPECT_EQ(5, counters["decision.route_build_runs.count"]);
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
  fb303::fbData->resetAllData();
  auto publication = createThriftPublication(
      {{"adj2:1", createAdjValue("1", 1, {adj12})},
       {"adji2:2", createAdjValue("2", 1, {adj21})},
       {"prefix2:1", createPrefixValue("1", 1, {addr1})},
       {"prefix2:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      std::string(""));

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeoutMax);

  // make sure the counter did not increment
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);
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
  fb303::fbData->resetAllData();
  auto const publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {},
      std::string(""));

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeoutMax);

  // make sure counter is incremented
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters["decision.spf_runs.count"]);

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(2 * debounceTimeoutMax);

  // make sure counter is not incremented
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters["decision.spf_runs.count"]);
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

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12, adj13})},
       {"adj:2", createAdjValue("2", 1, {adj21, adj23})},
       {"adj:3", createAdjValue("3", 1, {adj31, adj32})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})},
       {"prefix:3", createPrefixValue("3", 1, {addr3})}},
      {},
      {},
      {},
      std::string(""));

  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  // validate routers
  auto routeMapList = dumpRouteDb({"1", "2", "3"});
  RouteMap routeMap;
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
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
  publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 2, {adj12, adj13})},
       {"adj:2", createAdjValue("2", 2, {adj21, adj23})}},
      {},
      {},
      {},
      std::string(""));

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  // Query new information
  // validate routers
  routeMapList = dumpRouteDb({"1", "2", "3"});
  routeMap.clear();
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
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
  auto publication = createThriftPublication(
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
      std::string(""));

  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  // Query new information
  // validate routers
  auto routeMapList = dumpRouteDb({"1", "2", "3", "4"});
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  RouteMap routeMap;
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
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
   * Overload node-2 and node-4. Now we on node-1 will only route p2 toward
   * node-3 but will still have route p4 toward node-4 since it's unicast
   *
   *  node4(p4)
   *     |
   *   5 |
   *     |         10     (overloaded)
   *  node1(p1) --------- node2(p2)
   *     |
   *     | 10
   *     |
   *  node3(p2)
   */

  publication = createThriftPublication(
      {{"adj:2", createAdjValue("2", 1, {adj21}, true /* overloaded */)},
       {"adj:4", createAdjValue("4", 1, {adj41}, true /* overloaded */)}},
      {},
      {},
      {},
      std::string(""));

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  routeMapList = dumpRouteDb({"1"});
  RouteMap routeMap2;
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap2, value);
  }
  EXPECT_EQ(
      routeMap2[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));

  EXPECT_EQ(
      routeMap2[make_pair("1", toString(addr4))],
      NextHops({createNextHopFromAdj(adj14, false, 5)}));

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

  publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 2, {adj12, adj13, adj14})},
       {"adj:2", createAdjValue("2", 2, {adj21, adj23})}},
      {},
      {},
      {},
      std::string(""));

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvMyRouteDb("1", serializer);

  // Query new information
  // validate routers
  routeMapList = dumpRouteDb({"1", "2", "3", "4"});
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  routeMap.clear();
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
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
  fb303::fbData->resetAllData();

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
    if (diff > (3 * debounceTimeoutMax)) {
      LOG(INFO) << "Hammered decision with " << totalSent
                << " updates. Stopping";
      break;
    }
    ++totalSent;
    sendKvPublication(duplicatePub);
  }

  // Receive RouteUpdate from Decision
  auto routesDelta1 = recvMyRouteDb("1", serializer);
  EXPECT_EQ(999, routesDelta1.unicastRoutesToUpdate.size()); // Route to all
                                                             // nodes except
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
  // Receive RouteDelta from Decision
  auto routesDelta2 = recvMyRouteDb("1", serializer);
  // Expect no routes delta
  EXPECT_EQ(0, routesDelta2.unicastRoutesToUpdate.size());

  //
  // Verify counters information
  //

  const int64_t adjUpdateCnt = 1000 /* initial */;
  const int64_t prefixUpdateCnt = totalSent + 1000 /* initial */ + 1 /* end */;
  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(4, counters["decision.spf_runs.count"]);
  EXPECT_EQ(adjUpdateCnt, counters["decision.adj_db_update.count"]);
  EXPECT_EQ(prefixUpdateCnt, counters["decision.prefix_db_update.count"]);
}

/*
 * The following topology is used:
 *
 * 1---2
 *
 * Test case to test if old prefix key expiry does not delete prefixes from
 * FIB when migrating from old prefix key format to 'per prefix key' format
 *
 *  node1 [old key format]   ------- node2 [prefix:node1 {p1, p2, p3}]
 *
 *  now change node1 to 'per prefix key'
 *
 *  node1 [per prefix key]   ------- node2 [prefix:node1 {p1, p2, p3},
 *                                          prefix:node1:p1,
 *                                          prefix:node1:p2,
 *                                          prefix:node1:p3]
 *
 *  "prefix:node1 {p1, p2, p3}" -- will expire, but p1, p2, p3 shouldn't be
 *  removed from decision.
 */

TEST_F(DecisionTestFixture, PerPrefixKeyExpiry) {
  //
  // publish the link state info to KvStore
  //

  auto publication0 = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2",
        createPrefixValue("2", 1, {addr2, addr5, addr6, addr1V4, addr4V4})}},
      {},
      {},
      {},
      std::string(""));
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes.begin(), routeDbBefore.unicastRoutes.end());
  sendKvPublication(publication0);
  auto routeDbDelta = recvMyRouteDb("1", serializer);
  EXPECT_EQ(5, routeDbDelta.unicastRoutesToUpdate.size());
  auto routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes.begin(), routeDb.unicastRoutes.end());
  auto routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));

  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr5))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr6))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr1V4))],
      NextHops({createNextHopFromAdj(adj12, true, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4V4))],
      NextHops({createNextHopFromAdj(adj12, true, 10)}));

  // expire prefix:2, must delete all 5 routes
  auto publication =
      createThriftPublication({}, {"prefix:2"}, {}, {}, std::string(""));
  sendKvPublication(publication);
  routeDbDelta = recvMyRouteDb("1", serializer);
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(5, routeDbDelta.unicastRoutesToDelete.size());

  // add 5 routes using old foramt
  sendKvPublication(publication0);
  routeDbDelta = recvMyRouteDb("1", serializer);
  EXPECT_EQ(5, routeDbDelta.unicastRoutesToUpdate.size());

  // re-add 4 routes using per prefix key format, one of the old key must
  // be deleted
  auto perPrefixKeyValue =
      createPerPrefixKeyValue("2", 1, {addr2, addr6, addr1V4, addr4V4});
  publication =
      createThriftPublication(perPrefixKeyValue, {}, {}, {}, std::string(""));
  sendKvPublication(publication);

  auto routeDb1 = dumpRouteDb({"1"})["1"];
  // expect to still have all 5 routes
  EXPECT_EQ(5, routeDb1.unicastRoutes.size());
  // again send expire 'prefix:2' key, now we expect the one extra route to
  // be deleted
  LOG(INFO) << "Sending prefix key expiry";
  publication =
      createThriftPublication({}, {"prefix:2"}, {}, {}, std::string(""));
  sendKvPublication(publication);
  routeDbDelta = recvMyRouteDb("1", serializer);
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToDelete.size());
  EXPECT_EQ(addr5, routeDbDelta.unicastRoutesToDelete.at(0));
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
