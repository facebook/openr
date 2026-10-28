/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/Topology.h>

#include <algorithm>
#include <utility>

#include <fmt/core.h>
#include <openr/tests/e2e/TopologyConnectionValidation.h>
#include <openr/tests/e2e/TopologyValidation.h>

namespace openr::e2e {

Topology::Topology(
    std::string name,
    const std::vector<NodeSpec>& nodes,
    std::vector<LinkSpec> links,
    std::vector<SegmentSpec> segments)
    : name_{std::move(name)},
      links_{std::move(links)},
      segments_{std::move(segments)} {
  nodeNames_.reserve(nodes.size());
  nodes_.reserve(nodes.size());
  for (const auto& node : nodes) {
    const auto nodeName = node.name;
    if (!nodes_.emplace(nodeName, node).second) {
      throw TopologyError(fmt::format("duplicate node '{}'", nodeName));
    }
    nodeNames_.push_back(nodeName);
  }
  validateTopology(name_, nodes_);
  validateConnections(nodes_, links_, segments_);
}

const NodeSpec&
Topology::node(const std::string& nodeName) const {
  const auto it = nodes_.find(nodeName);
  if (it == nodes_.end()) {
    throw TopologyError(fmt::format("unknown node '{}'", nodeName));
  }
  return it->second;
}

const LinkSpec&
Topology::link(const std::string& linkName) const {
  const auto it =
      std::find_if(links_.begin(), links_.end(), [&](const auto& link) {
        return link.name == linkName;
      });
  if (it == links_.end()) {
    throw TopologyError(fmt::format("unknown link '{}'", linkName));
  }
  return *it;
}

const SegmentSpec&
Topology::segment(const std::string& segmentName) const {
  const auto it = std::find_if(
      segments_.begin(), segments_.end(), [&](const auto& segment) {
        return segment.name == segmentName;
      });
  if (it == segments_.end()) {
    throw TopologyError(fmt::format("unknown segment '{}'", segmentName));
  }
  return *it;
}

size_t
Topology::realNodeCount() const {
  return std::count_if(nodes_.begin(), nodes_.end(), [](const auto& entry) {
    return entry.second.kind == NodeKind::REAL;
  });
}

size_t
Topology::prefixCount() const {
  size_t count{0};
  for (const auto& [_, node] : nodes_) {
    count += node.prefixes.size();
  }
  return count;
}

} // namespace openr::e2e
