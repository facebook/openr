/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/decision/PrefixState.h"

#include <openr/common/Util.h>

namespace openr {

bool
PrefixState::updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb) {
  auto const& nodeName = prefixDb.thisNodeName;

  // Get old and new set of prefixes - NOTE explicit copy
  const std::set<thrift::IpPrefix> oldPrefixSet = nodeToPrefixes_[nodeName];

  // update the entry
  auto& newPrefixSet = nodeToPrefixes_[nodeName];
  newPrefixSet.clear();
  for (const auto& prefixEntry : prefixDb.prefixEntries) {
    newPrefixSet.emplace(prefixEntry.prefix);
  }

  // Boolean to indicate update in prefix entry
  bool isUpdated{false};

  // Remove old prefixes first
  for (const auto& prefix : oldPrefixSet) {
    if (newPrefixSet.count(prefix)) {
      continue;
    }
    VLOG(1) << "Prefix " << toString(prefix) << " has been withdrawn by "
            << nodeName;
    auto& nodeList = prefixes_.at(prefix);
    nodeList.erase(nodeName);
    isUpdated = true;
    if (nodeList.empty()) {
      prefixes_.erase(prefix);
    }
  }
  for (const auto& prefixEntry : prefixDb.prefixEntries) {
    auto& nodeList = prefixes_[prefixEntry.prefix];
    auto nodePrefixIt = nodeList.find(nodeName);

    // Add or Update prefix
    if (nodePrefixIt == nodeList.end()) {
      VLOG(1) << "Prefix " << toString(prefixEntry.prefix)
              << " has been advertised by node " << nodeName;
      nodeList.emplace(nodeName, prefixEntry);
      isUpdated = true;
    } else if (nodePrefixIt->second != prefixEntry) {
      VLOG(1) << "Prefix " << toString(prefixEntry.prefix)
              << " has been updated by node " << nodeName;
      nodeList[nodeName] = prefixEntry;
      isUpdated = true;
    } else {
      // This prefix has no change. Skip rest of code!
      continue;
    }

    // Keep track of loopback addresses (v4 / v6) for each node
    if (thrift::PrefixType::LOOPBACK == prefixEntry.type) {
      auto addrSize = prefixEntry.prefix.prefixAddress.addr.size();
      if (addrSize == folly::IPAddressV4::byteCount() &&
          folly::IPAddressV4::bitCount() == prefixEntry.prefix.prefixLength) {
        nodeHostLoopbacksV4_[nodeName] = prefixEntry.prefix.prefixAddress;
      }
      if (addrSize == folly::IPAddressV6::byteCount() &&
          folly::IPAddressV6::bitCount() == prefixEntry.prefix.prefixLength) {
        nodeHostLoopbacksV6_[nodeName] = prefixEntry.prefix.prefixAddress;
      }
    }
  }

  return isUpdated;
}

bool
PrefixState::deletePrefixDatabase(const std::string& nodeName) {
  VLOG(1) << "Deleting prefix database for node " << nodeName;
  auto search = nodeToPrefixes_.find(nodeName);
  if (search == nodeToPrefixes_.end()) {
    LOG(INFO) << "Trying to delete non-existent prefix db for node "
              << nodeName;
    return false;
  }

  bool isUpdated = false;
  for (const auto& prefix : search->second) {
    try {
      auto& nodeList = prefixes_.at(prefix);
      nodeList.erase(nodeName);
      isUpdated = true;
      VLOG(1) << "Prefix " << toString(prefix) << " has been withdrawn by "
              << nodeName;
      if (nodeList.empty()) {
        prefixes_.erase(prefix);
      }
    } catch (std::out_of_range const& e) {
      LOG(FATAL) << "std::out_of_range prefix error for " << nodeName;
    }
  }

  nodeToPrefixes_.erase(search);
  nodeHostLoopbacksV4_.erase(nodeName);
  nodeHostLoopbacksV6_.erase(nodeName);
  return isUpdated;
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
PrefixState::getPrefixDatabases() const {
  std::unordered_map<std::string, thrift::PrefixDatabase> prefixDatabases;
  for (auto const& kv : nodeToPrefixes_) {
    thrift::PrefixDatabase prefixDb;
    prefixDb.thisNodeName = kv.first;
    for (auto const& prefix : kv.second) {
      prefixDb.prefixEntries.emplace_back(prefixes_.at(prefix).at(kv.first));
    }
    prefixDatabases.emplace(kv.first, std::move(prefixDb));
  }
  return prefixDatabases;
}

std::vector<thrift::NextHopThrift>
PrefixState::getLoopbackVias(
    std::unordered_set<std::string> const& nodes,
    bool const isV4,
    folly::Optional<int64_t> const& igpMetric) const {
  std::vector<thrift::NextHopThrift> result;
  result.reserve(nodes.size());
  auto const& hostLoopBacks =
      isV4 ? nodeHostLoopbacksV4_ : nodeHostLoopbacksV6_;
  for (auto const& node : nodes) {
    if (!hostLoopBacks.count(node)) {
      LOG(ERROR) << "No loopback for node " << node;
    } else {
      result.emplace_back(
          createNextHop(hostLoopBacks.at(node), "", igpMetric.value_or(0)));
    }
  }
  return result;
}
} // namespace openr
