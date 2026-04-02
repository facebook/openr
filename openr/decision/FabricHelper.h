/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/container/F14Map.h>
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
      const folly::F14FastMap<std::string /* nodeName */, Link::LinkSet>&
          linkMap)
      : fabricConfig_(fabricConfig), linkMap_(linkMap) {}

  // Returns the name of the fabric.
  std::string getFabricName() const;

  // Returns the name of the leaf that the external link is connected to.
  std::string getRealOtherNodeName(
      const std::string& nodeName, const thrift::Adjacency& adj) const;

  void updateExternalNodeToLeafMap(
      const openr::thrift::AdjacencyDatabase& newAdjacencyDb);

  // Returns the name of fabric node that is currently the master generator.
  std::string getFabricMasterGenerator() const;

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

  FabricConfig fabricConfig_;

  // External node/interface to the fabric leaf/interface mapping.
  folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
      externalNodeToLeaf_;

  // Leaf node name to (external NodeInterface -> leaf NodeInterface) mapping.
  folly::F14NodeMap<
      std::string,
      folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>>
      leafToExternalNode_;

  const folly::F14FastMap<std::string /* nodeName */, Link::LinkSet>& linkMap_;

  friend class FabricHelperTestFixture;
};

} // namespace openr
