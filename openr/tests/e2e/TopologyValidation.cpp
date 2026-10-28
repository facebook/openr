/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyValidation.h>

#include <algorithm>

#include <fmt/core.h>
#include <folly/container/F14Set.h>

namespace openr::e2e {

void
validateNetwork(
    const folly::CIDRNetwork& network,
    const std::string& owner,
    bool requireCanonical) {
  if (network.first.empty() || network.second > network.first.bitCount()) {
    throw TopologyError(fmt::format("{} has an invalid network", owner));
  }
  if (requireCanonical && network.first != network.first.mask(network.second)) {
    throw TopologyError(
        fmt::format("{} is not a canonical network prefix", owner));
  }
}

void
validateTopology(
    const std::string& name,
    const folly::F14FastMap<std::string, NodeSpec>& nodes) {
  if (name.empty() || nodes.empty()) {
    throw TopologyError("topology name and node set must not be empty");
  }

  for (const auto& [_, node] : nodes) {
    if (node.name.empty() || node.areas.empty()) {
      throw TopologyError("node name and area set must not be empty");
    }
    if (node.nodeLabel.has_value() && *node.nodeLabel <= 0) {
      throw TopologyError(
          fmt::format("node '{}' has a non-positive label", node.name));
    }

    folly::F14FastSet<std::string> areas;
    for (const auto& area : node.areas) {
      if (area.empty() || !areas.emplace(area).second) {
        throw TopologyError(
            fmt::format("node '{}' has an empty or repeated area", node.name));
      }
    }

    folly::F14FastSet<std::string> prefixKeys;
    for (const auto& prefix : node.prefixes) {
      validateNetwork(
          prefix.prefix, fmt::format("prefix on node '{}'", node.name), true);
      const auto text = folly::IPAddress::networkToString(prefix.prefix);
      if (!areas.contains(prefix.area)) {
        throw TopologyError(
            fmt::format(
                "prefix '{}' uses an unknown area on node '{}'",
                text,
                node.name));
      }
      if (!prefixKeys.emplace(fmt::format("{}:{}", prefix.area, text)).second) {
        throw TopologyError(
            fmt::format("node '{}' repeats prefix '{}'", node.name, text));
      }
      folly::F14FastSet<std::string> tags;
      for (const auto& tag : prefix.tags) {
        if (tag.empty() || !tags.emplace(tag).second) {
          throw TopologyError(
              fmt::format(
                  "prefix '{}' on node '{}' has an invalid tag",
                  text,
                  node.name));
        }
      }
    }
  }
}

} // namespace openr::e2e
