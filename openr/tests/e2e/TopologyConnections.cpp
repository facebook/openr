/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyConnections.h>

#include <utility>

#include <fmt/core.h>

namespace openr::e2e {

namespace {

LinkEndpointSpec
makeEndpoint(
    const std::string& node,
    const std::string& peer,
    size_t ordinal,
    const LinkOptions& link,
    const LinkEndpointOptions& endpoint) {
  LinkEndpointSpec result;
  result.nodeName = node;
  result.interfaceName = endpoint.interfaceName.value_or(
      fmt::format("if_{}_{}_{}", node, peer, ordinal));
  result.interfaceIndex = endpoint.interfaceIndex;
  result.addresses = endpoint.addresses;
  result.area = endpoint.area.value_or(link.area);
  result.metric = endpoint.metric.value_or(link.metric);
  result.latency = endpoint.latency.value_or(link.latency);
  result.overloaded = endpoint.overloaded.value_or(link.overloaded);
  result.adjacencyLabel = endpoint.adjacencyLabel.has_value()
      ? endpoint.adjacencyLabel
      : link.adjacencyLabel;
  return result;
}

} // namespace

LinkSpec
makeLink(
    const std::string& firstNode,
    const std::string& secondNode,
    size_t ordinal,
    const LinkOptions& options) {
  LinkSpec link;
  if (options.name.empty()) {
    link.name = fmt::format("link_{}_{}_{}", firstNode, secondNode, ordinal);
  } else {
    link.name = options.name;
  }
  link.first =
      makeEndpoint(firstNode, secondNode, ordinal, options, options.first);
  link.second =
      makeEndpoint(secondNode, firstNode, ordinal, options, options.second);
  return link;
}

SegmentSpec
makeSharedSegment(
    std::string name,
    const std::vector<std::string>& nodeNames,
    const SegmentOptions& options) {
  SegmentSpec segment;
  segment.name = std::move(name);
  segment.endpoints.reserve(nodeNames.size());
  for (const auto& node : nodeNames) {
    LinkEndpointSpec endpoint;
    endpoint.nodeName = node;
    endpoint.interfaceName = fmt::format("if_{}_{}", node, segment.name);
    endpoint.area = options.area;
    endpoint.metric = options.metric;
    endpoint.latency = options.latency;
    endpoint.overloaded = options.overloaded;
    endpoint.adjacencyLabel = options.adjacencyLabel;
    segment.endpoints.push_back(std::move(endpoint));
  }
  return segment;
}

} // namespace openr::e2e
