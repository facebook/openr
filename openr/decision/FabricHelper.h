/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/format.h>
#include <folly/Range.h>
#include <folly/container/F14Map.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/decision/Link.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/messaging/ReplicateQueue.h>

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
      const std::string& area,
      const std::string& myNodeName,
      messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue)
      : fabricConfig_(fabricConfig),
        linkMap_(linkMap),
        adjacencyDatabases_(adjacencyDatabases),
        area_(area),
        myNodeName_(myNodeName),
        drainStatusKey_(
            fmt::format(
                "{}{}",
                FabricConfig::kDrainStatusMarker,
                fabricConfig.getFabricName())),
        kvRequestQueue_(kvRequestQueue) {}

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

  // Updates this fabric's synthetic key-values in response to the changed keys,
  // including this fabric's drain status. Refreshes the tracked fabric master
  // generator and pushes the resulting requests onto the kvRequestQueue.
  // Generates keys only if `myNodeName_ == fabricMasterName_` (this node is the
  // fabric master); otherwise clears previously generated keys.
  void updateFabricKv(
      const std::unordered_set<std::string>& changedKeys,
      const thrift::Publication& thriftPub);

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

  // Updates this fabric's drain status from the given publication. Returns true
  // if the fabric's drain status changed.
  bool updateFabricDrainStatus(const thrift::Publication& thriftPub);

  // Returns true if the fabric's drainStatus changed.
  bool setDrainStatus(const thrift::InstanceDrainStatus& drainStatus);

  // Updates external adjacencies of the changed leaves and stores the
  // fabric node's drain status. Returns updated KV set requests for the fabric;
  // empty vector if nothing changed.
  std::vector<PersistKeyValueRequest> updateChangedFabricKvs(
      const std::unordered_set<std::string>& changedLeafNames,
      bool isDrainStatusChanged);

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

  // Name of the local node this Open/R instance runs on. Used to determine
  // whether this node is the fabric master generator.
  const std::string myNodeName_;

  // The drain status key for this fabric.
  const std::string drainStatusKey_;

  // Name of the node currently generating this fabric's synthetic key-values.
  // Refreshed on each updateFabricKv(); empty until first computed.
  std::string fabricMasterName_;

  // Queue onto which generated/cleared fabric key-value requests are pushed.
  messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue_;

  // Current drain status of this fabric node from KvStore. Defaults to
  // undrained.
  thrift::InstanceDrainStatus fabricDrainStatus_;

  apache::thrift::CompactSerializer serializer_;

  friend class FabricHelperTestFixture;
};

} // namespace openr
