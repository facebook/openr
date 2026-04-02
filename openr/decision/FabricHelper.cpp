/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/decision/FabricHelper.h>

namespace openr {

std::string
FabricHelper::getFabricName() const {
  return fabricConfig_.getFabricName();
}

std::string
FabricHelper::getRealOtherNodeName(
    const std::string& nodeName, const thrift::Adjacency& adj) const {
  if (*adj.otherNodeName() == fabricConfig_.getFabricName()) {
    NodeInterface nodeInterface{.nodeName = nodeName, .ifName = *adj.ifName()};
    auto leaf = externalNodeToLeaf_.find(nodeInterface);
    if (leaf != externalNodeToLeaf_.end()) {
      return leaf->second.nodeName;
    }
  }
  return *adj.otherNodeName();
}

void
FabricHelper::updateExternalNodeToLeafMap(
    const openr::thrift::AdjacencyDatabase& newAdjacencyDb) {
  const std::string& leafName = *newAdjacencyDb.thisNodeName();
  if (!fabricConfig_.isLeaf(leafName)) {
    return;
  }

  // Build the new map of external NI -> leaf NI from newAdjacencyDb.
  folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
      newExternalLinks;
  for (const auto& adj : *newAdjacencyDb.adjacencies()) {
    if (fabricConfig_.isFabric(*adj.otherNodeName())) {
      continue;
    }
    NodeInterface leaf{.nodeName = leafName, .ifName = *adj.ifName()};
    NodeInterface other{
        .nodeName = *adj.otherNodeName(), .ifName = *adj.otherIfName()};
    newExternalLinks.insert_or_assign(std::move(other), std::move(leaf));
  }

  // Remove stale entries from externalNodeToLeaf_.
  if (auto it = leafToExternalNode_.find(leafName);
      it != leafToExternalNode_.end()) {
    for (const auto& [ext, oldLeaf] : it->second) {
      auto newIt = newExternalLinks.find(ext);
      if (newIt == newExternalLinks.end() || newIt->second != oldLeaf) {
        externalNodeToLeaf_.erase(ext);
      }
    }
  }

  // Update externalNodeToLeaf_ with current entries.
  for (const auto& [other, leaf] : newExternalLinks) {
    externalNodeToLeaf_.insert_or_assign(other, leaf);
  }

  // Update leafToExternalNode_.
  if (newExternalLinks.empty()) {
    leafToExternalNode_.erase(leafName);
  } else {
    leafToExternalNode_.insert_or_assign(leafName, std::move(newExternalLinks));
  }
}

// Returns the name of the master generator node. The master generator is the
// node that is
// - not disconnected from the rest of the nodes, and
// - has the lexicographically highest name string.
std::string
FabricHelper::getFabricMasterGenerator() const {
  std::string master;
  for (const auto& [nodeName, linkSet] : linkMap_) {
    if (linkSet.empty()) {
      // disconnected node
      continue;
    }
    if (!fabricConfig_.isFabric(nodeName)) {
      // non-fabric node
      continue;
    }
    if (master < nodeName) {
      // higher name
      master = nodeName;
    }
  }
  return master;
}

} // namespace openr
