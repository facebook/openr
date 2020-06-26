/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/decision/PrefixState.h"

#include <openr/common/Util.h>

namespace openr {

void
PrefixState::deleteLoopbackPrefix(
    thrift::IpPrefix const& prefix, const std::string& nodeName) {
  auto addrSize = prefix.prefixAddress.addr.size();
  if (addrSize == folly::IPAddressV4::byteCount() &&
      folly::IPAddressV4::bitCount() == prefix.prefixLength) {
    if (nodeHostLoopbacksV4_.find(nodeName) != nodeHostLoopbacksV4_.end() &&
        prefix.prefixAddress == nodeHostLoopbacksV4_.at(nodeName)) {
      nodeHostLoopbacksV4_.erase(nodeName);
    }
  }
  if (addrSize == folly::IPAddressV6::byteCount() &&
      folly::IPAddressV6::bitCount() == prefix.prefixLength) {
    if (nodeHostLoopbacksV6_.find(nodeName) != nodeHostLoopbacksV6_.end() &&
        nodeHostLoopbacksV6_.at(nodeName) == prefix.prefixAddress) {
      nodeHostLoopbacksV6_.erase(nodeName);
    }
  }
}

std::unordered_set<thrift::IpPrefix>
PrefixState::updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb) {
  std::unordered_set<thrift::IpPrefix> changed;

  auto const& nodeName = prefixDb.thisNodeName;

  // Get old and new set of prefixes - NOTE explicit copy
  const std::set<thrift::IpPrefix> oldPrefixSet = nodeToPrefixes_[nodeName];

  // update the entry
  auto& newPrefixSet = nodeToPrefixes_[nodeName];
  newPrefixSet.clear();
  for (const auto& prefixEntry : prefixDb.prefixEntries) {
    newPrefixSet.emplace(prefixEntry.prefix);
  }

  // Remove old prefixes first
  for (const auto& prefix : oldPrefixSet) {
    if (newPrefixSet.count(prefix)) {
      continue;
    }
    VLOG(1) << "Prefix " << toString(prefix) << " has been withdrawn by "
            << nodeName;
    auto& nodeList = prefixes_.at(prefix);
    nodeList.erase(nodeName);
    changed.insert(prefix);
    if (nodeList.empty()) {
      prefixes_.erase(prefix);
    }
    deleteLoopbackPrefix(prefix, nodeName);
  }
  for (const auto& prefixEntry : prefixDb.prefixEntries) {
    auto& nodeList = prefixes_[prefixEntry.prefix];
    auto nodePrefixIt = nodeList.find(nodeName);

    // Add or Update prefix
    if (nodePrefixIt == nodeList.end()) {
      VLOG(1) << "Prefix " << toString(prefixEntry.prefix)
              << " has been advertised by node " << nodeName;
      nodeList.emplace(nodeName, prefixEntry);
      changed.insert(prefixEntry.prefix);
    } else if (nodePrefixIt->second != prefixEntry) {
      VLOG(1) << "Prefix " << toString(prefixEntry.prefix)
              << " has been updated by node " << nodeName;
      nodeList[nodeName] = prefixEntry;
      changed.insert(prefixEntry.prefix);
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

  if (newPrefixSet.empty()) {
    nodeToPrefixes_.erase(nodeName);
  }

  return changed;
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
    std::optional<int64_t> const& igpMetric) const {
  std::vector<thrift::NextHopThrift> result;
  result.reserve(nodes.size());
  auto const& hostLoopBacks =
      isV4 ? nodeHostLoopbacksV4_ : nodeHostLoopbacksV6_;
  for (auto const& node : nodes) {
    if (!hostLoopBacks.count(node)) {
      LOG(ERROR) << "No loopback for node " << node;
    } else {
      result.emplace_back(createNextHop(
          hostLoopBacks.at(node), std::nullopt, igpMetric.value_or(0)));
    }
  }
  return result;
}
} // namespace openr
