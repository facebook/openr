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

std::unordered_set<thrift::IpPrefix>
PrefixState::updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb) {
  std::unordered_set<thrift::IpPrefix> changed;

  auto const nodeAndArea =
      std::make_pair(*prefixDb.thisNodeName_ref(), *prefixDb.area_ref());
  auto const& nodeName = *prefixDb.thisNodeName_ref();
  auto const& area = *prefixDb.area_ref();

  // Get reference existing set or create new one
  auto& newPrefixSet = nodeToPrefixes_[nodeAndArea];

  // Create copy of existing prefix set - NOTE explicit copy
  const std::set<thrift::IpPrefix> oldPrefixSet = newPrefixSet;

  // update the entry
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

    // Update prefix
    auto& entriesByOriginator = prefixes_.at(prefix);
    entriesByOriginator.erase(nodeAndArea);
    if (entriesByOriginator.empty()) {
      prefixes_.erase(prefix);
    }
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
  }

  if (newPrefixSet.empty()) {
    nodeToPrefixes_.erase(nodeAndArea);
  }

  // TODO: check reference count threshold for local originiated prefixes

  return changed;
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
PrefixState::getPrefixDatabases() const {
  std::unordered_map<std::string, thrift::PrefixDatabase> prefixDatabases;
  for (auto const& [nodeAndArea, prefixes] : nodeToPrefixes_) {
    thrift::PrefixDatabase prefixDb;
    *prefixDb.thisNodeName_ref() = nodeAndArea.first;
    prefixDb.area_ref() = nodeAndArea.second;
    for (auto const& prefix : prefixes) {
      prefixDb.prefixEntries_ref()->emplace_back(
          prefixes_.at(prefix).at(nodeAndArea));
    }
    prefixDatabases.emplace(nodeAndArea.first, std::move(prefixDb));
  }
  return prefixDatabases;
}

std::vector<thrift::ReceivedRouteDetail>
PrefixState::getReceivedRoutesFiltered(
    thrift::ReceivedRouteFilter const& filter) const {
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
          it->second);
    }
  } else {
    for (auto& [prefix, prefixEntries] : prefixes_) {
      filterAndAddReceivedRoute(
          routes,
          filter.nodeName_ref(),
          filter.areaName_ref(),
          prefix,
          prefixEntries);
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
    PrefixEntries const& prefixEntries) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  thrift::ReceivedRouteDetail routeDetail;
  routeDetail.prefix_ref() = prefix;

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

bool
PrefixState::hasConflictingForwardingInfo(const PrefixEntries& prefixEntries) {
  // Empty prefix entries doesn't indicate conflicting information
  if (prefixEntries.empty()) {
    return false;
  }

  // There is at-least one entry, get its reference
  const auto& firstEntry = prefixEntries.begin()->second;

  // Iterate over all entries and make sure the forwarding information agrees
  for (auto& [_, entry] : prefixEntries) {
    if (firstEntry.forwardingAlgorithm_ref() !=
        entry.forwardingAlgorithm_ref()) {
      return true;
    }
    if (firstEntry.forwardingType_ref() != entry.forwardingType_ref()) {
      return true;
    }
  }

  // No conflicting information
  return false;
}

} // namespace openr
