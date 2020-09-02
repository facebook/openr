/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/decision/PrefixState.h"

#include <openr/common/Util.h>

using apache::thrift::can_throw;

namespace openr {

void
PrefixState::deleteLoopbackPrefix(
    thrift::IpPrefix const& prefix, const std::string& nodeName) {
  auto addrSize = prefix.prefixAddress_ref()->addr_ref()->size();
  if (addrSize == folly::IPAddressV4::byteCount() &&
      folly::IPAddressV4::bitCount() == *prefix.prefixLength_ref()) {
    if (nodeHostLoopbacksV4_.find(nodeName) != nodeHostLoopbacksV4_.end() &&
        *prefix.prefixAddress_ref() == nodeHostLoopbacksV4_.at(nodeName)) {
      nodeHostLoopbacksV4_.erase(nodeName);
    }
  }
  if (addrSize == folly::IPAddressV6::byteCount() &&
      folly::IPAddressV6::bitCount() == *prefix.prefixLength_ref()) {
    if (nodeHostLoopbacksV6_.find(nodeName) != nodeHostLoopbacksV6_.end() &&
        nodeHostLoopbacksV6_.at(nodeName) == *prefix.prefixAddress_ref()) {
      nodeHostLoopbacksV6_.erase(nodeName);
    }
  }
}

std::unordered_set<thrift::IpPrefix>
PrefixState::updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb) {
  std::unordered_set<thrift::IpPrefix> changed;

  auto const nodeAndArea =
      std::make_pair(*prefixDb.thisNodeName_ref(), *prefixDb.area_ref());
  auto const& nodeName = *prefixDb.thisNodeName_ref();
  auto const& area = *prefixDb.area_ref();

  // Get old and new set of prefixes - NOTE explicit copy
  const std::set<thrift::IpPrefix> oldPrefixSet =
      nodeToPrefixes_[nodeName][area];

  // update the entry
  auto& newPrefixSet = nodeToPrefixes_[nodeName][area];
  newPrefixSet.clear();
  for (const auto& prefixEntry : *prefixDb.prefixEntries_ref()) {
    newPrefixSet.emplace(*prefixEntry.prefix_ref());
  }

  // Remove old prefixes first
  for (const auto& prefix : oldPrefixSet) {
    if (newPrefixSet.count(prefix)) {
      continue;
    }

    VLOG(1) << "Prefix " << toString(prefix) << " has been withdrawn by "
            << nodeName << " from area " << area;

    auto& entriesByOriginator = prefixes_.at(prefix);
    entriesByOriginator.erase(nodeAndArea);
    if (entriesByOriginator.empty()) {
      prefixes_.erase(prefix);
    }

    deleteLoopbackPrefix(prefix, nodeName);
    changed.insert(prefix);
  }

  // update prefix entry for new announcement
  for (const auto& prefixEntry : *prefixDb.prefixEntries_ref()) {
    auto& entriesByOriginator = prefixes_[*prefixEntry.prefix_ref()];

    // Skip rest of code, if prefix exists and has no change
    auto [it, inserted] = entriesByOriginator.emplace(nodeAndArea, prefixEntry);
    if (not inserted && it->second == prefixEntry) {
      continue;
    }

    // Update prefix
    if (not inserted) {
      it->second = prefixEntry;
    }
    changed.insert(*prefixEntry.prefix_ref());

    VLOG(1) << "Prefix " << toString(*prefixEntry.prefix_ref())
            << " has been advertised/updated by node " << nodeName
            << " from area " << area;

    // Keep track of loopback addresses (v4 / v6) for each node
    if (thrift::PrefixType::LOOPBACK == *prefixEntry.type_ref()) {
      auto addrSize =
          prefixEntry.prefix_ref()->prefixAddress_ref()->addr_ref()->size();
      if (addrSize == folly::IPAddressV4::byteCount() &&
          folly::IPAddressV4::bitCount() ==
              *prefixEntry.prefix_ref()->prefixLength_ref()) {
        nodeHostLoopbacksV4_[nodeName] =
            *prefixEntry.prefix_ref()->prefixAddress_ref();
      }
      if (addrSize == folly::IPAddressV6::byteCount() &&
          folly::IPAddressV6::bitCount() ==
              *prefixEntry.prefix_ref()->prefixLength_ref()) {
        nodeHostLoopbacksV6_[nodeName] =
            *prefixEntry.prefix_ref()->prefixAddress_ref();
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
  for (auto const& [node, areaToPrefixes] : nodeToPrefixes_) {
    for (auto const& [area, prefixes] : areaToPrefixes) {
      thrift::PrefixDatabase prefixDb;
      *prefixDb.thisNodeName_ref() = node;
      prefixDb.area_ref() = area;
      for (auto const& prefix : prefixes) {
        prefixDb.prefixEntries_ref()->emplace_back(
            prefixes_.at(prefix).at({node, area}));
      }
      prefixDatabases.emplace(node, std::move(prefixDb));
    }
  }
  return prefixDatabases;
}

std::vector<thrift::NextHopThrift>
PrefixState::getLoopbackVias(
    std::unordered_set<std::string> const& nodes, bool const isV4) const {
  std::vector<thrift::NextHopThrift> result;
  result.reserve(nodes.size());
  auto const& hostLoopBacks =
      isV4 ? nodeHostLoopbacksV4_ : nodeHostLoopbacksV6_;
  for (auto const& node : nodes) {
    if (!hostLoopBacks.count(node)) {
      LOG(ERROR) << "No loopback for node " << node;
    } else {
      result.emplace_back(
          createNextHop(hostLoopBacks.at(node), std::nullopt, 0));
    }
  }
  return result;
}

std::vector<thrift::ReceivedRouteDetail>
PrefixState::getReceivedRoutesFiltered(
    thrift::ReceivedRouteFilter const& filter,
    std::string const& myNodeName) const {
  std::vector<thrift::ReceivedRouteDetail> routes;
  if (filter.prefixes_ref()) {
    for (auto& prefix : filter.prefixes_ref().value()) {
      auto it = prefixes_.find(prefix);
      if (it == prefixes_.end()) {
        continue;
      }
      filterAndAddReceivedRoute(
          routes,
          filter.nodeName_ref(),
          filter.areaName_ref(),
          it->first,
          it->second,
          myNodeName);
    }
  } else {
    for (auto& [prefix, prefixEntries] : prefixes_) {
      filterAndAddReceivedRoute(
          routes,
          filter.nodeName_ref(),
          filter.areaName_ref(),
          prefix,
          prefixEntries,
          myNodeName);
    }
  }
  return routes;
}

void
PrefixState::filterAndAddReceivedRoute(
    std::vector<thrift::ReceivedRouteDetail>& routes,
    apache::thrift::optional_field_ref<const std::string&> const& nodeFilter,
    apache::thrift::optional_field_ref<const std::string&> const& areaFilter,
    thrift::IpPrefix const& prefix,
    PrefixEntries const& prefixEntries,
    std::string const& myNodeName) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  thrift::ReceivedRouteDetail routeDetail;
  routeDetail.prefix_ref() = prefix;

  // Add best route selection data
  // TODO: We can save best route computation cycles on CLI invocation by
  // performing it when prefixes are updated and caching the result.
  auto allNodeAreas = selectBestPrefixMetrics(prefixEntries);
  auto bestNodeArea = selectBestNodeArea(allNodeAreas, myNodeName);
  for (auto& [node, area] : allNodeAreas) {
    routeDetail.bestKeys_ref()->emplace_back();
    auto& key = routeDetail.bestKeys_ref()->back();
    key.node_ref() = node;
    key.area_ref() = area;
  }
  thrift::NodeAndArea tBestNodeArea;
  tBestNodeArea.node_ref() = bestNodeArea.first;
  tBestNodeArea.area_ref() = bestNodeArea.second;
  routeDetail.bestKey_ref() = tBestNodeArea;

  // Add prefix entries and honor the filter
  for (auto& [nodeAndArea, prefixEntry] : prefixEntries) {
    if (nodeFilter && *nodeFilter != nodeAndArea.first) {
      continue;
    }
    if (areaFilter && *areaFilter != nodeAndArea.second) {
      continue;
    }
    routeDetail.routes_ref()->emplace_back();
    auto& route = routeDetail.routes_ref()->back();
    route.key_ref()->node_ref() = nodeAndArea.first;
    route.key_ref()->area_ref() = nodeAndArea.second;
    route.route_ref() = prefixEntry;
  }

  // Add detail if there are entries to return
  if (routeDetail.routes_ref()->size()) {
    routes.emplace_back(std::move(routeDetail));
  }
}

} // namespace openr
