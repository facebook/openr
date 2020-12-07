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
#include <openr/decision/RouteUpdate.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

DEFINE_bool(stress_test, false, "pass this to run the stress test");

using namespace std;
using namespace openr;
using namespace testing;

namespace fb303 = facebook::fb303;

using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;

namespace {

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
    kTestingAreaName)};

// timeout to wait until decision debounce
// (i.e. spf recalculation, route rebuild) finished
const std::chrono::milliseconds debounceTimeoutMin{10};
const std::chrono::milliseconds debounceTimeoutMax{500};

// Empty Perf Events
const thrift::AdjacencyDatabase kEmptyAdjDb;
const apache::thrift::optional_field_ref<thrift::PerfEvents const&>
    kEmptyPerfEventRef{kEmptyAdjDb.perfEvents_ref()};

// TODO @girasoley - Remove this once we implement feature in BGP to not
// program Open/R received routes.
// Decision enforces the need for loopback address of the node
void
addLoopbackAddress(thrift::PrefixDatabase& prefixDb, bool v4Enabled) {
  const auto index = folly::to<size_t>(prefixDb.thisNodeName_ref().value());
  if (v4Enabled) {
    prefixDb.prefixEntries_ref()->emplace_back(
        createPrefixEntry(toIpPrefix(folly::sformat("172.0.0.{}/32", index))));
  } else {
    prefixDb.prefixEntries_ref()->emplace_back(
        createPrefixEntry(toIpPrefix(folly::sformat("fd00::{}/128", index))));
  }
}

thrift::PrefixDatabase
createPrefixDbWithKspfAlgo(
    thrift::PrefixDatabase const& prefixDb,
    std::optional<thrift::PrefixType> prefixType = std::nullopt,
    std::optional<thrift::IpPrefix> prefix = std::nullopt,
    std::optional<uint32_t> prependLabel = std::nullopt,
    bool v4Enabled = false) {
  thrift::PrefixDatabase newPrefixDb = prefixDb;

  for (auto& p : *newPrefixDb.prefixEntries_ref()) {
    p.forwardingType_ref() = thrift::PrefixForwardingType::SR_MPLS;
    p.forwardingAlgorithm_ref() =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    if (prefixType == thrift::PrefixType::BGP and not prefix.has_value()) {
      p.type_ref() = thrift::PrefixType::BGP;
      p.mv_ref() = thrift::MetricVector();
    }
  }

  if (prefix.has_value()) {
    thrift::PrefixEntry entry;
    *entry.prefix_ref() = prefix.value();
    entry.forwardingType_ref() = thrift::PrefixForwardingType::SR_MPLS;
    entry.forwardingAlgorithm_ref() =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    entry.mv_ref() = thrift::MetricVector();
    entry.type_ref() = thrift::PrefixType::BGP;
    if (prependLabel.has_value()) {
      entry.prependLabel_ref() = prependLabel.value();
    }
    newPrefixDb.prefixEntries_ref()->push_back(entry);
  }

  // Add loopback address if any
  if (prefixType == thrift::PrefixType::BGP and not prefix.has_value()) {
    addLoopbackAddress(newPrefixDb, v4Enabled);
  }

  return newPrefixDb;
}

thrift::NextHopThrift
createNextHopFromAdj(
    thrift::Adjacency adj,
    bool isV4,
    int32_t metric,
    std::optional<thrift::MplsAction> mplsAction = std::nullopt,
    const std::string& area = kTestingAreaName) {
  return createNextHop(
      isV4 ? *adj.nextHopV4_ref() : *adj.nextHopV6_ref(),
      *adj.ifName_ref(),
      metric,
      std::move(mplsAction),
      area,
      *adj.otherNodeName_ref());
}

thrift::PrefixMetrics
createMetrics(int32_t pp, int32_t sp, int32_t d) {
  thrift::PrefixMetrics metrics;
  metrics.path_preference_ref() = pp;
  metrics.source_preference_ref() = sp;
  metrics.distance_ref() = d;
  return metrics;
}

thrift::PrefixEntry
createPrefixEntryWithMetrics(
    thrift::IpPrefix const& prefix,
    thrift::PrefixType const& type,
    thrift::PrefixMetrics const& metrics) {
  thrift::PrefixEntry entry;
  entry.prefix_ref() = prefix;
  entry.type_ref() = type;
  entry.metrics_ref() = metrics;
  return entry;
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
    const string& node, RouteMap& routeMap, const DecisionRouteDb& routeDb) {
  for (auto const& [_, entry] : routeDb.unicastRoutes) {
    auto prefix = folly::IPAddress::networkToString(entry.prefix);
    for (const auto& nextHop : entry.nexthops) {
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << toString(nextHop);

      routeMap[make_pair(node, prefix)].emplace(nextHop);
    }
  }
  for (auto const& [_, entry] : routeDb.mplsRoutes) {
    auto topLabelStr = std::to_string(entry.label);
    for (const auto& nextHop : entry.nexthops) {
      VLOG(4) << "node: " << node << " label: " << topLabelStr << " -> "
              << toString(nextHop);
      routeMap[make_pair(node, topLabelStr)].emplace(nextHop);
    }
  }
}

void
fillRouteMap(
    const string& node,
    RouteMap& routeMap,
    const thrift::RouteDatabase& routeDb) {
  for (auto const& route : *routeDb.unicastRoutes_ref()) {
    auto prefix = toString(*route.dest_ref());
    for (const auto& nextHop : *route.nextHops_ref()) {
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << toString(nextHop);

      routeMap[make_pair(node, prefix)].emplace(nextHop);
    }
  }
  for (auto const& route : *routeDb.mplsRoutes_ref()) {
    auto topLabelStr = std::to_string(*route.topLabel_ref());
    for (const auto& nextHop : *route.nextHops_ref()) {
      VLOG(4) << "node: " << node << " label: " << topLabelStr << " -> "
              << toString(nextHop);
      routeMap[make_pair(node, topLabelStr)].emplace(nextHop);
    }
  }
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

// Note: routeMap will be modified
void
fillPrefixRoutes(
    const string& node,
    PrefixRoutes& prefixRoutes,
    const DecisionRouteDb& routeDb) {
  for (auto const& [_, entry] : routeDb.unicastRoutes) {
    auto prefix = folly::IPAddress::networkToString(entry.prefix);
    prefixRoutes[make_pair(node, prefix)] = entry.toThrift();
  }
}

PrefixRoutes
getUnicastRoutes(
    SpfSolver& spfSolver,
    const vector<string>& nodes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  PrefixRoutes prefixRoutes;

  for (string const& node : nodes) {
    auto routeDb = spfSolver.buildRouteDb(node, areaLinkStates, prefixState);
    if (not routeDb.has_value()) {
      continue;
    }

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
        nodeName, std::to_string(*adj.adjLabel_ref())};
    ASSERT_EQ(1, routeMap.count(routeKey));
    EXPECT_EQ(
        routeMap.at(routeKey),
        NextHops({createNextHopFromAdj(
            adj, false, *adj.metric_ref(), labelPhpAction)}));
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
  for (const auto ucRoute : *routeDb.value().unicastRoutes_ref()) {
    LOG(INFO) << "dest: " << toString(*ucRoute.dest_ref());
    if (ucRoute.adminDistance_ref().has_value()) {
      LOG(INFO) << "ad_dis: "
                << static_cast<int>(ucRoute.adminDistance_ref().value());
    }
    if (ucRoute.prefixType_ref().has_value()) {
      LOG(INFO) << "prefix_type: "
                << static_cast<int>(ucRoute.prefixType_ref().value());
    }

    LOG(INFO) << "doNotInstall: " << *ucRoute.doNotInstall_ref();

    for (const auto nh : *ucRoute.nextHops_ref()) {
      LOG(INFO) << "nexthops: " << toString(nh);
    }
  }
}

const auto&
getNextHops(const thrift::UnicastRoute& r) {
  return *r.nextHops_ref();
}

// DPERECTAED: utility finction provided for old test callsites that once used
// PrefixState::updatePrefixDatabase() expecting all node route advertisments to
// be synced.
// Prefer PrefixState::updatePrefix() and PrefixState::deletePrefix() in newly
// written tests
std::unordered_set<thrift::IpPrefix>
updatePrefixDatabase(
    PrefixState& state, thrift::PrefixDatabase const& prefixDb) {
  auto const& nodeName = prefixDb.get_thisNodeName();
  auto const& area = prefixDb.get_area();

  std::unordered_set<PrefixKey> oldKeys, newKeys;
  for (auto const& [_, db] : state.getPrefixDatabases()) {
    if (nodeName == db.get_thisNodeName() && area == db.get_area()) {
      for (auto const& entry : db.get_prefixEntries()) {
        oldKeys.emplace(nodeName, toIPNetwork(entry.get_prefix()), area);
      }
      break;
    }
  }

  std::unordered_set<thrift::IpPrefix> changed;

  for (auto const& entry : prefixDb.get_prefixEntries()) {
    PrefixKey key(nodeName, toIPNetwork(entry.get_prefix()), area);
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  unordered_map<
      pair<string /* node name */, string /* ip prefix */>,
      thrift::UnicastRoute>
      routeMap;

  vector<string> allNodes = {"1", "2"};

  for (string const& node : allNodes) {
    auto routeDb = spfSolver.buildRouteDb(node, areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
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
      nodeName, false /* disable v4 */, false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  PrefixState prefixState;

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  EXPECT_FALSE(routeDb.has_value());

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1 and R2's adjacency + prefix dbs
  //

  {
    auto res = linkState.updateAdjacencyDatabase(adjacencyDb1);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged); // label changed for node1
  }
  {
    auto res = linkState.updateAdjacencyDatabase(adjacencyDb2);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  //
  // dump routes for both nodes, expect 4 route entries (1 unicast, 3 label) on
  // each (node1-label, node2-label and adjacency-label)
  //

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  //
  // Update adjacency database of node 1 by changing it's nexthops and verift
  // that update properly responds to the event
  //
  *adjacencyDb1.adjacencies_ref()[0].nextHopV6_ref() =
      toBinaryAddress("fe80::1234:b00c");
  {
    auto res = linkState.updateAdjacencyDatabase(adjacencyDb1);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.linkAttributesChanged);
  }

  //
  // dump routes for both nodes, expect 4 route entries (1 unicast, 3 label) on
  // each (node1-label, node2-label and adjacency-label)
  //

  routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  //
  // Update adjacency database of node 2 by changing it's nexthops and verift
  // that update properly responds to the event (no spf trigger needed)
  //
  *adjacencyDb2.adjacencies_ref()[0].nextHopV6_ref() =
      toBinaryAddress("fe80::5678:b00c");
  {
    auto res = linkState.updateAdjacencyDatabase(adjacencyDb2);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.linkAttributesChanged);
  }

  //
  // dump routes for both nodes, expect 4 route entries (1 unicast, 3 label) on
  // each (node1-label, node2-label and adjacency-label)
  //

  routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(3, routeDb->mplsRoutes.size()); // node and adj route

  //
  // Change adjLabel. This should report route-attribute change only for node1
  // and not for node2's adjLabel change
  //

  adjacencyDb1.adjacencies_ref()[0].adjLabel_ref() = 111;
  {
    auto res = linkState.updateAdjacencyDatabase(adjacencyDb1);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.linkAttributesChanged);
  }

  adjacencyDb2.adjacencies_ref()[0].adjLabel_ref() = 222;
  {
    auto res = linkState.updateAdjacencyDatabase(adjacencyDb2);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.linkAttributesChanged);
  }

  // Change nodeLabel.
  adjacencyDb1.nodeLabel_ref() = 11;
  {
    auto res = linkState.updateAdjacencyDatabase(adjacencyDb1);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_FALSE(res.linkAttributesChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }

  adjacencyDb2.nodeLabel_ref() = 22;
  {
    auto res = linkState.updateAdjacencyDatabase(adjacencyDb2);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_FALSE(res.linkAttributesChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);

  PrefixState prefixState;
  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj23}, 0); // No node label
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, false),
      linkState.updateAdjacencyDatabase(adjacencyDb1));

  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, false),
      linkState.updateAdjacencyDatabase(adjacencyDb2));

  EXPECT_EQ(
      LinkState::LinkStateChange(true, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb3));

  auto routeMap =
      getRouteMap(spfSolver, {"1", "2", "3"}, areaLinkStates, prefixState);
  EXPECT_EQ(5, routeMap.size());

  // Validate 1's routes
  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());

  // Validate 2's routes (no node label route)
  validateAdjLabelRoutes(routeMap, "2", {adj23});

  // Validate 3's routes
  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", {adj32});
}

TEST(BGPRedistribution, BasicOperation) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  thrift::PrefixDatabase prefixDb1WithBGP = prefixDb1;
  thrift::PrefixDatabase prefixDb2WithBGP = prefixDb2;

  std::string data1 = "data1", data2 = "data2";
  thrift::IpPrefix bgpPrefix1 = addr3;

  thrift::MetricVector mv1, mv2;
  int64_t numMetrics = 5;
  mv1.metrics_ref()->resize(numMetrics);
  mv2.metrics_ref()->resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    mv1.metrics_ref()[i].type_ref() = i;
    mv2.metrics_ref()[i].type_ref() = i;
    mv1.metrics_ref()[i].priority_ref() = i;
    mv2.metrics_ref()[i].priority_ref() = i;
    mv1.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    mv2.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    mv1.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    mv2.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    *mv1.metrics_ref()[i].metric_ref() =
        *mv2.metrics_ref()[i].metric_ref() = {i};
  }

  // only node1 advertises the BGP prefix, it will have the best path
  prefixDb1WithBGP.prefixEntries_ref()->push_back(createPrefixEntry(
      bgpPrefix1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      mv1,
      std::nullopt));

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1WithBGP).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2WithBGP).empty());

  auto decisionRouteDb =
      *spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  auto routeDb = decisionRouteDb.toThrift();
  auto route1 = createUnicastRoute(
      bgpPrefix1, {createNextHopFromAdj(adj21, false, *adj21.metric_ref())});
  route1.prefixType_ref() = thrift::PrefixType::BGP;
  route1.data_ref() = data1;
  route1.doNotInstall_ref() = false;

  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(2));
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::Contains(route1));

  // add the prefix to node2 with the same metric vector. we expect the bgp
  // route to be gone since both nodes have same metric vector we can't
  // determine a best path
  prefixDb2WithBGP.prefixEntries_ref()->push_back(createPrefixEntry(
      bgpPrefix1,
      thrift::PrefixType::BGP,
      data2,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      mv2,
      std::nullopt));
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2WithBGP).empty());
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(1));

  // decrease the one of second node's metrics and expect to see the route
  // toward just the first
  prefixDb2WithBGP.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .metric_ref()
      ->front()--;
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2WithBGP).empty());
  decisionRouteDb = *spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(2));
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::Contains(route1));

  // now make 2 better
  prefixDb2WithBGP.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .metric_ref()
      ->front() += 2;
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2WithBGP).empty());

  auto route2 = createUnicastRoute(
      bgpPrefix1, {createNextHopFromAdj(adj12, false, *adj12.metric_ref())});
  route2.prefixType_ref() = thrift::PrefixType::BGP;
  route2.data_ref() = data2;
  route2.doNotInstall_ref() = false;

  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(2));
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::Contains(route2));

  // now make that a tie break for a multipath route
  prefixDb1WithBGP.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .isBestPathTieBreaker_ref() = true;
  prefixDb2WithBGP.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .isBestPathTieBreaker_ref() = true;
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1WithBGP).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2WithBGP).empty());

  // 1 and 2 will not program BGP route
  EXPECT_THAT(
      spfSolver.buildRouteDb("1", areaLinkStates, prefixState)
          .value()
          .unicastRoutes,
      testing::SizeIs(1));

  // 3 will program the BGP route towards both
  decisionRouteDb = *spfSolver.buildRouteDb("3", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::Contains(AllOf(
          Truly([&data2, &bgpPrefix1](auto i) {
            return i.dest_ref() == bgpPrefix1 and i.data_ref() == data2;
          }),
          ResultOf(
              getNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj31, false, 10))))));

  // dicsonnect the network, each node will consider it's BGP route the best,
  // and thus not program anything
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(createAdjDb("1", {}, 0))
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::AllOf(
          testing::Not(testing::Contains(route1)),
          testing::Not(testing::Contains(route2))));

  decisionRouteDb = *spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
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
  const std::string data1{"data1"};
  const auto expectedAddr = addr1;
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* enableV4 */,
      false /* computeLfaPaths */,
      false /* enableOrderedFib */,
      false /* bgpDryRun */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Create BGP prefix
  //
  thrift::MetricVector metricVector;
  int64_t numMetrics = 5;
  metricVector.metrics_ref()->resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    metricVector.metrics_ref()[i].type_ref() = i;
    metricVector.metrics_ref()[i].priority_ref() = i;
    metricVector.metrics_ref()[i].op_ref() =
        thrift::CompareType::WIN_IF_PRESENT;
    metricVector.metrics_ref()[i].isBestPathTieBreaker_ref() =
        (i == numMetrics - 1);
    *metricVector.metrics_ref()[i].metric_ref() = {i};
  }
  const auto bgpPrefix2 = createPrefixEntry(
      addr1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      metricVector);
  // Make tie breaking metric different
  *metricVector.metrics_ref()->at(4).metric_ref() = {100}; // Make it different
  const auto bgpPrefix3 = createPrefixEntry(
      addr1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      metricVector);

  //
  // Setup adjacencies
  //
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

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
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::Contains(AllOf(
          Truly([&data1, &expectedAddr](auto i) {
            return i.data_ref() == data1 and i.dest_ref() == expectedAddr;
          }),
          ResultOf(
              getNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10),
                  createNextHopFromAdj(adj13, false, 10))))));

  //
  // Increase cost towards node3 to 20; prefix -> {node2}
  //
  adjacencyDb1.adjacencies_ref()[1].metric_ref() = 20;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::Contains(AllOf(
          Truly([&data1, &expectedAddr](auto i) {
            return i.data_ref() == data1 and i.dest_ref() == expectedAddr;
          }),
          ResultOf(
              getNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10))))));

  //
  // mark link towards node2 as drained; prefix1 -> {node3}
  // No route towards addr2 (node2's loopback)
  //
  adjacencyDb1.adjacencies_ref()[0].isOverloaded_ref() = true;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();

  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(2));
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::Contains(AllOf(
          Truly([&data1, &expectedAddr](auto i) {
            return i.data_ref() == data1 and i.dest_ref() == expectedAddr;
          }),
          ResultOf(
              getNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj13, false, 20))))));

  //
  // Set cost towards node2 to 20 (still drained); prefix1 -> {node3}
  // No route towards addr2 (node2's loopback)
  //
  adjacencyDb1.adjacencies_ref()[0].metric_ref() = 20;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(2));
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::Contains(AllOf(
          Truly([&data1, &expectedAddr](auto i) {
            return i.data_ref() == data1 and i.dest_ref() == expectedAddr;
          }),
          ResultOf(
              getNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj13, false, 20))))));

  //
  // Undrain link; prefix1 -> {node2, node3}
  //
  adjacencyDb1.adjacencies_ref()[0].isOverloaded_ref() = false;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::Contains(AllOf(
          Truly([&data1, &expectedAddr](auto i) {
            return i.data_ref() == data1 and i.dest_ref() == expectedAddr;
          }),
          ResultOf(
              getNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 20),
                  createNextHopFromAdj(adj13, false, 20))))));
}

TEST(Decision, BestRouteSelection) {
  std::string nodeName("1");
  const auto expectedAddr = addr1;
  SpfSolver spfSolver(
      nodeName,
      false /* enableV4 */,
      false /* computeLfaPaths */,
      false /* enableOrderedFib */,
      false /* bgpDryRun */,
      true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;

  //
  // Setup adjacencies
  // 2 <--> 1 <--> 3
  //
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 3);
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  //
  // Setup prefixes. node2 and node3 announces the same prefix with same metrics
  //
  const auto node2Prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 0, 0));
  const auto node3Prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 0, 0));
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
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(1));
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::Contains(AllOf(
          Truly(
              [&expectedAddr](auto i) { return i.dest_ref() == expectedAddr; }),
          ResultOf(
              getNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10),
                  createNextHopFromAdj(adj13, false, 10))))));

  //
  // Verify that prefix-state report two best routes
  //
  {
    auto bestRoutesCache = spfSolver.getBestRoutesCache();
    ASSERT_EQ(1, bestRoutesCache.count(addr1));
    auto& bestRoutes = bestRoutesCache.at(addr1);
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
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(1));
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref(),
      testing::Contains(AllOf(
          Truly(
              [&expectedAddr](auto i) { return i.dest_ref() == expectedAddr; }),
          ResultOf(
              getNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10))))));
  //
  // Verify that prefix-state report only one best route
  //
  {
    auto bestRoutesCache = spfSolver.getBestRoutesCache();
    ASSERT_EQ(1, bestRoutesCache.count(addr1));
    auto& bestRoutes = bestRoutesCache.at(addr1);
    EXPECT_EQ(1, bestRoutes.allNodeAreas.size());
    EXPECT_EQ(1, bestRoutes.allNodeAreas.count({"2", kTestingAreaName}));
    EXPECT_EQ("2", bestRoutes.bestNodeArea.first);
  }

  //
  // Verify that forwarding type is selected from the best entry
  // node2 - advertising SR_MPLS forwarding type
  // node3 - advertising IP forwarding type
  // Decision chooses MPLS entry
  //
  auto node2PrefixPreferredMpls = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 100, 0));
  node2PrefixPreferredMpls.forwardingType_ref() =
      thrift::PrefixForwardingType::SR_MPLS;
  EXPECT_FALSE(updatePrefixDatabase(
                   prefixState, createPrefixDb("2", {node2PrefixPreferredMpls}))
                   .empty());
  decisionRouteDb = *spfSolver.buildRouteDb("3", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes_ref(), testing::SizeIs(1));
  auto push2 = createMplsAction(
      thrift::MplsActionCode::PUSH, std::nullopt, std::vector<int32_t>{2});
  LOG(INFO) << toString(routeDb.unicastRoutes_ref()->at(0));
  EXPECT_EQ(*routeDb.unicastRoutes_ref()->at(0).dest_ref(), addr1);
  EXPECT_THAT(
      *routeDb.unicastRoutes_ref()->at(0).nextHops_ref(),
      testing::UnorderedElementsAre(
          createNextHopFromAdj(adj31, false, 20, push2)));
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb1));
  EXPECT_EQ(
      LinkState::LinkStateChange(!partitioned, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb2));
  EXPECT_EQ(
      LinkState::LinkStateChange(!partitioned, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb3));

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  // route from 1 to 3
  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  bool foundRouteV6 = false;
  bool foundRouteNodeLabel = false;
  if (routeDb.has_value()) {
    for (auto const& [prefix, _] : routeDb->unicastRoutes) {
      if (toIpPrefix(prefix) == addr3) {
        foundRouteV6 = true;
        break;
      }
    }
    for (auto const& [label, _] : routeDb->mplsRoutes) {
      if (label == 3) {
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  // Make node-2 overloaded
  adjacencyDb2.isOverloaded_ref() = true;

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  auto routeMap =
      getRouteMap(spfSolver, {"1", "2", "3"}, areaLinkStates, prefixState);

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
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj12, false, *adj12.metric_ref(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj21, false, *adj21.metric_ref(), labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj23, false, *adj23.metric_ref(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj32, false, *adj32.metric_ref(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12_old_1}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21_old_1, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32, adj31_old}, 3);

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);

  // add/update adjacency of node1 with old versions
  adjacencyDb1 = createAdjDb("1", {adj12_old_1, adj13_old}, 1);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  adjacencyDb1 = createAdjDb("1", {adj12_old_2, adj13_old}, 1);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);

  auto routeMap =
      getRouteMap(spfSolver, {"1", "2", "3"}, areaLinkStates, prefixState);

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
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12_old_2, false, 20, labelPhpAction),
                createNextHopFromAdj(adj13_old, false, 20, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj13_old, false, *adj13_old.metric_ref(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj21, false, *adj21.metric_ref(), labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj23, false, *adj23.metric_ref(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj31, false, *adj31.metric_ref(), labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(
          adj32, false, *adj32.metric_ref(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // adjacency update (remove adjacency) for node1
  adjacencyDb1 = createAdjDb("1", {adj12_old_2}, 0);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  adjacencyDb3 = createAdjDb("3", {adj32}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  adjacencyDb1 = createAdjDb("1", {adj12_old_2, adj13_old}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
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

    areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);

    EXPECT_EQ(
        LinkState::LinkStateChange(false, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb1));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb2));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb3));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb4));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;

    auto bgp1 = v4Enabled ? bgpAddr1V4 : bgpAddr1;
    auto bgp2 = v4Enabled ? bgpAddr2V4 : bgpAddr2;
    auto bgp3 = v4Enabled ? bgpAddr3V4 : bgpAddr3;
    auto bgp4 = v4Enabled ? bgpAddr4V4 : bgpAddr4;

    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb1,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp1)
                                    : std::nullopt,
                  std::nullopt,
                  v4Enabled)
            : pdb1);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb2,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp2)
                                    : std::nullopt,
                  std::nullopt,
                  v4Enabled)
            : pdb2);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb3,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp3)
                                    : std::nullopt,
                  std::nullopt,
                  v4Enabled)
            : pdb3);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb4,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp4)
                                    : std::nullopt,
                  std::nullopt,
                  v4Enabled)
            : pdb4);
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  std::unique_ptr<SpfSolver> spfSolver;
  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;
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
  auto routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);

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
      NextHops({createNextHopFromAdj(adj14, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj12, v4Enabled, 20, push4),
                createNextHopFromAdj(adj13, v4Enabled, 20, push4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj14, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj12, v4Enabled, 20, push3),
                createNextHopFromAdj(adj14, v4Enabled, 20, push3)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj13, v4Enabled, 20, push2),
                createNextHopFromAdj(adj14, v4Enabled, 20, push2)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  adjacencyDb3.isOverloaded_ref() = true;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);
  routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj14, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj12, v4Enabled, 20, push4)}));
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

    areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);

    EXPECT_EQ(
        LinkState::LinkStateChange(false, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb1));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb2));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb3));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb4));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;

    auto bgp1 = v4Enabled ? bgpAddr1V4 : bgpAddr1;
    auto bgp2 = v4Enabled ? bgpAddr2V4 : bgpAddr2;
    auto bgp3 = v4Enabled ? bgpAddr3V4 : bgpAddr3;
    auto bgp4 = v4Enabled ? bgpAddr4V4 : bgpAddr4;

    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb1,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp1)
                                    : std::nullopt,
                  std::nullopt,
                  v4Enabled)
            : pdb1);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb2,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp2)
                                    : std::nullopt,
                  std::nullopt,
                  v4Enabled)
            : pdb2);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb3,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp3)
                                    : std::nullopt,
                  std::nullopt,
                  v4Enabled)
            : pdb3);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  pdb4,
                  prefixType,
                  createNewBgpRoute ? std::make_optional<thrift::IpPrefix>(bgp4)
                                    : std::nullopt,
                  std::nullopt,
                  v4Enabled)
            : pdb4);
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

    int find = 0;
    for (const auto& mplsRoute : deltaRoutes.mplsRoutesToUpdate) {
      if (mplsRoute.label == mplsLabel) {
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
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

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
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20),
                createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20),
                createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20),
                createNextHopFromAdj(adj43, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "4", *adjacencyDb4.adjacencies_ref());
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
  adjacencyDb1.nodeLabel_ref() = 2;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  linkState.updateAdjacencyDatabase(adjacencyDb1);

  // verify route DB change in node 1, 2 ,3.
  // verify that only one route to mpls lable 1 is installed in all nodes
  DecisionRouteDb emptyRouteDb;
  verifyRouteInUpdateNoDelete("1", 2, emptyRouteDb);

  verifyRouteInUpdateNoDelete("2", 2, emptyRouteDb);

  verifyRouteInUpdateNoDelete("3", 2, emptyRouteDb);

  auto counters = fb303::fbData->getCounters();
  // verify the counters to be 3 because each node will noticed a duplicate
  // for mpls label 1.
  EXPECT_EQ(counters.at("decision.duplicate_node_label.count.60"), 3);

  auto compDb1 =
      spfSolver->buildRouteDb("1", areaLinkStates, prefixState).value();
  auto compDb2 =
      spfSolver->buildRouteDb("2", areaLinkStates, prefixState).value();
  auto compDb3 =
      spfSolver->buildRouteDb("3", areaLinkStates, prefixState).value();

  counters = fb303::fbData->getCounters();
  // now the counter should be 6, becasue we called buildRouteDb 3 times.
  EXPECT_EQ(counters.at("decision.duplicate_node_label.count.60"), 6);

  // change nodelabel of node 1 to be 1. Now each node has it's own
  // mpls label, there should be no duplicate.
  // verify that there is an update entry for mpls route to label 1.
  // verify that no withdrawals of mpls routes to label 1.
  adjacencyDb1.nodeLabel_ref() = 1;
  linkState.updateAdjacencyDatabase(adjacencyDb1);
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
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

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
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20),
                createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20),
                createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20),
                createNextHopFromAdj(adj43, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "4", *adjacencyDb4.adjacencies_ref());
}

/**
 * Validates the IP to MPLS path generataion for shortest path
 * - No label prepend
 * - Label prepend
 * - Min-nexthop requirement
 * - Label prepend with static next-hops
 */
TEST_P(SimpleRingTopologyFixture, IpToMplsLabelPrepend) {
  const int32_t prependLabel{10001};
  CustomSetUp(
      false /* multipath - ignored */,
      true /* useKsp2Ed */,
      std::get<1>(GetParam()));

  auto const push1 = createMplsAction(
      thrift::MplsActionCode::PUSH,
      std::nullopt,
      std::vector<int32_t>{adjacencyDb1.nodeLabel_ref().value()});
  auto const pushPrepend = createMplsAction(
      thrift::MplsActionCode::PUSH,
      std::nullopt,
      std::vector<int32_t>{prependLabel});
  auto const push1Prepend = createMplsAction(
      thrift::MplsActionCode::PUSH,
      std::nullopt,
      std::vector<int32_t>{prependLabel, adjacencyDb1.nodeLabel_ref().value()});
  auto const push4Prepend = createMplsAction(
      thrift::MplsActionCode::PUSH,
      std::nullopt,
      std::vector<int32_t>{prependLabel, adjacencyDb4.nodeLabel_ref().value()});

  //
  // Case-1 IP to MPLS routes towards node1
  //
  auto prefixDb = prefixState.getPrefixDatabases()["1"];
  auto& entry1 = prefixDb.prefixEntries_ref()->front();
  for (auto& prefixEntry : prefixDb.prefixEntries_ref().value()) {
    prefixEntry.forwardingAlgorithm_ref() =
        thrift::PrefixForwardingAlgorithm::SP_ECMP;
  }
  if (entry1.mv_ref()) {
    // Add metric entity with tie-breaker for best path calculation success
    thrift::MetricEntity entity;
    entity.type_ref() = 1;
    entity.priority_ref() = 1;
    entity.isBestPathTieBreaker_ref() = true;
    entity.metric_ref()->emplace_back(1);
    entry1.mv_ref()->metrics_ref()->push_back(entity);
  }
  updatePrefixDatabase(prefixState, prefixDb);
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  EXPECT_EQ(
      0, routeMap.count(make_pair("1", toString(v4Enabled ? addr1V4 : addr1))));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20, push1),
                createNextHopFromAdj(adj43, v4Enabled, 20, push1)}));

  //
  // Case-2 Min-nexthop requirement. No route on node2 and node3 because it
  // doesn't qualify min-nexthop requirement of 2
  //
  entry1.minNexthop_ref() = 2;
  updatePrefixDatabase(prefixState, prefixDb);
  routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  EXPECT_EQ(
      0, routeMap.count(make_pair("1", toString(v4Enabled ? addr1V4 : addr1))));
  EXPECT_EQ(
      0, routeMap.count(make_pair("2", toString(v4Enabled ? addr1V4 : addr1))));
  EXPECT_EQ(
      0, routeMap.count(make_pair("3", toString(v4Enabled ? addr1V4 : addr1))));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20, push1),
                createNextHopFromAdj(adj43, v4Enabled, 20, push1)}));

  //
  // Case-3 Add label prepend instruction
  //
  entry1.prependLabel_ref() = prependLabel;
  updatePrefixDatabase(prefixState, prefixDb);
  routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  EXPECT_EQ(
      0, routeMap.count(make_pair("1", toString(v4Enabled ? addr1V4 : addr1))));
  EXPECT_EQ(
      0, routeMap.count(make_pair("2", toString(v4Enabled ? addr1V4 : addr1))));
  EXPECT_EQ(
      0, routeMap.count(make_pair("3", toString(v4Enabled ? addr1V4 : addr1))));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20, push1Prepend),
                createNextHopFromAdj(adj43, v4Enabled, 20, push1Prepend)}));

  //
  // Case-4 Announce same prefix from node4
  //

  // Advertise addr1[V4] from node4 as well with prepend label
  auto prefixDb4 = prefixState.getPrefixDatabases()["4"];
  prefixDb4.prefixEntries_ref()->emplace_back(entry1);
  if (entry1.mv_ref()) {
    auto& entry4 = prefixDb4.prefixEntries_ref()->back();
    entry4.mv_ref()->metrics_ref()->at(0).metric_ref()->at(0) += 1;
  }
  updatePrefixDatabase(prefixState, prefixDb4);

  // insert the new nexthop to mpls static routes cache
  const auto nh1Addr = toBinaryAddress("1.1.1.1");
  const auto nh2Addr = toBinaryAddress("2.2.2.2");
  const auto phpAction = createMplsAction(thrift::MplsActionCode::PHP);
  std::vector<thrift::NextHopThrift> staticNextHops{
      createNextHop(nh1Addr, std::nullopt, 0, phpAction),
      createNextHop(nh2Addr, std::nullopt, 0, phpAction)};
  staticNextHops.at(0).area_ref().reset();
  staticNextHops.at(1).area_ref().reset();
  thrift::RouteDatabaseDelta routesDelta;
  routesDelta.mplsRoutesToUpdate_ref() = {
      createMplsRoute(prependLabel, staticNextHops)};
  spfSolver->updateStaticRoutes(std::move(routesDelta));

  // Get and verify next-hops. Both node1 & node4 will report static next-hops
  // NOTE: PUSH action is removed.
  routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20, push4Prepend),
                createNextHopFromAdj(adj13, v4Enabled, 20, push4Prepend),
                createNextHop(nh1Addr, std::nullopt, 0, std::nullopt),
                createNextHop(nh2Addr, std::nullopt, 0, std::nullopt)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10, pushPrepend),
                createNextHopFromAdj(adj24, v4Enabled, 10, pushPrepend)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10, pushPrepend),
                createNextHopFromAdj(adj34, v4Enabled, 10, pushPrepend)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20, push1Prepend),
                createNextHopFromAdj(adj43, v4Enabled, 20, push1Prepend),
                createNextHop(nh1Addr, std::nullopt, 0, std::nullopt),
                createNextHop(nh2Addr, std::nullopt, 0, std::nullopt)}));
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
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(
      (std::get<1>(GetParam()) == thrift::PrefixType::BGP ? 48 : 36),
      routeMap.size());

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
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20, push4),
                createNextHopFromAdj(adj13, v4Enabled, 20, push4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj12, v4Enabled, 30, push34)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj13, v4Enabled, 30, push24)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj21, v4Enabled, 30, push43)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20, push3),
                createNextHopFromAdj(adj24, v4Enabled, 20, push3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj24, v4Enabled, 30, push13)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj31, v4Enabled, 30, push42)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20, push2),
                createNextHopFromAdj(adj34, v4Enabled, 20, push2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj34, v4Enabled, 30, push12)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj42, v4Enabled, 30, push31)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10, std::nullopt),
                createNextHopFromAdj(adj43, v4Enabled, 30, push21)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20, push1),
                createNextHopFromAdj(adj43, v4Enabled, 20, push1)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "4", *adjacencyDb4.adjacencies_ref());

  // this is to test corner cases for traceEdgeDisjointPaths algorithm.
  // In this example, node 3 is overloaded, and link between node 1 and node 2
  // are overloaded. In such case, there is no route from node 1 to node 2 and 4
  adjacencyDb1.adjacencies_ref()[0].isOverloaded_ref() = true;
  adjacencyDb3.isOverloaded_ref() = true;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);
  routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);

  EXPECT_EQ(
      routeMap.find(make_pair("1", toString(v4Enabled ? addr4V4 : addr4))),
      routeMap.end());

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10, std::nullopt)}));

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
  mv1.metrics_ref()->resize(numMetrics);
  mv2.metrics_ref()->resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    mv1.metrics_ref()[i].type_ref() = i;
    mv2.metrics_ref()[i].type_ref() = i;
    mv1.metrics_ref()[i].priority_ref() = i;
    mv2.metrics_ref()[i].priority_ref() = i;
    mv1.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    mv2.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    mv1.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    mv2.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    *mv1.metrics_ref()[i].metric_ref() =
        *mv2.metrics_ref()[i].metric_ref() = {i};
  }

  auto prefixDBs = prefixState.getPrefixDatabases();
  auto prefixDBOne = prefixDBs["1"];
  auto prefixDBTwo = prefixDBs["2"];

  // set metric vector for two prefixes in two nodes to be same
  prefixDBOne.prefixEntries_ref()->at(1).mv_ref() = mv1;
  prefixDBTwo.prefixEntries_ref()->push_back(
      prefixDBOne.prefixEntries_ref()->at(1));
  // only node 1 is announcing the prefix with prependLabel.
  // node 2 is announcing the prefix without prependLabel.
  prefixDBOne.prefixEntries_ref()[1].prependLabel_ref() = 60000;

  updatePrefixDatabase(prefixState, prefixDBOne);
  updatePrefixDatabase(prefixState, prefixDBTwo);

  auto routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);

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
  prefixDBTwo.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .metric_ref()
      ->front()--;

  // change data to some special case for verification
  prefixDBOne.prefixEntries_ref()->at(1).data_ref() = "123";
  updatePrefixDatabase(prefixState, prefixDBTwo);
  updatePrefixDatabase(prefixState, prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 10, pushPrependLabel),
           createNextHopFromAdj(adj34, v4Enabled, 30, push12AndPrependLabel)}));

  auto route = getUnicastRoutes(
      *spfSolver,
      {"3"},
      areaLinkStates,
      prefixState)[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))];

  // increase mv for the second node by 2, now router 3 should point to 2
  prefixDBTwo.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .metric_ref()
      ->front() += 2;
  updatePrefixDatabase(prefixState, prefixDBTwo);
  routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20, push2),
                createNextHopFromAdj(adj34, v4Enabled, 20, push2)}));

  route = getUnicastRoutes(
      *spfSolver,
      {"3"},
      areaLinkStates,
      prefixState)[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))];

  // set the tie breaker to be true. in this case, both nodes will be selected
  prefixDBTwo.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .isBestPathTieBreaker_ref() = true;

  prefixDBOne.prefixEntries_ref()
      ->at(1)
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .isBestPathTieBreaker_ref() = true;
  updatePrefixDatabase(prefixState, prefixDBTwo);
  updatePrefixDatabase(prefixState, prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);

  // createNextHopFromAdj(adj34, v4Enabled, 30, push12, true) getting ignored
  // because in kspf, we will ignore second shortest path if it starts with
  // one of shortest path for anycast prefix
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20, push2),
                createNextHopFromAdj(adj34, v4Enabled, 20, push2),
                createNextHopFromAdj(adj31, v4Enabled, 10, pushPrependLabel)}));

  route = getUnicastRoutes(
      *spfSolver,
      {"3"},
      areaLinkStates,
      prefixState)[make_pair("3", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))];

  EXPECT_EQ(route.prefixType_ref(), thrift::PrefixType::BGP);

  // verify on node 1. From node 1 point of view, both node 1 and node 2 are
  // are annoucing the prefix. So it will program 3 nexthops.
  // 1: recursively resolved MPLS nexthops of prependLabel
  // 2: shortest path to node 2.
  // 3: second shortest path node 2.
  int32_t staticMplsRouteLabel = 60000;
  // insert the new nexthop to mpls static routes cache
  thrift::NextHopThrift nh;
  *nh.address_ref() = toBinaryAddress("1.1.1.1");
  nh.mplsAction_ref() = createMplsAction(thrift::MplsActionCode::PHP);
  thrift::MplsRoute staticMplsRoute;
  staticMplsRoute.topLabel_ref() = staticMplsRouteLabel;
  staticMplsRoute.nextHops_ref()->emplace_back(nh);
  thrift::RouteDatabaseDelta routesDelta;
  *routesDelta.mplsRoutesToUpdate_ref() = {staticMplsRoute};
  spfSolver->updateStaticRoutes(std::move(routesDelta));

  routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);
  // NOTE: 60000 is the static MPLS route on node 2 which prevent routing loop.
  auto push24 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 4});
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops({createNextHop(
                    toBinaryAddress("1.1.1.1"), std::nullopt, 0, std::nullopt),
                createNextHopFromAdj(adj13, v4Enabled, 30, push24),
                createNextHopFromAdj(adj12, v4Enabled, 10, std::nullopt)}));
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
  mv1.metrics_ref()->resize(numMetrics);
  mv2.metrics_ref()->resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    mv1.metrics_ref()[i].type_ref() = i;
    mv2.metrics_ref()[i].type_ref() = i;
    mv1.metrics_ref()[i].priority_ref() = i;
    mv2.metrics_ref()[i].priority_ref() = i;
    mv1.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    mv2.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    mv1.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    mv2.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    *mv1.metrics_ref()[i].metric_ref() =
        *mv2.metrics_ref()[i].metric_ref() = {i};
  }

  auto prefixDBs = prefixState.getPrefixDatabases();
  auto prefixDBOne = prefixDBs["1"];
  auto prefixDBTwo = prefixDBs["2"];

  // set metric vector for two prefixes in two nodes to be same
  prefixDBOne.prefixEntries_ref()->at(1).mv_ref() = mv1;
  prefixDBTwo.prefixEntries_ref()->push_back(
      prefixDBOne.prefixEntries_ref()->at(1));
  prefixDBOne.prefixEntries_ref()->at(1).prependLabel_ref() = 60000;

  // increase mv for the second node by 2, now router 3 should point to 2
  prefixDBTwo.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .metric_ref()
      ->front() += 1;

  // set the tie breaker to be true. in this case, both nodes will be selected
  prefixDBTwo.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .isBestPathTieBreaker_ref() = true;

  prefixDBOne.prefixEntries_ref()
      ->at(1)
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .isBestPathTieBreaker_ref() = true;

  updatePrefixDatabase(prefixState, prefixDBOne);
  updatePrefixDatabase(prefixState, prefixDBTwo);
  auto pushCode = thrift::MplsActionCode::PUSH;

  // verify on node 1. From node 1 point of view, both node 1 and node 2 are
  // are annoucing the prefix. So it will program 3 nexthops.
  // 1: recursively resolved MPLS nexthops of prependLabel
  // 2: shortest path to node 2.
  // 3: second shortest path node 2.
  int32_t staticMplsRouteLabel = 60000;
  // insert the new nexthop to mpls static routes cache
  thrift::NextHopThrift nh;
  *nh.address_ref() = toBinaryAddress("1.1.1.1");
  nh.mplsAction_ref() = createMplsAction(thrift::MplsActionCode::PHP);
  thrift::MplsRoute staticMplsRoute;
  staticMplsRoute.topLabel_ref() = staticMplsRouteLabel;
  staticMplsRoute.nextHops_ref()->emplace_back(nh);
  thrift::RouteDatabaseDelta routesDelta;
  *routesDelta.mplsRoutesToUpdate_ref() = {staticMplsRoute};
  spfSolver->updateStaticRoutes(std::move(routesDelta));

  auto routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);
  // NOTE: 60000 is the static MPLS route on node 2 which prevent routing loop.
  auto push24 =
      createMplsAction(pushCode, std::nullopt, std::vector<int32_t>{2, 4});
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? bgpAddr1V4 : bgpAddr1))],
      NextHops({createNextHop(
                    toBinaryAddress("1.1.1.1"), std::nullopt, 0, std::nullopt),
                createNextHopFromAdj(adj13, v4Enabled, 30, push24),
                createNextHopFromAdj(adj12, v4Enabled, 10, std::nullopt)}));

  prefixDBOne.prefixEntries_ref()->at(1).minNexthop_ref() = 3;
  updatePrefixDatabase(prefixState, prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);

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
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb4).empty());

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

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
  adjacencyDb2.isOverloaded_ref() = true;
  adjacencyDb3.isOverloaded_ref() = true;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 2 + 3 + 3 + 2 = 10
  // Node label routes => 3 + 4 + 4 + 3 = 14
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(32, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)})); // No LFA
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 20),
                createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 20),
                createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "4", *adjacencyDb4.adjacencies_ref());
}

//
// Verify overload bit setting of individual adjacencies with multipath
// enabled. node-3 will get disconnected
//
TEST_P(SimpleRingTopologyFixture, OverloadLinkTest) {
  CustomSetUp(true /* multipath */, false /* useKsp2Ed */);
  adjacencyDb3.adjacencies_ref()[0].isOverloaded_ref() =
      true; // make adj31 overloaded
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(36, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 30)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 30, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3
  // no routes for router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 30)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34, false, 30, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "4", *adjacencyDb4.adjacencies_ref());

  // Now also make adj34 overloaded which will disconnect the node-3
  adjacencyDb3.adjacencies_ref()[1].isOverloaded_ref() = true;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 2 + 2 + 0 + 2 = 6
  // Node label routes => 3 * 3 + 1 = 10
  // Adj label routes => 4 * 2 = 8
  EXPECT_EQ(24, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "4", *adjacencyDb4.adjacencies_ref());
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
    areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);
    EXPECT_FALSE(
        linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb4).topologyChanged);

    // Prefix db's

    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  prefixDb1, prefixType, std::nullopt, std::nullopt, false)
            : prefixDb1);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  prefixDb2, prefixType, std::nullopt, std::nullopt, false)
            : prefixDb2);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  prefixDb3, prefixType, std::nullopt, std::nullopt, false)
            : prefixDb3);
    updatePrefixDatabase(
        prefixState,
        useKsp2Ed
            ? createPrefixDbWithKspfAlgo(
                  prefixDb4, prefixType, std::nullopt, std::nullopt, false)
            : prefixDb4);
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

INSTANTIATE_TEST_CASE_P(
    ParallelAdjRingTopologyInstance,
    ParallelAdjRingTopologyFixture,
    ::testing::Values(std::nullopt, thrift::PrefixType::BGP));

TEST_F(ParallelAdjRingTopologyFixture, ShortestPathTest) {
  CustomSetUp(false /* shortest path */, false /* useKsp2Ed */);
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

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
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12_2, false, 22, labelSwapAction4),
                createNextHopFromAdj(adj13_1, false, 22, labelSwapAction4),
                createNextHopFromAdj(adj12_1, false, 22, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_2, false, 11),
                createNextHopFromAdj(adj12_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12_2, false, 11, labelPhpAction),
                createNextHopFromAdj(adj12_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21_2, false, 22),
                createNextHopFromAdj(adj21_1, false, 22),
                createNextHopFromAdj(adj24_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21_2, false, 22, labelSwapAction3),
                createNextHopFromAdj(adj21_1, false, 22, labelSwapAction3),
                createNextHopFromAdj(adj24_1, false, 22, labelSwapAction3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21_2, false, 11),
                createNextHopFromAdj(adj21_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21_2, false, 11, labelPhpAction),
                createNextHopFromAdj(adj21_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22),
                createNextHopFromAdj(adj34_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, labelSwapAction2),
                createNextHopFromAdj(adj34_1, false, 22, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22),
                createNextHopFromAdj(adj43_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22, labelSwapAction1),
                createNextHopFromAdj(adj43_1, false, 22, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "4", *adjacencyDb4.adjacencies_ref());
}

//
// Use the same topology, but test multi-path routing
//
TEST_F(ParallelAdjRingTopologyFixture, MultiPathTest) {
  CustomSetUp(true /* multipath */, false /* useKsp2Ed */);
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

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
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12_1, false, 22, labelSwapAction4),
                createNextHopFromAdj(adj12_2, false, 22, labelSwapAction4),
                createNextHopFromAdj(adj12_3, false, 31, labelSwapAction4),
                createNextHopFromAdj(adj13_1, false, 22, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11),
                createNextHopFromAdj(adj12_2, false, 11),
                createNextHopFromAdj(adj12_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11, labelPhpAction),
                createNextHopFromAdj(adj12_2, false, 11, labelPhpAction),
                createNextHopFromAdj(adj12_3, false, 20, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "1", *adjacencyDb1.adjacencies_ref());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21_1, false, 22),
                createNextHopFromAdj(adj21_2, false, 22),
                createNextHopFromAdj(adj21_3, false, 31),
                createNextHopFromAdj(adj24_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
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
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21_1, false, 11, labelPhpAction),
                createNextHopFromAdj(adj21_2, false, 11, labelPhpAction),
                createNextHopFromAdj(adj21_3, false, 20, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "2", *adjacencyDb2.adjacencies_ref());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11),
                createNextHopFromAdj(adj34_2, false, 20),
                createNextHopFromAdj(adj34_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
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
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, labelSwapAction2),
                createNextHopFromAdj(adj34_1, false, 22, labelSwapAction2),
                createNextHopFromAdj(adj34_2, false, 31, labelSwapAction2),
                createNextHopFromAdj(adj34_3, false, 31, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "3", *adjacencyDb3.adjacencies_ref());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11),
                createNextHopFromAdj(adj43_2, false, 20),
                createNextHopFromAdj(adj43_3, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11, labelPhpAction),
                createNextHopFromAdj(adj43_2, false, 20, labelPhpAction),
                createNextHopFromAdj(adj43_3, false, 20, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22),
                createNextHopFromAdj(adj43_1, false, 22),
                createNextHopFromAdj(adj43_2, false, 31),
                createNextHopFromAdj(adj43_3, false, 31)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22, labelSwapAction1),
                createNextHopFromAdj(adj43_1, false, 22, labelSwapAction1),
                createNextHopFromAdj(adj43_2, false, 31, labelSwapAction1),
                createNextHopFromAdj(adj43_3, false, 31, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());
  validateAdjLabelRoutes(routeMap, "4", *adjacencyDb4.adjacencies_ref());
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
  auto routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj12_2, false, 11, std::nullopt),
                createNextHopFromAdj(adj12_3, false, 20, std::nullopt)}));

  auto prefixDBs = prefixState.getPrefixDatabases();
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
      std::make_optional<int64_t>(4));

  prefixDBFour.prefixEntries_ref()->push_back(newPrefix);
  updatePrefixDatabase(prefixState, prefixDBFour);
  routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);

  // in theory, kspf will choose adj12_2, adj13_1 as nexthops,
  // but since we set threhold to be 4, this route will get ignored.
  EXPECT_EQ(routeMap.find(make_pair("1", toString(bgpAddr1))), routeMap.end());

  // updating threshold hold to be 2.
  // becasue we use edge disjoint algorithm, we should expect adj12_2 and
  // adj13_1 as nexthop
  prefixDBFour.prefixEntries_ref()->pop_back();
  apache::thrift::fromFollyOptional(
      newPrefix.minNexthop_ref(), folly::make_optional<int64_t>(2));
  prefixDBFour.prefixEntries_ref()->push_back(newPrefix);
  updatePrefixDatabase(prefixState, prefixDBFour);
  routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(bgpAddr1))],
      NextHops({createNextHopFromAdj(adj12_2, false, 22, push4),
                createNextHopFromAdj(adj13_1, false, 22, push4)}));

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
  prefixDBThr.prefixEntries_ref()->push_back(newPrefix);
  updatePrefixDatabase(prefixState, prefixDBThr);
  routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);

  EXPECT_EQ(routeMap.find(make_pair("1", toString(bgpAddr1))), routeMap.end());

  // Revert the setup to normal state
  prefixDBFour.prefixEntries_ref()->pop_back();
  prefixDBThr.prefixEntries_ref()->pop_back();
  updatePrefixDatabase(prefixState, prefixDBFour);
  updatePrefixDatabase(prefixState, prefixDBThr);

  //
  // Bring down adj12_2 and adj34_2 to make our nexthop validations easy
  // Then validate routing table of all the nodes
  //

  adjacencyDb1.adjacencies_ref()->at(1).isOverloaded_ref() = true;
  adjacencyDb3.adjacencies_ref()->at(2).isOverloaded_ref() = true;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  // Adj label routes => 4 * 3 = 16
  EXPECT_EQ((GetParam() == thrift::PrefixType::BGP ? 56 : 44), routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops({createNextHopFromAdj(adj12_1, false, 22, push4),
                createNextHopFromAdj(adj13_1, false, 22, push4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj12_1, false, 33, push34)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj12_3, false, 20, std::nullopt)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj21_1, false, 33, push43)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj21_1, false, 22, push3),
                createNextHopFromAdj(adj24_1, false, 22, push3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj21_3, false, 20, std::nullopt)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj34_3, false, 20, std::nullopt)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, push2),
                createNextHopFromAdj(adj34_1, false, 22, push2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj34_1, false, 33, push12)}));

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj43_3, false, 20, std::nullopt)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj43_1, false, 33, push21)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops({createNextHopFromAdj(adj42_1, false, 22, push1),
                createNextHopFromAdj(adj43_1, false, 22, push1)}));
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
  auto routeMap = getRouteMap(*spfSolver, {"1"}, areaLinkStates, prefixState);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj12_2, false, 11, std::nullopt),
                createNextHopFromAdj(adj12_3, false, 20, std::nullopt)}));

  //
  // Bring down adj12_2 and adj34_2 to make our nexthop validations easy
  // Then validate routing table of all the nodes
  //

  adjacencyDb1.adjacencies_ref()->at(1).isOverloaded_ref() = true;
  adjacencyDb3.adjacencies_ref()->at(2).isOverloaded_ref() = true;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);

  thrift::MetricVector mv1, mv2;
  int64_t numMetrics = 5;
  mv1.metrics_ref()->resize(numMetrics);
  mv2.metrics_ref()->resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    mv1.metrics_ref()[i].type_ref() = i;
    mv2.metrics_ref()[i].type_ref() = i;
    mv1.metrics_ref()[i].priority_ref() = i;
    mv2.metrics_ref()[i].priority_ref() = i;
    mv1.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    mv2.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    mv1.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    mv2.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    *mv1.metrics_ref()[i].metric_ref() =
        *mv2.metrics_ref()[i].metric_ref() = {i};
  }

  auto prefixDBs = prefixState.getPrefixDatabases();
  auto prefixDBOne = prefixDBs["1"];
  auto prefixDBTwo = prefixDBs["2"];

  // set metric vector for addr1 prefixes in two nodes to be same
  prefixDBOne.prefixEntries_ref()->front().mv_ref() = mv1;
  prefixDBTwo.prefixEntries_ref()->push_back(
      prefixDBOne.prefixEntries_ref()->front());

  updatePrefixDatabase(prefixState, prefixDBOne);
  updatePrefixDatabase(prefixState, prefixDBTwo);

  routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);

  // validate router 3
  EXPECT_EQ(routeMap.find(make_pair("3", toString(addr1))), routeMap.end());

  // decrease mv for the second node, now router 3 should point to 1
  // also set the threshold hold on non-best node to be 4. Threshold
  // on best node is 2. In such case, we should allow the route to be
  // programmed and announced.
  prefixDBTwo.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .metric_ref()
      ->front()--;
  prefixDBTwo.prefixEntries_ref()->back().minNexthop_ref() = 4;
  prefixDBOne.prefixEntries_ref()->front().minNexthop_ref() = 2;
  updatePrefixDatabase(prefixState, prefixDBTwo);
  updatePrefixDatabase(prefixState, prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, std::nullopt),
                createNextHopFromAdj(adj34_1, false, 33, push12)}));

  // node 1 is the preferred node. Set threshold on Node 1 to be 4.
  // threshold on  node 2 is 2. In such case, threshold should be respected
  // and we should not program/annouce any routes
  prefixDBTwo.prefixEntries_ref()->back().minNexthop_ref() = 2;
  prefixDBOne.prefixEntries_ref()->front().minNexthop_ref() = 4;
  updatePrefixDatabase(prefixState, prefixDBTwo);
  updatePrefixDatabase(prefixState, prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);

  // validate router 3
  EXPECT_EQ(routeMap.find(make_pair("3", toString(addr1))), routeMap.end());
  // reset min nexthop to rest of checks
  prefixDBTwo.prefixEntries_ref()->back().minNexthop_ref().reset();
  prefixDBOne.prefixEntries_ref()->front().minNexthop_ref().reset();
  updatePrefixDatabase(prefixState, prefixDBTwo);
  updatePrefixDatabase(prefixState, prefixDBOne);

  // decrease mv for the second node, now router 3 should point to 2
  prefixDBTwo.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .metric_ref()
      ->front() += 2;
  updatePrefixDatabase(prefixState, prefixDBTwo);
  routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, push2),
                createNextHopFromAdj(adj34_1, false, 22, push2)}));

  // set the tie breaker to be true. in this case, both nodes will be selected
  prefixDBTwo.prefixEntries_ref()
      ->back()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .isBestPathTieBreaker_ref() = true;

  prefixDBOne.prefixEntries_ref()
      ->front()
      .mv_ref()
      .value()
      .metrics_ref()
      ->at(numMetrics - 1)
      .isBestPathTieBreaker_ref() = true;
  updatePrefixDatabase(prefixState, prefixDBTwo);
  updatePrefixDatabase(prefixState, prefixDBOne);
  routeMap = getRouteMap(*spfSolver, {"3"}, areaLinkStates, prefixState);

  // createNextHopFromAdj(adj34, v4Enabled, 30, push12, true) getting ignored
  // because in kspf, we will ignore second shortest path if it starts with
  // one of shortest path for anycast prefix
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 22, push2),
                createNextHopFromAdj(adj34_1, false, 22, push2),
                createNextHopFromAdj(adj31_1, false, 11, std::nullopt)}));
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

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

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
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb4).topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb5).topologyChanged);

  // Prefix db's
  const auto defaultPrefixV6 = toIpPrefix("::/0");
  const auto prefixDb1_ = createPrefixDb(
      "1",
      {createPrefixEntry(
          addr1,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1_).empty());
  const auto prefixDb2_ = createPrefixDb(
      "2",
      {createPrefixEntry(
          addr2,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2_).empty());
  const auto prefixDb3_ = createPrefixDb(
      "3",
      {createPrefixEntry(
          addr3,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3_).empty());
  const auto prefixDb4_ = createPrefixDb(
      "4",
      {createPrefixEntry(
          defaultPrefixV6,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb4_).empty());
  const auto prefixDb5_ = createPrefixDb(
      "5",
      {createPrefixEntry(
          defaultPrefixV6,
          thrift::PrefixType::LOOPBACK,
          {},
          thrift::PrefixForwardingType::SR_MPLS)});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb5_).empty());

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
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4", "5"}, areaLinkStates, prefixState);

  // Unicast routes => 15 (5 * 3)
  // Node label routes => 5 * 5 = 25
  // Adj label routes => 0
  EXPECT_EQ(40, routeMap.size());

  // Validate router-1

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel_ref());

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_2, false, 10, std::nullopt),
                createNextHopFromAdj(adj12_1, false, 10, std::nullopt)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13, false, 10, std::nullopt)}));

  EXPECT_EQ(
      routeMap[make_pair("1", "::/0")],
      NextHops({createNextHopFromAdj(adj13, false, 30, labelPush4),
                createNextHopFromAdj(adj13, false, 20, labelPush5),
                createNextHopFromAdj(adj12_2, false, 20, labelPush4),
                createNextHopFromAdj(adj12_2, false, 20, labelPush5),
                createNextHopFromAdj(adj12_1, false, 20, labelPush4),
                createNextHopFromAdj(adj12_1, false, 20, labelPush5)}));

  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12_1, false, 10, labelPhpAction),
                createNextHopFromAdj(adj12_2, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12_1, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj12_2, false, 20, labelSwapAction4),
                createNextHopFromAdj(adj13, false, 30, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb5.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj12_1, false, 20, labelSwapAction5),
                createNextHopFromAdj(adj12_2, false, 20, labelSwapAction5),
                createNextHopFromAdj(adj13, false, 20, labelSwapAction5)}));

  // Validate router-2
  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel_ref());

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
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21_1, false, 10, labelPhpAction),
                createNextHopFromAdj(adj21_2, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj21_1, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj21_2, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj25, false, 20, labelSwapAction3),
                createNextHopFromAdj(adj24, false, 30, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb5.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj25, false, 10, labelPhpAction)}));

  // Validate router-3
  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel_ref());

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
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction),
                createNextHopFromAdj(adj34, false, 40, labelSwapAction1)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj35, false, 20, labelSwapAction2),
                createNextHopFromAdj(adj34, false, 30, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj34, false, 20, labelPhpAction),
                createNextHopFromAdj(adj31, false, 30, labelSwapAction4),
                createNextHopFromAdj(adj35, false, 30, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb5.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj35, false, 10, labelPhpAction),
                createNextHopFromAdj(adj34, false, 40, labelSwapAction5)}));

  // Validate router-4

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel_ref());

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
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj43, false, 30, labelSwapAction1)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction),
                createNextHopFromAdj(adj43, false, 40, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj43, false, 20, labelPhpAction),
                createNextHopFromAdj(adj42, false, 30, labelSwapAction3)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb5.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction5),
                createNextHopFromAdj(adj43, false, 30, labelSwapAction5)}));

  // Validate router-5
  validatePopLabelRoute(routeMap, "5", *adjacencyDb5.nodeLabel_ref());

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
      routeMap[make_pair("5", std::to_string(*adjacencyDb1.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj52, false, 20, labelSwapAction1),
                createNextHopFromAdj(adj53, false, 20, labelSwapAction1)}));
  EXPECT_EQ(
      routeMap[make_pair("5", std::to_string(*adjacencyDb2.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj52, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("5", std::to_string(*adjacencyDb3.nodeLabel_ref()))],
      NextHops({createNextHopFromAdj(adj53, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("5", std::to_string(*adjacencyDb4.nodeLabel_ref()))],
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
createGrid(LinkState& linkState, PrefixState& prefixState, int n) {
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
      linkState.updateAdjacencyDatabase(adjacencyDb);

      // prefix
      auto addrV6 = toIpPrefix(nodeToPrefixV6(node));
      updatePrefixDatabase(
          prefixState, createPrefixDb(nodeName, {createPrefixEntry(addrV6)}));
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
    areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
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

  return abs(x_a - x_b) + abs(y_a - y_b);
}

TEST_P(GridTopologyFixture, ShortestPathTest) {
  vector<string> allNodes;
  for (int i = 0; i < n * n; ++i) {
    allNodes.push_back(folly::sformat("{}", i));
  }

  auto routeMap = getRouteMap(spfSolver, allNodes, areaLinkStates, prefixState);

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
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric_ref());

  // secondary diagnal
  src = n - 1;
  dst = n * (n - 1);
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric_ref());

  // 2) from origin (i.e., node 0) to random inner node
  src = 0;
  dst = folly::Random::rand32() % (n * n - 1) + 1;
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric_ref());

  // 3) from one random node to another
  src = folly::Random::rand32() % (n * n);
  while ((dst = folly::Random::rand32() % (n * n)) == src) {
  }
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops =
      routeMap[make_pair(folly::sformat("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric_ref());
}

// measure SPF execution time for large networks
TEST(GridTopology, StressTest) {
  if (!FLAGS_stress_test) {
    return;
  }
  std::string nodeName("1");
  SpfSolver spfSolver(nodeName, false, true);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(kTestingAreaName, LinkState(kTestingAreaName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  createGrid(linkState, prefixState, 99);
  spfSolver.buildRouteDb("523", areaLinkStates, prefixState);
}

//
// Start the decision thread and simulate KvStore communications
// Expect proper RouteDatabase publications to appear
//
class DecisionTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    // reset all global counters
    fb303::fbData->resetAllData();

    auto tConfig = createConfig();
    config = std::make_shared<Config>(tConfig);

    decision = make_shared<Decision>(
        config,
        true, /* computeLfaPaths */
        false, /* bgpDryRun */
        debounceTimeoutMin,
        debounceTimeoutMax,
        kvStoreUpdatesQueue.getReader(),
        staticRoutesUpdateQueue.getReader(),
        routeUpdatesQueue);

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

  virtual openr::thrift::OpenrConfig
  createConfig() {
    auto tConfig = getBasicOpenrConfig("1");
    // set coldstart to be longer than debounce time
    tConfig.eor_time_s_ref() = ((debounceTimeoutMax.count() * 2) / 1000);
    return tConfig;
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
      EXPECT_EQ(node, *resp->thisNodeName_ref());
      routeMap[node] = std::move(*resp);
    }

    return routeMap;
  }

  DecisionRouteUpdate
  recvRouteUpdates() {
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
    adjDB.isOverloaded_ref() = overloaded;
    return thrift::Value(
        FRAGILE,
        version,
        "originator-1",
        writeThriftObjStr(adjDB, serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  thrift::Value
  createPrefixValue(
      const string& node,
      int64_t version,
      thrift::PrefixDatabase const& prefixDb) {
    return createThriftValue(
        version,
        node,
        writeThriftObjStr(prefixDb, serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  thrift::Value
  createPrefixValue(
      const string& node,
      int64_t version,
      const vector<thrift::IpPrefix>& prefixes = {},
      const string& area = kTestingAreaName) {
    vector<thrift::PrefixEntry> prefixEntries;
    for (const auto& prefix : prefixes) {
      prefixEntries.emplace_back(createPrefixEntry(prefix));
    }
    return createPrefixValue(
        node, version, createPrefixDb(node, prefixEntries, area));
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
          kTestingAreaName);
      keyVal[prefixKey.getPrefixKey()] = createThriftValue(
          version,
          node,
          writeThriftObjStr(
              createPrefixDb(node, {createPrefixEntry(prefix)}), serializer),
          Constants::kTtlInfinity /* ttl */,
          0 /* ttl version */,
          0 /* hash */);
    }
    return keyVal;
  }

  /**
   * Check whether two DecisionRouteUpdates to be equal
   */
  bool
  checkEqualRoutesDelta(
      DecisionRouteUpdate& lhsC, thrift::RouteDatabaseDelta& rhs) {
    auto lhs = lhsC.toThrift();
    std::sort(
        lhs.unicastRoutesToUpdate_ref()->begin(),
        lhs.unicastRoutesToUpdate_ref()->end());
    std::sort(
        rhs.unicastRoutesToUpdate_ref()->begin(),
        rhs.unicastRoutesToUpdate_ref()->end());

    std::sort(
        lhs.unicastRoutesToDelete_ref()->begin(),
        lhs.unicastRoutesToDelete_ref()->end());
    std::sort(
        rhs.unicastRoutesToDelete_ref()->begin(),
        rhs.unicastRoutesToDelete_ref()->end());

    return *lhs.unicastRoutesToUpdate_ref() ==
        *rhs.unicastRoutesToUpdate_ref() &&
        *lhs.unicastRoutesToDelete_ref() == *rhs.unicastRoutesToDelete_ref();
  }

  //
  // member variables
  //

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  CompactSerializer serializer{};

  std::shared_ptr<Config> config;
  messaging::ReplicateQueue<thrift::Publication> kvStoreUpdatesQueue;
  messaging::ReplicateQueue<thrift::RouteDatabaseDelta> staticRoutesUpdateQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueueReader{
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
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {},
      std::string(""));
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  // self mpls route, node 2 mpls route and adj12 label route
  EXPECT_EQ(3, routeDbDelta.mplsRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());

  auto routeDb = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDb.unicastRoutes_ref()->begin(), routeDb.unicastRoutes_ref()->end());
  std::sort(routeDb.mplsRoutes_ref()->begin(), routeDb.mplsRoutes_ref()->end());

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
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {},
      std::string(""));
  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes_ref()->begin(),
      routeDbBefore.unicastRoutes_ref()->end());
  std::sort(
      routeDbBefore.mplsRoutes_ref()->begin(),
      routeDbBefore.mplsRoutes_ref()->end());
  sendKvPublication(publication);
  // validate routers

  // receive my local Decision routeDbDelta publication
  routeDbDelta = recvRouteUpdates();
  // only expect to add a route to addr3
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      routeDbDelta.unicastRoutesToUpdate.begin()->second.prefix,
      toIPNetwork(addr3));
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());

  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDb.unicastRoutes_ref()->begin(), routeDb.unicastRoutes_ref()->end());
  std::sort(routeDb.mplsRoutes_ref()->begin(), routeDb.mplsRoutes_ref()->end());
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
  EXPECT_EQ(2, routeDbMap["2"].unicastRoutes_ref()->size());
  EXPECT_EQ(2, routeDbMap["3"].unicastRoutes_ref()->size());
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
      routeDbBefore.unicastRoutes_ref()->begin(),
      routeDbBefore.unicastRoutes_ref()->end());
  std::sort(
      routeDbBefore.mplsRoutes_ref()->begin(),
      routeDbBefore.mplsRoutes_ref()->end());

  sendKvPublication(publication);
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToDelete.size());
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToDelete.size());
  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDb.unicastRoutes_ref()->begin(), routeDb.unicastRoutes_ref()->end());
  std::sort(routeDb.mplsRoutes_ref()->begin(), routeDb.mplsRoutes_ref()->end());

  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  publication = createThriftPublication(
      {{"adj:3", createAdjValue("3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue("2", 4, {adj21, adj23}, false, 2)},
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {},
      std::string(""));
  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes_ref()->begin(),
      routeDbBefore.unicastRoutes_ref()->end());
  std::sort(
      routeDbBefore.mplsRoutes_ref()->begin(),
      routeDbBefore.mplsRoutes_ref()->end());
  sendKvPublication(publication);
  // validate routers

  // receive my local Decision routeDbDelta publication
  routeDbDelta = recvRouteUpdates();
  // only expect to add a route to addr3
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      routeDbDelta.unicastRoutesToUpdate.begin()->second.prefix,
      toIPNetwork(addr3));
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.size());

  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDb.unicastRoutes_ref()->begin(), routeDb.unicastRoutes_ref()->end());
  std::sort(routeDb.mplsRoutes_ref()->begin(), routeDb.mplsRoutes_ref()->end());
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));

  // construct new static mpls route add
  thrift::RouteDatabaseDelta input;
  thrift::NextHopThrift nh, nh1;
  *nh.address_ref() = toBinaryAddress(folly::IPAddressV6("::1"));
  *nh1.address_ref() = toBinaryAddress(folly::IPAddressV6("::2"));
  nh.mplsAction_ref() =
      createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
  nh1.mplsAction_ref() =
      createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
  thrift::MplsRoute route;
  route.topLabel_ref() = 32011;
  route.nextHops_ref() = {nh};
  input.mplsRoutesToUpdate_ref()->push_back(route);

  // Update 32011 and make sure only that is updated
  sendStaticRoutesUpdate(input);
  auto routesDelta = routeUpdatesQueueReader.get().value();
  routesDelta.perfEvents.reset();
  EXPECT_EQ(routesDelta.toThrift(), input);

  // Update 32012 and make sure only that is updated
  route.topLabel_ref() = 32012;
  input.mplsRoutesToUpdate_ref() = {route};
  sendStaticRoutesUpdate(input);
  routesDelta = routeUpdatesQueueReader.get().value();
  routesDelta.perfEvents.reset();
  EXPECT_EQ(routesDelta.toThrift(), input);

  auto staticRoutes = decision->getMplsStaticRoutes().get();
  EXPECT_EQ(staticRoutes.size(), 2);
  EXPECT_THAT(staticRoutes[32012], testing::UnorderedElementsAreArray({nh}));
  EXPECT_THAT(staticRoutes[32011], testing::UnorderedElementsAreArray({nh}));

  // Test our consolidating logic, we first update 32011 then delete 32011
  // making sure only delete for 32011 is emitted.
  route.topLabel_ref() = 32011;
  route.nextHops_ref() = {nh, nh1};
  input.mplsRoutesToUpdate_ref() = {route};
  input.mplsRoutesToDelete_ref()->clear();
  sendStaticRoutesUpdate(input);

  input.mplsRoutesToUpdate_ref()->clear();
  input.mplsRoutesToDelete_ref() = {32011};
  sendStaticRoutesUpdate(input);

  routesDelta = routeUpdatesQueueReader.get().value();
  routesDelta.perfEvents.reset();
  EXPECT_EQ(routesDelta.mplsRoutesToDelete.at(0), 32011);
  EXPECT_EQ(routesDelta.mplsRoutesToUpdate.size(), 0);

  staticRoutes = decision->getMplsStaticRoutes().get();
  EXPECT_EQ(staticRoutes.size(), 1);
  EXPECT_THAT(staticRoutes[32012], testing::UnorderedElementsAreArray({nh}));

  // test our consolidating logic, we first delete 32012 then update 32012
  // making sure only update for 32012 is emitted.
  input.mplsRoutesToUpdate_ref()->clear();
  input.mplsRoutesToDelete_ref() = {32012};
  sendStaticRoutesUpdate(input);

  route.topLabel_ref() = 32012;
  route.nextHops_ref() = {nh, nh1};
  input.mplsRoutesToUpdate_ref() = {route};
  input.mplsRoutesToDelete_ref()->clear();
  sendStaticRoutesUpdate(input);

  routesDelta = routeUpdatesQueueReader.get().value();
  routesDelta.perfEvents.reset();
  EXPECT_EQ(1, routesDelta.mplsRoutesToUpdate.size());
  EXPECT_EQ(32012, routesDelta.mplsRoutesToUpdate.at(0).label);
  EXPECT_THAT(
      routesDelta.mplsRoutesToUpdate.at(0).nexthops,
      testing::UnorderedElementsAre(nh, nh1));

  staticRoutes = decision->getMplsStaticRoutes().get();
  EXPECT_EQ(staticRoutes.size(), 1);
  EXPECT_THAT(
      staticRoutes[32012], testing::UnorderedElementsAreArray({nh, nh1}));

  routeDb = dumpRouteDb({"1"})["1"];
  bool foundLabelRoute{false};
  for (auto const& mplsRoute : *routeDb.mplsRoutes_ref()) {
    if (*mplsRoute.topLabel_ref() != 32012) {
      continue;
    }

    EXPECT_THAT(
        *mplsRoute.nextHops_ref(),
        testing::UnorderedElementsAreArray({nh, nh1}));
    foundLabelRoute = true;
    break;
  }
  EXPECT_TRUE(foundLabelRoute);
}

/**
 * Publish all types of update to Decision and expect that Decision emits
 * a full route database that includes all the routes as its first update.
 *
 * Types of information updated
 * - Adjacencies (with MPLS labels)
 * - Prefixes
 * - MPLS Static routes
 */
TEST_F(DecisionTestFixture, InitialRouteUpdate) {
  // Send adj publication
  sendKvPublication(createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue("2", 1, {adj21}, false, 2)}},
      {},
      {},
      {},
      std::string("")));

  // Send prefix publication
  sendKvPublication(createThriftPublication(
      {createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {},
      std::string("")));

  // Send static MPLS routes
  thrift::RouteDatabaseDelta staticRoutes;
  {
    thrift::NextHopThrift nh;
    nh.address_ref() = toBinaryAddress(folly::IPAddressV6("::1"));
    nh.mplsAction_ref() =
        createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
    thrift::MplsRoute mplsRoute;
    mplsRoute.topLabel_ref() = 32011;
    mplsRoute.nextHops_ref() = {nh};
    staticRoutes.mplsRoutesToUpdate_ref()->push_back(mplsRoute);
  }
  sendStaticRoutesUpdate(staticRoutes);

  //
  // Receive & verify all the expected updates
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  // self mpls route, node 2 mpls route, adj12 label route, static MPLS route
  EXPECT_EQ(4, routeDbDelta.mplsRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());
}

// The following topology is used:
//  1--- A ---2
//  |         |
//  B         A
//  |         |
//  3--- B ---4
//
// area A: adj12, adj24
// area B: adj13, adj34
TEST_F(DecisionTestFixture, MultiAreaBestPathCalculation) {
  //
  // publish area A adj and prefix
  // "1" originate addr1 into A
  // "2" originate addr2 into A
  //
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue("2", 1, {adj21, adj24}, false, 2)},
       {"adj:4", createAdjValue("4", 1, {adj42}, false, 4)},
       createPrefixKeyValue("1", 1, addr1, "A"),
       createPrefixKeyValue("2", 1, addr2, "A")},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      std::string(""), /*floodRootId */
      "A");

  sendKvPublication(publication);
  recvRouteUpdates();

  //
  // publish area B adj and prefix
  // "3" originate addr3 into B
  // "4" originate addr4 into B
  //
  publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj13}, false, 1)},
       {"adj:3", createAdjValue("3", 1, {adj31, adj34}, false, 3)},
       {"adj:4", createAdjValue("4", 1, {adj43}, false, 4)},
       createPrefixKeyValue("3", 1, addr3, "B"),
       createPrefixKeyValue("4", 1, addr4, "B")},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      std::string(""), /*floodRootId */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  auto routeDb1 = dumpRouteDb({"1"})["1"];
  auto routeDb2 = dumpRouteDb({"2"})["2"];
  auto routeDb3 = dumpRouteDb({"3"})["3"];
  auto routeDb4 = dumpRouteDb({"4"})["4"];

  // routeDb1 from node "1"
  {
    auto routeToAddr2 = createUnicastRoute(
        addr2, {createNextHopFromAdj(adj12, false, 10, std::nullopt, "A")});
    auto routeToAddr3 = createUnicastRoute(
        addr3, {createNextHopFromAdj(adj13, false, 10, std::nullopt, "B")});
    // addr4 is only originated in area B
    auto routeToAddr4 = createUnicastRoute(
        addr4,
        {createNextHopFromAdj(adj12, false, 20, std::nullopt, "A"),
         createNextHopFromAdj(adj13, false, 20, std::nullopt, "B")});
    EXPECT_THAT(*routeDb1.unicastRoutes_ref(), testing::SizeIs(3));
    EXPECT_THAT(
        *routeDb1.unicastRoutes_ref(),
        testing::UnorderedElementsAre(
            routeToAddr2, routeToAddr3, routeToAddr4));
  }

  // routeDb2 from node "2" will only see addr1 in area A
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1, {createNextHopFromAdj(adj21, false, 10, std::nullopt, "A")});
    EXPECT_THAT(*routeDb2.unicastRoutes_ref(), testing::SizeIs(1));
    EXPECT_THAT(
        *routeDb2.unicastRoutes_ref(),
        testing::UnorderedElementsAre(routeToAddr1));
  }

  // routeDb3 will only see addr4 in area B
  {
    auto routeToAddr4 = createUnicastRoute(
        addr4, {createNextHopFromAdj(adj34, false, 10, std::nullopt, "B")});
    EXPECT_THAT(*routeDb3.unicastRoutes_ref(), testing::SizeIs(1));
    EXPECT_THAT(
        *routeDb3.unicastRoutes_ref(),
        testing::UnorderedElementsAre(routeToAddr4));
  }

  // routeDb4
  {
    auto routeToAddr2 = createUnicastRoute(
        addr2, {createNextHopFromAdj(adj42, false, 10, std::nullopt, "A")});
    auto routeToAddr3 = createUnicastRoute(
        addr3, {createNextHopFromAdj(adj43, false, 10, std::nullopt, "B")});
    // addr1 is only originated in area A
    auto routeToAddr1 = createUnicastRoute(
        addr1,
        {createNextHopFromAdj(adj42, false, 20, std::nullopt, "A"),
         createNextHopFromAdj(adj43, false, 20, std::nullopt, "B")});
    EXPECT_THAT(*routeDb4.unicastRoutes_ref(), testing::SizeIs(3));
    EXPECT_THAT(
        *routeDb4.unicastRoutes_ref(),
        testing::UnorderedElementsAre(
            routeToAddr2, routeToAddr3, routeToAddr1));
  }

  //
  // "1" originate addr1 into B
  //
  publication = createThriftPublication(
      {createPrefixKeyValue("1", 1, addr1, "B")},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      std::string(""), /*floodRootId */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  routeDb3 = dumpRouteDb({"3"})["3"];
  routeDb4 = dumpRouteDb({"4"})["4"];

  // routeMap3 now should see addr1 in areaB
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1, {createNextHopFromAdj(adj31, false, 10, std::nullopt, "B")});
    EXPECT_THAT(*routeDb3.unicastRoutes_ref(), testing::Contains(routeToAddr1));
  }

  // routeMap4 now could reach addr1 through areaA or areaB
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1,
        {createNextHopFromAdj(adj43, false, 20, std::nullopt, "B"),
         createNextHopFromAdj(adj42, false, 20, std::nullopt, "A")});
    EXPECT_THAT(*routeDb4.unicastRoutes_ref(), testing::Contains(routeToAddr1));
  }
}

// MultiArea Tology topology is used:
//  1--- A ---2
//  |
//  B
//  |
//  3
//
// area A: adj12
// area B: adj13
TEST_F(DecisionTestFixture, SelfReditributePrefixPublication) {
  //
  // publish area A adj and prefix
  // "2" originate addr2 into A
  //
  auto originKeyStr = PrefixKey("2", toIPNetwork(addr2), "A").getPrefixKey();
  auto originPfx = createPrefixEntry(addr2);
  originPfx.area_stack_ref() = {"65000"};
  auto originPfxVal =
      createPrefixValue("2", 1, createPrefixDb("2", {originPfx}, "A"));

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue("2", 1, {adj21}, false, 2)},
       {originKeyStr, originPfxVal}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      std::string(""), /*floodRootId */
      "A");

  sendKvPublication(publication);
  recvRouteUpdates();
  auto expectedPrefixDbs = *decision->getDecisionPrefixDbs().get();

  //
  // publish area B adj and prefix
  //
  publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj13}, false, 1)},
       {"adj:3", createAdjValue("3", 1, {adj31}, false, 3)}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      std::string(""), /*floodRootId */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  //
  // "1" reditribute addr2 into B
  //   - this should not cause prefix db update
  //   - not route update
  //
  auto redistributeKeyStr =
      PrefixKey("1", toIPNetwork(addr2), "B").getPrefixKey();
  auto redistributePfx = createPrefixEntry(addr2, thrift::PrefixType::RIB);
  redistributePfx.area_stack_ref() = {"65000", "A"};
  auto redistributePfxVal =
      createPrefixValue("1", 1, createPrefixDb("1", {redistributePfx}, "B"));

  publication = createThriftPublication(
      {{redistributeKeyStr, redistributePfxVal}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      std::string(""), /*floodRootId */
      "B");
  sendKvPublication(publication);

  // wait for publication to be processed
  /* sleep override */
  std::this_thread::sleep_for(
      debounceTimeoutMax + std::chrono::milliseconds(100));

  auto prefixDbs = *decision->getDecisionPrefixDbs().get();
  EXPECT_EQ(expectedPrefixDbs, prefixDbs);
  EXPECT_EQ(0, routeUpdatesQueueReader.size());
}

/**
 * Exhaustively RibPolicy feature in Decision. The intention here is to
 * verify the functionality of RibPolicy in Decision module. RibPolicy
 * is also unit-tested for it's complete correctness and we don't aim
 * it here.
 *
 * Test covers
 * - Get policy without setting (exception case)
 * - Set policy
 * - Get policy after setting
 * - Verify that set-policy triggers the route database change (apply policy)
 * - Set the policy with 0 weight. See that route dis-appears
 * - Expire policy. Verify it triggers the route database change (undo policy)
 */
TEST_F(DecisionTestFixture, RibPolicy) {
  // Setup topology and prefixes. 1 unicast route will be computed
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue("2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);

  // Expect route update. Verify next-hop weight to be 0 (ECMP)
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    EXPECT_EQ(
        0,
        *updates.unicastRoutesToUpdate.begin()
             ->second.nexthops.begin()
             ->weight_ref());
  }

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight_ref()->emplace("2", 2);
  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher_ref()->prefixes_ref() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action_ref()->set_weight_ref() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements_ref()->emplace_back(policyStatement);
  policy.ttl_secs_ref() = 1;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  {
    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_EQ(*policy.statements_ref(), *retrievedPolicy.statements_ref());
    EXPECT_GE(*policy.ttl_secs_ref(), *retrievedPolicy.ttl_secs_ref());
  }

  // Expect the route database change with next-hop weight to be 2
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    EXPECT_EQ(
        2,
        *updates.unicastRoutesToUpdate.begin()
             ->second.nexthops.begin()
             ->weight_ref());
  }

  // Set the policy with empty weight. Expect route remains intact and error
  // counter is reported
  policy.statements_ref()
      ->at(0)
      .action_ref()
      ->set_weight_ref()
      ->neighbor_to_weight_ref()["2"] = 0;
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());
  {
    auto updates = recvRouteUpdates();
    EXPECT_EQ(1, updates.unicastRoutesToUpdate.size());
    ASSERT_EQ(0, updates.unicastRoutesToDelete.size());
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.count(toIPNetwork(addr2)));
    for (auto& nh :
         updates.unicastRoutesToUpdate.at(toIPNetwork(addr2)).nexthops) {
      EXPECT_FALSE(nh.weight_ref().has_value());
    }
    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("decision.rib_policy.invalidated_routes.count"));
  }

  // trigger addr2 recalc by flapping the advertisement
  publication = createThriftPublication(
      {createPrefixKeyValue(
          "2", 2, addr2, kTestingAreaName, true /* withdraw */)},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);
  publication = createThriftPublication(
      {createPrefixKeyValue("2", 3, addr2)}, {}, {}, {}, std::string(""));
  sendKvPublication(publication);

  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    ASSERT_EQ(0, updates.unicastRoutesToDelete.size());
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.count(toIPNetwork(addr2)));
    for (auto& nh :
         updates.unicastRoutesToUpdate.at(toIPNetwork(addr2)).nexthops) {
      EXPECT_FALSE(nh.weight_ref().has_value());
    }
    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(2, counters.at("decision.rib_policy.invalidated_routes.count"));
  }

  // Let the policy expire. Wait for another route database change
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(0, updates.unicastRoutesToUpdate.size());

    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_GE(0, *retrievedPolicy.ttl_secs_ref());
  }
}

/**
 * Verifies that error is set if RibPolicy is invalid
 */
TEST_F(DecisionTestFixture, RibPolicyError) {
  // Set empty rib policy
  auto sf = decision->setRibPolicy(thrift::RibPolicy{});

  // Expect an error to be set immediately (validation happens inline)
  EXPECT_TRUE(sf.isReady());
  EXPECT_TRUE(sf.hasException());
  EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
}

/**
 * Verifies that set/get APIs throws exception if RibPolicy feature is not
 * enabled.
 */
TEST(Decision, RibPolicyFeatureKnob) {
  auto tConfig = getBasicOpenrConfig("1");
  tConfig.enable_rib_policy_ref() = false; // Disable rib_policy feature
  auto config = std::make_shared<Config>(tConfig);
  ASSERT_FALSE(config->isRibPolicyEnabled());

  messaging::ReplicateQueue<thrift::Publication> kvStoreUpdatesQueue;
  messaging::ReplicateQueue<thrift::RouteDatabaseDelta> staticRoutesUpdateQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  auto decision = std::make_unique<Decision>(
      config,
      true, /* computeLfaPaths */
      false, /* bgpDryRun */
      debounceTimeoutMin,
      debounceTimeoutMax,
      kvStoreUpdatesQueue.getReader(),
      staticRoutesUpdateQueue.getReader(),
      routeUpdatesQueue);

  // SET
  {
    // Create valid rib policy
    thrift::RibRouteActionWeight actionWeight;
    actionWeight.neighbor_to_weight_ref()->emplace("2", 2);
    thrift::RibPolicyStatement policyStatement;
    policyStatement.matcher_ref()->prefixes_ref() =
        std::vector<thrift::IpPrefix>({addr2});
    policyStatement.action_ref()->set_weight_ref() = actionWeight;
    thrift::RibPolicy policy;
    policy.statements_ref()->emplace_back(policyStatement);
    policy.ttl_secs_ref() = 1;

    auto sf = decision->setRibPolicy(policy);
    EXPECT_TRUE(sf.isReady());
    EXPECT_TRUE(sf.hasException());
    EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
  }

  // GET
  {
    auto sf = decision->getRibPolicy();
    EXPECT_TRUE(sf.isReady());
    EXPECT_TRUE(sf.hasException());
    EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
  }

  kvStoreUpdatesQueue.close();
  staticRoutesUpdateQueue.close();
  routeUpdatesQueue.close();
}

// The following topology is used:
//
//         100
//  1--- ---------- 2
//   \_           _/
//      \_ ____ _/
//          800

// We upload parallel link 1---2 with the initial sync and later bring down
// the one with lower metric. We then verify updated route database is
// received
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
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {},
      std::string(""));
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();
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
  routeDbDelta = recvRouteUpdates();
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
  routeDbDelta = recvRouteUpdates();
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
  adj21_1_overloaded.isOverloaded_ref() = true;

  publication = createThriftPublication(
      {{"adj:2", createAdjValue("2", 2, {adj21_1_overloaded, adj21_2})}},
      {},
      {},
      {},
      std::string(""));
  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvRouteUpdates();
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

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {},
      std::string(""));

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);
  EXPECT_EQ(0, counters["decision.route_build_runs.count"]);

  sendKvPublication(publication);
  recvRouteUpdates();

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
       createPrefixKeyValue("3", 1, addr3)},
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
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(4, counters["decision.spf_runs.count"]);
  EXPECT_EQ(2, counters["decision.route_build_runs.count"]);

  //
  // Only publish prefix updates
  //
  auto getRouteForPrefixCount =
      counters.at("decision.get_route_for_prefix.count");
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 1, addr4)}, {}, {}, {}, std::string(""));
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(4, counters["decision.spf_runs.count"]);
  // only prefix changed no full rebuild needed
  EXPECT_EQ(2, counters["decision.route_build_runs.count"]);

  EXPECT_EQ(
      getRouteForPrefixCount + 1,
      counters["decision.get_route_for_prefix.count"]);

  //
  // publish adj updates right after prefix updates
  // Decision is supposed to only trigger spf recalculation

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 2, addr4),
       createPrefixKeyValue("4", 2, addr5)},
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
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(6, counters["decision.spf_runs.count"]);
  EXPECT_EQ(3, counters["decision.route_build_runs.count"]);

  //
  // publish multiple prefix updates in a row
  // Decision is supposed to process prefix update only once

  // Some tricks here; we need to bump the version on router 4's data, so
  // it can override existing;

  getRouteForPrefixCount = counters.at("decision.get_route_for_prefix.count");
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 5, addr4)}, {}, {}, {}, std::string(""));
  sendKvPublication(publication);

  publication = createThriftPublication(
      {createPrefixKeyValue("4", 7, addr4),
       createPrefixKeyValue("4", 7, addr6)},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);

  publication = createThriftPublication(
      {createPrefixKeyValue("4", 8, addr4),
       createPrefixKeyValue("4", 8, addr5),
       createPrefixKeyValue("4", 8, addr6)},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  // only prefix has changed so spf_runs is unchanged
  EXPECT_EQ(6, counters["decision.spf_runs.count"]);
  // addr6 is seen to have been advertised in this  interval
  EXPECT_EQ(
      getRouteForPrefixCount + 1,
      counters["decision.get_route_for_prefix.count"]);
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
  auto publication = createThriftPublication(
      {{"adj2:1", createAdjValue("1", 1, {adj12})},
       {"adji2:2", createAdjValue("2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {},
      std::string(""));

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

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
  auto const publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {},
      std::string(""));

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

  // make sure counter is incremented
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters["decision.spf_runs.count"]);

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

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
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2),
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {},
      std::string(""));

  sendKvPublication(publication);
  recvRouteUpdates();

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
  adj12.metric_ref() = 100;
  adj21.metric_ref() = 100;
  publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 2, {adj12, adj13})},
       {"adj:2", createAdjValue("2", 2, {adj21, adj23})}},
      {},
      {},
      {},
      std::string(""));

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvRouteUpdates();

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
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2),
       // node3 has same address w/ node2
       createPrefixKeyValue("3", 1, addr2),
       createPrefixKeyValue("4", 1, addr4)},
      {},
      {},
      {},
      std::string(""));

  sendKvPublication(publication);
  recvRouteUpdates();

  // Expect best route selection to be populated in route-details for addr2
  {
    thrift::ReceivedRouteFilter filter;
    filter.prefixes_ref() = std::vector<thrift::IpPrefix>({addr2});
    auto routes = decision->getReceivedRoutesFiltered(filter).get();
    ASSERT_EQ(1, routes->size());

    auto const& routeDetails = routes->at(0);
    EXPECT_EQ(2, routeDetails.bestKeys_ref()->size());
    EXPECT_EQ("2", routeDetails.bestKey_ref()->node_ref().value());
  }

  // Query new information
  // validate routers
  auto routeMapList = dumpRouteDb({"1", "2", "3", "4"});
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  RouteMap routeMap;
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
  }

  // 1
  EXPECT_EQ(2, routeMapList["1"].unicastRoutes_ref()->size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10),
                createNextHopFromAdj(adj13, false, 10)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].unicastRoutes_ref()->size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].unicastRoutes_ref()->size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].unicastRoutes_ref()->size());
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
  recvRouteUpdates();

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
  adj12.metric_ref() = 100;
  adj21.metric_ref() = 100;

  publication = createThriftPublication(
      {{"adj:1", createAdjValue("1", 2, {adj12, adj13, adj14})},
       {"adj:2", createAdjValue("2", 2, {adj21, adj23})}},
      {},
      {},
      {},
      std::string(""));

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvRouteUpdates();

  // Query new information
  // validate routers
  routeMapList = dumpRouteDb({"1", "2", "3", "4"});
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  routeMap.clear();
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
  }

  // 1
  EXPECT_EQ(2, routeMapList["1"].unicastRoutes_ref()->size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj13, false, 10),
                createNextHopFromAdj(adj12, false, 100)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].unicastRoutes_ref()->size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 100)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].unicastRoutes_ref()->size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].unicastRoutes_ref()->size());
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
  initialPub.set_area(kTestingAreaName);

  // wait for the inital coldstart sync, expect it to be empty
  EXPECT_EQ(0, recvRouteUpdates().unicastRoutesToUpdate.size());

  std::string keyToDup;

  // Create full topology
  for (int i = 1; i <= 1000; i++) {
    const std::string src = folly::to<std::string>(i);

    // Create prefixDb value
    const auto addr = toIpPrefix(folly::sformat("face:cafe:babe::{}/128", i));
    auto kv = createPrefixKeyValue(src, 1, addr);
    if (1 == i) {
      // arbitrarily choose the first key to send duplicate publications for
      keyToDup = kv.first;
    }
    initialPub.keyVals_ref()->emplace(kv);

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
    initialPub.keyVals_ref()->emplace(
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
  duplicatePub.set_area(kTestingAreaName);
  duplicatePub.keyVals_ref()[keyToDup] = initialPub.keyVals_ref()->at(keyToDup);
  int64_t totalSent = 0;
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (diff > (2 * debounceTimeoutMax)) {
      LOG(INFO) << "Hammered decision with " << totalSent
                << " updates. Stopping";
      break;
    }
    ++totalSent;
    sendKvPublication(duplicatePub);
  }

  // Receive RouteUpdate from Decision
  auto routeUpdates1 = recvRouteUpdates();
  EXPECT_EQ(999, routeUpdates1.unicastRoutesToUpdate.size()); // Route to all
                                                              // nodes except
                                                              // mine

  //
  // Advertise prefix update. Decision gonna take some
  // good amount of time to process this last update (as it has many queued
  // updates).
  //
  thrift::Publication newPub;
  newPub.set_area(kTestingAreaName);

  auto newAddr = toIpPrefix("face:b00c:babe::1/128");
  newPub.set_keyVals({createPrefixKeyValue("1", 1, newAddr)});
  LOG(INFO) << "Advertising prefix update";
  sendKvPublication(newPub);
  // Receive RouteDelta from Decision
  auto routeUpdates2 = recvRouteUpdates();
  // Expect no routes delta
  EXPECT_EQ(0, routeUpdates2.unicastRoutesToUpdate.size());

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

//
// This test aims to verify counter reporting from Decision module
//
TEST_F(DecisionTestFixture, Counters) {
  // Verifiy some initial/default counters
  {
    decision->updateGlobalCounters();
    const auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(counters.at("decision.num_nodes"), 1);
    EXPECT_EQ(counters.at("decision.num_conflicting_prefixes"), 0);
  }

  // set up first publication

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
      thrift::MetricVector{} /* empty metric vector */);
  auto bgpPrefixEntry2 = createPrefixEntry( // Missing metric vector
      toIpPrefix("10.3.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.3.0.0/16",
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      std::nullopt /* missing metric vector */);
  auto bgpPrefixEntry3 = createPrefixEntry( // Conflicting forwarding type
      toIpPrefix("10.3.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.3.0.0/16",
      thrift::PrefixForwardingType::SR_MPLS,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      thrift::MetricVector{} /* empty metric vector */);
  std::unordered_map<std::string, thrift::Value> pubKvs = {
      {"adj:1", createAdjValue("1", 1, {adj12, adj13}, false, 1)},
      {"adj:2", createAdjValue("2", 1, {adj21, adj23}, false, 2)},
      {"adj:3", createAdjValue("3", 1, {adj31}, false, 3 << 20)}, // invalid
                                                                  // mpls
                                                                  // label
      {"adj:4", createAdjValue("4", 1, {}, false, 4)} // Disconnected node
  };
  pubKvs.emplace(createPrefixKeyValue("1", 1, addr1));
  pubKvs.emplace(createPrefixKeyValue("1", 1, addr1V4));

  pubKvs.emplace(createPrefixKeyValue("2", 1, addr2));
  pubKvs.emplace(createPrefixKeyValue("2", 1, addr2V4));

  pubKvs.emplace(createPrefixKeyValue("3", 1, addr3));
  pubKvs.emplace(createPrefixKeyValue("3", 1, bgpPrefixEntry1));
  pubKvs.emplace(createPrefixKeyValue("3", 1, bgpPrefixEntry3));
  pubKvs.emplace(createPrefixKeyValue("3", 1, mplsPrefixEntry1));

  pubKvs.emplace(createPrefixKeyValue("4", 1, addr4));
  pubKvs.emplace(createPrefixKeyValue("4", 1, bgpPrefixEntry2));

  // Node1 connects to 2/3, Node2 connects to 1, Node3 connects to 1
  // Node2 has partial adjacency
  auto publication0 =
      createThriftPublication(pubKvs, {}, {}, {}, std::string(""));
  sendKvPublication(publication0);
  const auto routeDb = recvRouteUpdates();
  for (const auto& [_, uniRoute] : routeDb.unicastRoutesToUpdate) {
    EXPECT_NE(
        folly::IPAddress::networkToString(uniRoute.prefix), "10.1.0.0/16");
  }

  // Verify counters
  decision->updateGlobalCounters();
  const auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.num_conflicting_prefixes"), 1);
  EXPECT_EQ(counters.at("decision.num_partial_adjacencies"), 1);
  EXPECT_EQ(counters.at("decision.num_complete_adjacencies"), 2);
  EXPECT_EQ(counters.at("decision.num_nodes"), 4);
  EXPECT_EQ(counters.at("decision.num_prefixes"), 9);
  EXPECT_EQ(counters.at("decision.no_route_to_prefix.count.60"), 1);
  EXPECT_EQ(counters.at("decision.incompatible_forwarding_type.count.60"), 1);
  EXPECT_EQ(counters.at("decision.skipped_unicast_route.count.60"), 0);
  EXPECT_EQ(counters.at("decision.skipped_mpls_route.count.60"), 1);
  EXPECT_EQ(counters.at("decision.no_route_to_label.count.60"), 1);

  // fully disconnect node 2
  auto publication1 = createThriftPublication(
      {{"adj:1", createAdjValue("1", 2, {adj13}, false, 1)}},
      {},
      {},
      {},
      std::string(""));
  sendKvPublication(publication1);
  // wait for update
  recvRouteUpdates();

  decision->updateGlobalCounters();
  EXPECT_EQ(
      fb303::fbData->getCounters().at("decision.num_partial_adjacencies"), 0);
}

TEST_F(DecisionTestFixture, ExceedMaxBackoff) {
  for (int i = debounceTimeoutMin.count(); true; i *= 2) {
    auto nodeName = std::to_string(i);
    auto publication = createThriftPublication(
        {createPrefixKeyValue(nodeName, 1, addr1)},
        {},
        {},
        {},
        std::string(""));
    sendKvPublication(publication);
    if (i >= debounceTimeoutMax.count()) {
      break;
    }
  }

  // wait for debouncer to try to fire
  /* sleep override */
  std::this_thread::sleep_for(
      debounceTimeoutMax + std::chrono::milliseconds(100));
  // send one more update
  auto publication = createThriftPublication(
      {createPrefixKeyValue("2", 1, addr1)}, {}, {}, {}, std::string(""));
  sendKvPublication(publication);
}

// DecisionTestFixture with different enableBestRouteSelection_ input
class EnableBestRouteSelectionFixture
    : public DecisionTestFixture,
      public ::testing::WithParamInterface<bool> {
  openr::thrift::OpenrConfig
  createConfig() override {
    auto tConfig = DecisionTestFixture::createConfig();
    tConfig.enable_best_route_selection_ref() = GetParam();
    return tConfig;
  }
};

INSTANTIATE_TEST_CASE_P(
    EnableBestRouteSelectionInstance,
    EnableBestRouteSelectionFixture,
    ::testing::Bool());

//
// Mixed type prefix announcements (e.g. prefix1 with type BGP and type RIB )
// are allowed when enableBestRouteSelection_ = true,
// Otherwise prefix will be skipped in route programming.
//
TEST_P(EnableBestRouteSelectionFixture, PrefixWithMixedTypeRoutes) {
  // Verifiy some initial/default counters
  {
    decision->updateGlobalCounters();
    const auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(counters.at("decision.num_nodes"), 1);
  }

  // set up first publication

  // node 2/3 announce loopbacks
  {
    const auto prefixDb2 = createPrefixDb(
        "2", {createPrefixEntry(addr2), createPrefixEntry(addr2V4)});
    const auto prefixDb3 = createPrefixDb(
        "3", {createPrefixEntry(addr3), createPrefixEntry(addr3V4)});

    // Node1 connects to 2/3, Node2 connects to 1, Node3 connects to 1
    auto publication = createThriftPublication(
        {{"adj:1", createAdjValue("1", 1, {adj12, adj13}, false, 1)},
         {"adj:2", createAdjValue("2", 1, {adj21}, false, 2)},
         {"adj:3", createAdjValue("3", 1, {adj31}, false, 3)},
         createPrefixKeyValue("2", 1, addr2),
         createPrefixKeyValue("2", 1, addr2V4),
         createPrefixKeyValue("3", 1, addr3),
         createPrefixKeyValue("3", 1, addr3V4)},
        {},
        {},
        {},
        std::string(""));
    sendKvPublication(publication);
    recvRouteUpdates();
  }

  // Node2 annouce prefix in BGP type,
  // Node3 announce prefix in Rib type
  {
    auto bgpPrefixEntry = createPrefixEntry(
        toIpPrefix("10.1.0.0/16"),
        thrift::PrefixType::BGP,
        "data=10.1.0.0/16",
        thrift::PrefixForwardingType::IP,
        thrift::PrefixForwardingAlgorithm::SP_ECMP,
        thrift::MetricVector{} /* empty metric vector */);
    auto ribPrefixEntry = createPrefixEntry(
        toIpPrefix("10.1.0.0/16"),
        thrift::PrefixType::RIB,
        "",
        thrift::PrefixForwardingType::IP,
        thrift::PrefixForwardingAlgorithm::SP_ECMP);

    auto publication = createThriftPublication(
        // node 2 announce BGP prefix with loopback
        {createPrefixKeyValue("2", 1, bgpPrefixEntry),
         createPrefixKeyValue("3", 1, ribPrefixEntry)},
        {},
        {},
        {},
        std::string(""));
    sendKvPublication(publication);
    recvRouteUpdates();
  }
  // Verify counters
  decision->updateGlobalCounters();
  const auto counters = fb303::fbData->getCounters();
  int skippedUnicastRouteCnt = GetParam() ? 0 : 1;
  EXPECT_EQ(
      skippedUnicastRouteCnt,
      counters.at("decision.skipped_unicast_route.count.60"));
}

TEST(DecisionPendingUpdates, needsFullRebuild) {
  openr::detail::DecisionPendingUpdates updates("node1");
  LinkState::LinkStateChange linkStateChange;

  linkStateChange.linkAttributesChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  updates.applyLinkStateChange("node1", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());

  updates.reset();
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  linkStateChange.linkAttributesChanged = false;
  linkStateChange.topologyChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());

  updates.reset();
  linkStateChange.topologyChanged = false;
  linkStateChange.nodeLabelChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());
}

TEST(DecisionPendingUpdates, updatedPrefixes) {
  openr::detail::DecisionPendingUpdates updates("node1");

  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());

  // empty update no change
  updates.applyPrefixStateChange({}, kEmptyPerfEventRef);
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());

  updates.applyPrefixStateChange({addr1, addr2V4}, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_THAT(
      updates.updatedPrefixes(), testing::UnorderedElementsAre(addr1, addr2V4));
  updates.applyPrefixStateChange({addr2}, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_THAT(
      updates.updatedPrefixes(),
      testing::UnorderedElementsAre(addr1, addr2V4, addr2));

  updates.reset();
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());
}

TEST(DecisionPendingUpdates, perfEvents) {
  openr::detail::DecisionPendingUpdates updates("node1");
  LinkState::LinkStateChange linkStateChange;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_THAT(*updates.perfEvents()->events_ref(), testing::SizeIs(1));
  EXPECT_EQ(
      *updates.perfEvents()->events_ref()->front().eventDescr_ref(),
      "DECISION_RECEIVED");
  thrift::PrefixDatabase perfEventDb;
  perfEventDb.perfEvents_ref() = openr::thrift::PerfEvents();
  auto& earlierEvents = *perfEventDb.perfEvents_ref();
  earlierEvents.events_ref()->push_back({});
  *earlierEvents.events_ref()->back().nodeName_ref() = "node3";
  *earlierEvents.events_ref()->back().eventDescr_ref() = "EARLIER";
  earlierEvents.events_ref()->back().unixTs_ref() = 1;
  updates.applyPrefixStateChange({}, perfEventDb.perfEvents_ref());

  // expect what we hasd to be displaced by this
  EXPECT_THAT(*updates.perfEvents()->events_ref(), testing::SizeIs(2));
  EXPECT_EQ(
      *updates.perfEvents()->events_ref()->front().eventDescr_ref(), "EARLIER");
  EXPECT_EQ(
      *updates.perfEvents()->events_ref()->back().eventDescr_ref(),
      "DECISION_RECEIVED");
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
