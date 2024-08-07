/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <vector>

#include <openr/decision/LinkState.h>
#include <openr/decision/SpfSolver.h>

namespace openr {

// Note: use unordered_set bcoz paths in a route can be in arbitrary order
using NextHops = std::unordered_set<thrift::NextHopThrift>;
using RouteMap = std::unordered_map<
    std::pair<std::string /* node name */, std::string /* prefix or label */>,
    NextHops>;

// Builds LinkState structure with interger named nodes up to 2^16.
// Input is map of node to adjacency and adj weight. Can support parallel
// adjacencies
//
// example use, create a simple box topolgy with one parallel adj:
//
//      10
//   1------2
//   |      |\
//  5|   15 | | 20
//   |      |/
//   3------4
//      20
//
// auto linkState = getLinkState({{1, {{2, 10}, {3, 5}}},
//                                {2, {{1, 10}, {4, 15}, {4, 20}}},
//                                {3, {{1, 5}, {4, 20}}},
//                                {4, {{2, 15}, {3, 20}, {2, 20}}},
//                              });
//
LinkState getLinkState(
    std::unordered_map<
        int /* node */,
        std::vector<std::pair<int /* adjNode */, int /* weight */>>> adjMap);

// overload without providing link weight
LinkState getLinkState(
    std::unordered_map<int /* node */, std::vector<int /* adjNode */>> adjMap);

thrift::NextHopThrift createNextHopFromAdj(
    thrift::Adjacency adj,
    bool isV4,
    int32_t metric,
    std::optional<thrift::MplsAction> mplsAction = std::nullopt,
    const std::string& area = kTestingAreaName,
    bool v4OverV6Nexthop = false,
    int64_t weight = 0);

// Note: routeMap will be modified
void fillRouteMap(
    const std::string& node,
    RouteMap& routeMap,
    const DecisionRouteDb& routeDb);

void fillRouteMap(
    const std::string& node,
    RouteMap& routeMap,
    const thrift::RouteDatabase& routeDb);
} // namespace openr
