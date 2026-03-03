/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <string>
#include <vector>

#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {

/*
 * KvStoreDataBuilder builds per-neighbor KV data with variations.
 *
 * Each neighbor's KV store contains:
 * 1. Its own adj:<neighborName> key (unique originatorId)
 * 2. Its own prefix:<neighborName> keys (unique originatorId)
 * 3. All other routers' adj/prefix keys (shared topology)
 *
 * This models real network behavior where each peer has full topology
 * but with its own self-originated keys.
 *
 * Usage:
 *   auto allData = KvStoreDataBuilder::buildForAllNeighbors(neighborNames,
 * topology); for (const auto& [name, kvData] : allData) {
 *     manager.addNeighbor(name, std::move(kvData));
 *   }
 */
class KvStoreDataBuilder {
 public:
  /*
   * Build KV data for a specific neighbor.
   *
   * The neighbor gets:
   * - Its own adj:<neighborName> key (unique originatorId)
   * - Its own prefix:<neighborName> keys (unique originatorId)
   * - All other routers' adj/prefix keys (shared topology)
   *
   * @param topology The full topology containing all routers
   * @return KeyVals for this neighbor's KV store
   */
  static thrift::KeyVals buildForNeighbor(const Topology& topology);

  /*
   * Build KV data for ALL neighbors at once (more efficient).
   *
   * This builds shared topology data once and copies it for each neighbor,
   * only adding the unique self-originated keys per neighbor.
   *
   * @param neighborNames List of neighbor names to build data for
   * @param topology The full topology containing all routers
   * @return Map of neighborName -> KeyVals
   */
  static std::map<std::string, thrift::KeyVals> buildForAllNeighbors(
      const std::vector<std::string>& neighborNames, const Topology& topology);

  /*
   * Build the adjacency database key-value for a router.
   *
   * @param router The router to build adjDb for
   * @param topology Full topology for building adjacencies
   * @param version Version number for the key
   * @return Pair of ("adj:<nodeName>", serialized AdjacencyDatabase)
   */
  static std::pair<std::string, thrift::Value> buildAdjKeyValue(
      const VirtualRouter& router,
      const Topology& topology,
      int64_t version = 1);

  /*
   * Build prefix key-values for a router.
   *
   * @param router The router to build prefix keys for
   * @param version Version number for the keys
   * @return Vector of ("prefix:<nodeName>:<prefix>", serialized PrefixDatabase)
   */
  static std::vector<std::pair<std::string, thrift::Value>>
  buildPrefixKeyValues(const VirtualRouter& router, int64_t version = 1);

  /*
   * Build an adjacency database with a specific adjacency removed.
   * Used for simulating link flaps.
   *
   * @param router The router whose adj DB to modify
   * @param topology Full topology
   * @param adjToRemove Name of the adjacency to remove (remoteRouterName)
   * @param version Version number for the key
   * @return Pair of ("adj:<nodeName>", serialized AdjacencyDatabase)
   */
  static std::pair<std::string, thrift::Value> buildAdjKeyValueWithLinkDown(
      const VirtualRouter& router,
      const Topology& topology,
      const std::string& adjToRemove,
      int64_t version);

  /*
   * Build an empty, overloaded adjacency database for node removal.
   *
   * @param routerName Name of the router to mark as removed
   * @param version Version number for the key
   * @return Pair of ("adj:<routerName>", serialized empty AdjacencyDatabase)
   */
  static std::pair<std::string, thrift::Value> buildRemovedNodeAdjKeyValue(
      const std::string& routerName, int64_t version);

  /*
   * Build an overloaded adjacency database (node still has adjacencies but
   * is not used for transit traffic).
   *
   * @param router The router to mark as overloaded
   * @param topology Full topology
   * @param version Version number for the key
   * @return Pair of ("adj:<nodeName>", serialized AdjacencyDatabase)
   */
  static std::pair<std::string, thrift::Value> buildOverloadedAdjKeyValue(
      const VirtualRouter& router, const Topology& topology, int64_t version);
};

} // namespace openr
