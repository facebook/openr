/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/Constants.h>
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
  for (const thrift::Adjacency& adj : *newAdjacencyDb.adjacencies()) {
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

std::pair<bool, std::unordered_set<std::string>>
FabricHelper::getFabricChanges(
    const std::unordered_set<std::string>& changedKeys) const {
  std::unordered_set<std::string> changedFabricLeafNames;
  bool fabricNodeChanged = false;
  for (const std::string& key : changedKeys) {
    if (fabricConfig_.isLeafAdjKey(key)) {
      fabricNodeChanged = true;
      // Remove adj: from the key to get the node name.
      std::string nodeName = key.substr(Constants::kAdjDbMarker.size());
      changedFabricLeafNames.emplace(nodeName);
      continue;
    }
    if (fabricConfig_.isFabricAdjKey(key)) {
      fabricNodeChanged = true;
    }
  }
  return {fabricNodeChanged, changedFabricLeafNames};
}

bool
FabricHelper::updateFabricAdjacencies(
    const std::unordered_set<std::string>& changedNodes) {
  bool rebuildNeeded = externalAdjacencies_.empty();
  bool changed = false;
  for (const auto& [_, adjDb] : adjacencyDatabases_) {
    const std::string& nodeName = *adjDb.thisNodeName();
    if (!rebuildNeeded && !changedNodes.contains(nodeName)) {
      continue;
    }
    if (!fabricConfig_.isLeaf(nodeName)) {
      continue;
    }
    std::set<thrift::Adjacency> newExternalAdjs;
    for (const thrift::Adjacency& adj : *adjDb.adjacencies()) {
      if (fabricConfig_.isFabric(*adj.otherNodeName())) {
        continue;
      }
      newExternalAdjs.insert(adj);
    }
    std::set<thrift::Adjacency>& existingAdjs = externalAdjacencies_[nodeName];
    if (existingAdjs != newExternalAdjs) {
      existingAdjs = std::move(newExternalAdjs);
      changed = true;
    }
  }
  return changed;
}

std::vector<ClearKeyValueRequest>
FabricHelper::clearFabricKvs() {
  if (externalAdjacencies_.empty()) {
    return {};
  }
  externalAdjacencies_.clear();

  std::vector<ClearKeyValueRequest> requests;
  const std::string& fabricName = getFabricName();
  // Erase the local key from KvStore, but do not flood a ttl=0 key
  // by setting setValue=false in ClearKeyValueRequest.
  requests.emplace_back(
      AreaId{area_}, fmt::format("{}{}", Constants::kAdjDbMarker, fabricName));
  for (const std::string& fabricPrefix : fabricConfig_.getFabricPrefixes()) {
    requests.emplace_back(
        AreaId{area_},
        fmt::format(
            "{}{}:[{}]", Constants::kPrefixDbMarker, fabricName, fabricPrefix));
  }
  return requests;
}

std::vector<PersistKeyValueRequest>
FabricHelper::updateChangedFabricKvs(
    const std::unordered_set<std::string>& changedLeafNames) {
  if (!updateFabricAdjacencies(changedLeafNames)) {
    return {};
  }

  thrift::AdjacencyDatabase fabricAdjDb;
  for (const auto& [_, adjacencies] : externalAdjacencies_) {
    for (const thrift::Adjacency& adj : adjacencies) {
      fabricAdjDb.adjacencies()->push_back(adj);
    }
  }
  const std::string& fabricName = getFabricName();
  fabricAdjDb.thisNodeName() = fabricName;
  fabricAdjDb.area() = area_;

  std::vector<PersistKeyValueRequest> requests;
  std::string adjDbStr = writeThriftObjStr(fabricAdjDb, serializer_);
  requests.emplace_back(
      AreaId{area_},
      fmt::format("{}{}", Constants::kAdjDbMarker, fabricName),
      adjDbStr);
  for (const std::string& fabricPrefix : fabricConfig_.getFabricPrefixes()) {
    thrift::PrefixDatabase prefixDb;
    prefixDb.thisNodeName() = fabricName;
    prefixDb.prefixEntries() = {createPrefixEntry(toIpPrefix(fabricPrefix))};
    std::string prefixDbStr = writeThriftObjStr(prefixDb, serializer_);
    requests.emplace_back(
        AreaId{area_},
        fmt::format(
            "{}{}:[{}]", Constants::kPrefixDbMarker, fabricName, fabricPrefix),
        prefixDbStr);
  }
  return requests;
}

} // namespace openr
