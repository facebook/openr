/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Format.h>

#include <openr/common/Util.h>
#include <openr/decision/tests/DecisionTestUtils.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/tests/utils/Utils.h>

namespace openr {
LinkState
getLinkState(std::unordered_map<int, std::vector<std::pair<int, int>>> adjMap) {
  using fmt::format;
  LinkState linkState{kTestingAreaName, kTestingNodeName};
  for (auto const& [node, adjList] : adjMap) {
    CHECK_LT(node, 0x1 << 16);
    std::vector<thrift::Adjacency> adjs;
    std::unordered_map<int, int> numParallel;
    for (auto const& [adj, metric] : adjList) {
      CHECK_LT(adj, 0x1 << 16);
      auto adjNum = numParallel[adj]++;
      int bottomByte = adj & 0xFF;
      int topByte = (adj & 0xFF00) >> 8;
      adjs.push_back(createAdjacency(
          format("{}", adj),
          format("{}/{}/{}", node, adj, adjNum),
          format("{}/{}/{}", adj, node, adjNum),
          format("fe80::{:02x}{:02x}", topByte, bottomByte),
          format("192.168.{}.{}", topByte, bottomByte),
          metric,
          // label top 16 bits are me, bottom is neighbor
          ((node << 16) + adj)));
    }
    linkState.updateAdjacencyDatabase(
        createAdjDb(format("{}", node), adjs, node), kTestingAreaName);
  }
  return linkState;
}

LinkState
getLinkState(std::unordered_map<int, std::vector<int>> adjMap) {
  std::unordered_map<int, std::vector<std::pair<int, int>>> weightedAdjMap;
  for (auto const& [node, adjs] : adjMap) {
    auto& entry = weightedAdjMap[node];
    for (auto const& adj : adjs) {
      entry.emplace_back(adj, 1);
    }
  }
  return getLinkState(weightedAdjMap);
}

thrift::NextHopThrift
createNextHopFromAdj(
    thrift::Adjacency adj,
    bool isV4,
    int32_t metric,
    std::optional<thrift::MplsAction> mplsAction,
    const std::string& area,
    bool v4OverV6Nexthop,
    int64_t weight) {
  return createNextHop(
      isV4 and not v4OverV6Nexthop ? *adj.nextHopV4() : *adj.nextHopV6(),
      *adj.ifName(),
      metric,
      std::move(mplsAction),
      area,
      *adj.otherNodeName(),
      weight);
}

// Note: routeMap will be modified
void
fillRouteMap(
    const std::string& node,
    RouteMap& routeMap,
    const DecisionRouteDb& routeDb) {
  for (auto const& [_, entry] : routeDb.unicastRoutes) {
    auto prefix = folly::IPAddress::networkToString(entry.prefix);
    for (const auto& nextHop : entry.nexthops) {
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << toString(nextHop);

      routeMap[make_pair(node, prefix)].emplace(nextHop);
    }
  }
}

void
fillRouteMap(
    const std::string& node,
    RouteMap& routeMap,
    const thrift::RouteDatabase& routeDb) {
  for (auto const& route : *routeDb.unicastRoutes()) {
    auto prefix = toString(*route.dest());
    for (const auto& nextHop : *route.nextHops()) {
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << toString(nextHop);

      routeMap[make_pair(node, prefix)].emplace(nextHop);
    }
  }
}

} // namespace openr
