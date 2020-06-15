/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Format.h>

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

#include <openr/decision/tests/DecisionTestUtils.h>

namespace openr {
LinkState
getLinkState(std::unordered_map<int, std::vector<std::pair<int, int>>> adjMap) {
  using folly::sformat;
  LinkState linkState{openr::thrift::KvStore_constants::kDefaultArea()};
  for (auto const& [node, adjList] : adjMap) {
    CHECK_LT(node, 0x1 << 16);
    std::vector<thrift::Adjacency> adjs;
    std::unordered_map<int, int> numParallel;
    for (auto const [adj, weight] : adjList) {
      CHECK_LT(adj, 0x1 << 16);
      auto adjNum = numParallel[adj]++;
      int bottomByte = adj & 0xFF;
      int topByte = (adj & 0xFF00) >> 8;
      adjs.push_back(createAdjacency(
          sformat("{}", adj),
          sformat("{}/{}/{}", node, adj, adjNum),
          sformat("{}/{}/{}", adj, node, adjNum),
          sformat("fe80::{:02x}{:02x}", topByte, bottomByte),
          sformat("192.168.{}.{}", topByte, bottomByte),
          weight,
          // label top 16 bits are me, bottom is neighbor
          ((node << 16) + adj)));
    }
    linkState.updateAdjacencyDatabase(
        createAdjDb(sformat("{}", node), adjs, node), 0, 0);
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
} // namespace openr
