/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/container/F14Map.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/decision/Link.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

#pragma once

namespace openr {

class FabricHelper {
 public:
  FabricHelper(
      const FabricConfig& fabricConfig,
      const folly::F14NodeMap<std::string /* nodeName */, Link::LinkSet>&
          linkMap,
      const folly::F14FastMap<std::string, thrift::AdjacencyDatabase>&
          adjacencyDatabases,
      const std::string& area)
      : fabricConfig_(fabricConfig),
        linkMap_(linkMap),
        adjacencyDatabases_(adjacencyDatabases),
        area_(area) {}

  // Returns the name of the fabric.
  std::string getFabricName() const;

  // Returns the name of the leaf that the external link is connected to.
  std::string getRealOtherNodeName(
      const std::string& nodeName, const thrift::Adjacency& adj) const;

  void updateExternalNodeToLeafMap(
      const openr::thrift::AdjacencyDatabase& newAdjacencyDb);

  // Returns the name of fabric node that is currently the master generator.
  std::string getFabricMasterGenerator() const;

  // Returns a pair of:
  //   bool: True the changed keys contain a leaf, spine or control node's key
  //   std::unordered_set<std::string>: Set of leaf node names whose keys
  //   changed
  //
  // Note: Only a spine or a control nodes' adjacency changes, returns
  // {true,{}}. The bool is used to determine if the fabric master generator may
  // have changed.
  std::pair<bool, std::unordered_set<std::string>> getFabricChanges(
      const std::unordered_set<std::string>& changedKeys) const;

  // Clears external adjacencies and returns KV unset requests if anything
  // changed. Returns an empty vector if nothing was cleared.
  std::vector<ClearKeyValueRequest> clearFabricKvs();

  // Updates external adjacencies for the given changed leaves and returns KV
  // set requests if anything changed. Returns an empty vector if unchanged.
  std::vector<PersistKeyValueRequest> updateChangedFabricKvs(
      const std::unordered_set<std::string>& changedLeafNames);

 private:
  struct NodeInterface {
    std::string nodeName;
    std::string ifName;
    bool
    operator==(const NodeInterface& other) const {
      return nodeName == other.nodeName && ifName == other.ifName;
    }
  };
  struct NodeInterfaceHasher {
    std::size_t
    operator()(const NodeInterface& s) const noexcept {
      return folly::hash::hash_combine(s.nodeName, s.ifName);
    }
  };

  // Returns true if any of the adjacencies changed.
  bool updateFabricAdjacencies(
      const std::unordered_set<std::string>& changedNodes);

  FabricConfig fabricConfig_;

  // External node/interface to the fabric leaf/interface mapping.
  folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
      externalNodeToLeaf_;

  // Leaf node name to (external NodeInterface -> leaf NodeInterface) mapping.
  folly::F14NodeMap<
      std::string,
      folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>>
      leafToExternalNode_;

  const folly::F14NodeMap<std::string /* nodeName */, Link::LinkSet>& linkMap_;

  const folly::F14FastMap<std::string, thrift::AdjacencyDatabase>&
      adjacencyDatabases_;

  // True nodeName (not key) -> {leaf to external adjacency}
  std::unordered_map<std::string, std::set<thrift::Adjacency>>
      externalAdjacencies_;

  // The area for the adjacencies.
  const std::string area_;

  apache::thrift::CompactSerializer serializer_;

  friend class FabricHelperTestFixture;
};

} // namespace openr
