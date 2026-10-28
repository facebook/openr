/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyAllocator.h>

#include <limits>
#include <utility>

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <openr/tests/e2e/TopologyAddressAllocator.h>

namespace openr::e2e {

namespace {

using IntSet = folly::F14FastSet<int32_t>;
using PerNodeInts = folly::F14FastMap<std::string, IntSet>;

int32_t
takeNext(IntSet& used, int32_t candidate) {
  while (used.contains(candidate)) {
    if (candidate == std::numeric_limits<int32_t>::max()) {
      throw TopologyError("deterministic allocation exhausted int32 values");
    }
    ++candidate;
  }
  used.emplace(candidate);
  return candidate;
}

void
reserveEndpoint(
    const LinkEndpointSpec& endpoint,
    PerNodeInts& indexes,
    PerNodeInts& labels) {
  if (endpoint.interfaceIndex.has_value() &&
      !indexes[endpoint.nodeName].emplace(*endpoint.interfaceIndex).second) {
    throw TopologyError("duplicate explicit interface index");
  }
  if (endpoint.adjacencyLabel.has_value() &&
      !labels[endpoint.nodeName].emplace(*endpoint.adjacencyLabel).second) {
    throw TopologyError("duplicate explicit adjacency label");
  }
}

void
allocateEndpoint(
    LinkEndpointSpec& endpoint,
    size_t nodeOrdinal,
    int32_t firstLabel,
    PerNodeInts& indexes,
    PerNodeInts& labels) {
  if (!endpoint.interfaceIndex.has_value()) {
    endpoint.interfaceIndex = takeNext(indexes[endpoint.nodeName], 1);
  }
  if (!endpoint.adjacencyLabel.has_value()) {
    if (nodeOrdinal >
        static_cast<size_t>(
            (std::numeric_limits<int32_t>::max() - firstLabel) / 1024)) {
      throw TopologyError("deterministic adjacency labels exhausted");
    }
    const auto candidate =
        firstLabel + static_cast<int32_t>(nodeOrdinal * 1024);
    endpoint.adjacencyLabel = takeNext(labels[endpoint.nodeName], candidate);
  }
}

} // namespace

Topology
TopologyAllocator::allocate(
    const Topology& topology, AllocationOptions options) {
  if (options.firstNodeLabel <= 0 || options.firstAdjacencyLabel <= 0) {
    throw TopologyError("allocation label ranges must be positive");
  }
  std::vector<NodeSpec> nodes;
  nodes.reserve(topology.nodeCount());
  folly::F14FastMap<std::string, size_t> ordinals;
  IntSet nodeLabels;
  for (const auto& name : topology.nodeNames()) {
    auto node = topology.node(name);
    ordinals.emplace(name, nodes.size());
    if (node.nodeLabel.has_value() &&
        !nodeLabels.emplace(*node.nodeLabel).second) {
      throw TopologyError("duplicate explicit node label");
    }
    nodes.push_back(std::move(node));
  }
  for (auto& node : nodes) {
    if (!node.nodeLabel.has_value()) {
      node.nodeLabel = takeNext(nodeLabels, options.firstNodeLabel);
    }
  }

  auto links = topology.links();
  auto segments = topology.segments();
  PerNodeInts indexes;
  PerNodeInts labels;
  for (const auto& link : links) {
    reserveEndpoint(link.first, indexes, labels);
    reserveEndpoint(link.second, indexes, labels);
  }
  for (const auto& segment : segments) {
    for (const auto& endpoint : segment.endpoints) {
      reserveEndpoint(endpoint, indexes, labels);
    }
  }
  const auto allocate = [&](LinkEndpointSpec& endpoint) {
    allocateEndpoint(
        endpoint,
        ordinals.at(endpoint.nodeName),
        options.firstAdjacencyLabel,
        indexes,
        labels);
  };
  size_t subnet{1};
  for (auto& link : links) {
    allocate(link.first);
    allocate(link.second);
    allocateLinkAddresses(link, subnet);
    ++subnet;
  }
  for (auto& segment : segments) {
    for (auto& endpoint : segment.endpoints) {
      allocate(endpoint);
    }
    allocateSegmentAddresses(segment, subnet);
    ++subnet;
  }
  return Topology{
      topology.name(), nodes, std::move(links), std::move(segments)};
}

} // namespace openr::e2e
