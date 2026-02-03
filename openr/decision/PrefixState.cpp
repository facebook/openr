/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/container/F14Set.h>
#include <folly/logging/xlog.h>

#include <openr/common/LsdbUtil.h>
#include <openr/decision/PrefixState.h>

namespace openr {

folly::F14FastSet<folly::CIDRNetwork>
PrefixState::updatePrefix(
    PrefixKey const& key, thrift::PrefixEntry const& entry) {
  folly::F14FastSet<folly::CIDRNetwork> changed;

  auto [it, inserted] = prefixes_[key.getCIDRNetwork()].emplace(
      key.getNodeAndArea(), std::make_shared<thrift::PrefixEntry>(entry));

  // Skip rest of code, if prefix exists and has no change
  if (!inserted && *it->second == entry) {
    return changed;
  }
  // Update prefix
  if (!inserted) {
    it->second = std::make_shared<thrift::PrefixEntry>(entry);
  }
  changed.insert(key.getCIDRNetwork());

  XLOG(DBG1) << "[ROUTE ADVERTISEMENT] " << "Area: " << key.getPrefixArea()
             << ", Node: " << key.getNodeName() << ", "
             << toString(entry, VLOG_IS_ON(1));
  return changed;
}

folly::F14FastSet<folly::CIDRNetwork>
PrefixState::deletePrefix(PrefixKey const& key) {
  folly::F14FastSet<folly::CIDRNetwork> changed;

  auto search = prefixes_.find(key.getCIDRNetwork());
  if (search != prefixes_.end() && search->second.erase(key.getNodeAndArea())) {
    changed.insert(key.getCIDRNetwork());
    XLOG(DBG1) << "[ROUTE WITHDRAW] " << "Area: " << key.getPrefixArea()
               << ", Node: " << key.getNodeName() << ", "
               << folly::IPAddress::networkToString(key.getCIDRNetwork());
    // clean up data structures
    if (search->second.empty()) {
      prefixes_.erase(search);
    }
  }
  return changed;
}

std::vector<thrift::ReceivedRouteDetail>
PrefixState::getReceivedRoutesFiltered(
    thrift::ReceivedRouteFilter const& filter) const {
  std::vector<thrift::ReceivedRouteDetail> routes;
  if (filter.prefixes()) {
    for (auto& prefix : filter.prefixes().value()) {
      auto it = prefixes_.find(toIPNetwork(prefix));
      if (it == prefixes_.end()) {
        continue;
      }
      filterAndAddReceivedRoute(
          routes, filter.nodeName(), filter.areaName(), it->first, it->second);
    }
  } else {
    for (auto& [prefix, prefixEntries] : prefixes_) {
      filterAndAddReceivedRoute(
          routes, filter.nodeName(), filter.areaName(), prefix, prefixEntries);
    }
  }
  return routes;
}

void
PrefixState::filterAndAddReceivedRoute(
    std::vector<thrift::ReceivedRouteDetail>& routes,
    apache::thrift::optional_field_ref<const std::string&> const& nodeFilter,
    apache::thrift::optional_field_ref<const std::string&> const& areaFilter,
    folly::CIDRNetwork const& prefix,
    PrefixEntries const& prefixEntries) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  thrift::ReceivedRouteDetail routeDetail;
  routeDetail.prefix() = toIpPrefix(prefix);

  // Add prefix entries and honor the filter
  for (auto& [nodeAndArea, prefixEntry] : prefixEntries) {
    if (nodeFilter && *nodeFilter != nodeAndArea.first) {
      continue;
    }
    if (areaFilter && *areaFilter != nodeAndArea.second) {
      continue;
    }
    routeDetail.routes()->emplace_back();
    auto& route = routeDetail.routes()->back();
    route.key()->node() = nodeAndArea.first;
    route.key()->area() = nodeAndArea.second;
    route.route() = *prefixEntry;
  }

  // Add detail if there are entries to return
  if (routeDetail.routes()->size()) {
    routes.emplace_back(std::move(routeDetail));
  }
}

} // namespace openr
