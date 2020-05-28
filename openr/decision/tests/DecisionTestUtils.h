/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <vector>

#include <openr/decision/LinkState.h>

namespace openr {

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
} // namespace openr
