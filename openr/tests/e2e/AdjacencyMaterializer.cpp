/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/AdjacencyMaterializer.h>

#include <algorithm>
#include <limits>

#include <fmt/core.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/Util.h>

namespace openr::e2e {

namespace {

const LinkEndpointSpec&
endpointFor(const LinkSpec& link, const std::string& node) {
  return link.first.nodeName == node ? link.first : link.second;
}

std::string
addressFor(const LinkEndpointSpec& endpoint, bool v4) {
  for (const auto& [address, _] : endpoint.addresses) {
    if (address.isV4() == v4) {
      return address.str();
    }
  }
  throw TopologyError(
      "materialization requires allocated IPv4 and IPv6 addresses");
}

thrift::Adjacency
adjacency(
    const NodeSpec& neighbor,
    const LinkEndpointSpec& local,
    const LinkEndpointSpec& remote) {
  if (!local.adjacencyLabel.has_value() || !neighbor.nodeLabel.has_value()) {
    throw TopologyError("materialization requires allocated labels");
  }
  const auto localLatency = local.latency.count();
  const auto remoteLatency = remote.latency.count();
  if (localLatency > std::numeric_limits<int64_t>::max() - remoteLatency) {
    throw TopologyError("link latency exceeds the adjacency RTT range");
  }
  const auto roundTripLatency = localLatency + remoteLatency;
  if (roundTripLatency > std::numeric_limits<int32_t>::max() / 1000) {
    throw TopologyError("link latency exceeds the adjacency RTT range");
  }
  const auto rtt = roundTripLatency * 1000;
  return createThriftAdjacency(
      neighbor.name,
      local.interfaceName,
      addressFor(remote, false),
      addressFor(remote, true),
      local.metric,
      *local.adjacencyLabel,
      local.overloaded,
      static_cast<int32_t>(rtt),
      0,
      1,
      remote.interfaceName);
}

std::vector<thrift::Adjacency>
adjacenciesFor(
    const Topology& topology, const NodeSpec& node, const std::string& area) {
  std::vector<thrift::Adjacency> adjacencies;
  if (!node.operational) {
    return adjacencies;
  }
  for (const auto& link : topology.links()) {
    if (!link.operational ||
        (link.first.nodeName != node.name &&
         link.second.nodeName != node.name)) {
      continue;
    }
    const auto& local = endpointFor(link, node.name);
    const auto& remote = endpointFor(
        link,
        link.first.nodeName == node.name ? link.second.nodeName
                                         : link.first.nodeName);
    const auto& neighbor = topology.node(remote.nodeName);
    if (local.area == area && remote.area == area && neighbor.operational) {
      adjacencies.push_back(adjacency(neighbor, local, remote));
    }
  }
  for (const auto& segment : topology.segments()) {
    const auto local = std::find_if(
        segment.endpoints.begin(),
        segment.endpoints.end(),
        [&](const auto& ep) {
          return ep.nodeName == node.name && ep.area == area;
        });
    if (local == segment.endpoints.end()) {
      continue;
    }
    for (const auto& remote : segment.endpoints) {
      const auto& neighbor = topology.node(remote.nodeName);
      if (remote.nodeName != node.name && remote.area == area &&
          neighbor.operational) {
        adjacencies.push_back(adjacency(neighbor, *local, remote));
      }
    }
  }
  return adjacencies;
}

} // namespace

std::vector<SerializedKvStoreKey>
materializeAdjacencies(
    const Topology& topology,
    const folly::F14FastSet<std::string>& selectedNodes) {
  apache::thrift::CompactSerializer serializer;
  std::vector<SerializedKvStoreKey> keys;
  for (const auto& name : topology.nodeNames()) {
    const auto& node = topology.node(name);
    if (node.kind == NodeKind::REAL ||
        (!selectedNodes.empty() && !selectedNodes.contains(name))) {
      continue;
    }
    if (!node.nodeLabel.has_value()) {
      throw TopologyError("materialization requires allocated node labels");
    }
    for (const auto& area : node.areas) {
      const auto database = createAdjDb(
          name,
          adjacenciesFor(topology, node, area),
          *node.nodeLabel,
          node.overloaded || !node.operational,
          area);
      keys.push_back(
          {{area, fmt::format("adj:{}", name)},
           name,
           writeThriftObjStr(database, serializer),
           std::nullopt});
    }
  }
  return keys;
}

} // namespace openr::e2e
