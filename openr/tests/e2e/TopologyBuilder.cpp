/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyBuilder.h>

#include <exception>
#include <utility>

#include <fmt/core.h>

namespace openr::e2e {

namespace {

std::pair<std::string, std::string>
orderedPair(const std::string& first, const std::string& second) {
  return first < second ? std::pair{first, second} : std::pair{second, first};
}

NodeSpec
makeNode(std::string name, NodeKind kind, NodeOptions options) {
  NodeSpec node;
  node.name = std::move(name);
  node.kind = kind;
  node.areas = std::move(options.areas);
  node.nodeLabel = options.nodeLabel;
  node.overloaded = options.overloaded;
  node.customizeConfig = std::move(options.customizeConfig);
  return node;
}

} // namespace

TopologyBuilder::TopologyBuilder(std::string name) : name_{std::move(name)} {}

TopologyBuilder&
TopologyBuilder::node(NodeSpec node) {
  if (nodeIndexes_.contains(node.name)) {
    throw TopologyError(fmt::format("duplicate node '{}'", node.name));
  }
  lastNode_ = node.name;
  nodeIndexes_.emplace(node.name, nodes_.size());
  nodes_.push_back(std::move(node));
  return *this;
}

TopologyBuilder&
TopologyBuilder::realNode(std::string name, NodeOptions options) {
  return node(makeNode(std::move(name), NodeKind::REAL, std::move(options)));
}

TopologyBuilder&
TopologyBuilder::router(std::string name, NodeOptions options) {
  return realNode(std::move(name), std::move(options));
}

TopologyBuilder&
TopologyBuilder::syntheticNode(std::string name, NodeOptions options) {
  return node(
      makeNode(std::move(name), NodeKind::SYNTHETIC, std::move(options)));
}

TopologyBuilder&
TopologyBuilder::peer(std::string name, NodeOptions options) {
  return syntheticNode(std::move(name), std::move(options));
}

TopologyBuilder&
TopologyBuilder::prefix(std::string prefix, PrefixOptions options) {
  if (!lastNode_.has_value()) {
    throw TopologyError("prefix requires a preceding node");
  }
  return this->prefix(*lastNode_, std::move(prefix), std::move(options));
}

TopologyBuilder&
TopologyBuilder::prefix(
    const std::string& nodeName, std::string prefix, PrefixOptions options) {
  auto& node = findNode(nodeName);
  std::string area;
  if (options.area.has_value()) {
    area = *options.area;
  } else if (node.areas.size() == 1) {
    area = node.areas.front();
  } else {
    throw TopologyError(
        fmt::format(
            "prefix '{}' on multi-area node '{}' requires an area",
            prefix,
            nodeName));
  }

  folly::CIDRNetwork network;
  try {
    network = folly::IPAddress::createNetwork(prefix);
    network.first = network.first.mask(network.second);
  } catch (const std::exception& error) {
    throw TopologyError(
        fmt::format("invalid prefix '{}': {}", prefix, error.what()));
  }
  node.prefixes.push_back(
      PrefixSpec{
          std::move(network),
          options.type,
          std::move(area),
          std::move(options.tags)});
  return *this;
}

TopologyBuilder&
TopologyBuilder::link(LinkSpec link) {
  ++linkCounts_[orderedPair(link.first.nodeName, link.second.nodeName)];
  links_.push_back(std::move(link));
  return *this;
}

TopologyBuilder&
TopologyBuilder::link(
    const std::string& first,
    const std::string& second,
    const LinkOptions& options) {
  findNode(first);
  findNode(second);
  return link(makeLink(first, second, nextLinkOrdinal(first, second), options));
}

TopologyBuilder&
TopologyBuilder::segment(SegmentSpec segment) {
  segments_.push_back(std::move(segment));
  return *this;
}

TopologyBuilder&
TopologyBuilder::sharedSegment(
    std::string name,
    const std::vector<std::string>& nodes,
    const SegmentOptions& options) {
  for (const auto& node : nodes) {
    findNode(node);
  }
  return segment(makeSharedSegment(std::move(name), nodes, options));
}

Topology
TopologyBuilder::build() {
  return Topology{
      std::move(name_), nodes_, std::move(links_), std::move(segments_)};
}

NodeSpec&
TopologyBuilder::findNode(const std::string& name) {
  const auto it = nodeIndexes_.find(name);
  if (it == nodeIndexes_.end()) {
    throw TopologyError(fmt::format("unknown node '{}'", name));
  }
  return nodes_.at(it->second);
}

size_t
TopologyBuilder::nextLinkOrdinal(
    const std::string& first, const std::string& second) const {
  const auto it = linkCounts_.find(orderedPair(first, second));
  return it == linkCounts_.end() ? 1 : it->second + 1;
}

} // namespace openr::e2e
