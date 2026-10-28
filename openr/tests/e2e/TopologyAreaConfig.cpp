/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyAreaConfig.h>

#include <string>
#include <utility>

#include <fmt/core.h>
#include <folly/container/F14Map.h>
#include <re2/re2.h>

namespace openr::e2e {

std::vector<thrift::AreaConfig>
makeTopologyAreaConfigs(const Topology& topology, const NodeSpec& router) {
  folly::F14FastMap<std::string, std::vector<std::string>> interfaces;
  folly::F14FastMap<std::string, std::vector<std::string>> neighbors;
  const auto add = [&](const LinkEndpointSpec& endpoint,
                       const std::string& neighbor) {
    if (endpoint.nodeName != router.name) {
      return;
    }
    interfaces[endpoint.area].push_back(
        fmt::format("^{}$", RE2::QuoteMeta(endpoint.interfaceName)));
    neighbors[endpoint.area].push_back(
        fmt::format("^{}$", RE2::QuoteMeta(neighbor)));
  };
  for (const auto& link : topology.links()) {
    add(link.first, link.second.nodeName);
    add(link.second, link.first.nodeName);
  }
  for (const auto& segment : topology.segments()) {
    for (const auto& endpoint : segment.endpoints) {
      for (const auto& neighbor : segment.endpoints) {
        if (endpoint.nodeName != neighbor.nodeName) {
          add(endpoint, neighbor.nodeName);
        }
      }
    }
  }

  std::vector<thrift::AreaConfig> areas;
  for (const auto& name : router.areas) {
    thrift::AreaConfig area;
    area.area_id() = name;
    area.include_interface_regexes() = interfaces[name].empty()
        ? std::vector<std::string>{"^$"}
        : std::move(interfaces[name]);
    area.neighbor_regexes() = neighbors[name].empty()
        ? std::vector<std::string>{"^$"}
        : std::move(neighbors[name]);
    areas.push_back(std::move(area));
  }
  return areas;
}

std::vector<thrift::InterfaceMetricStatic>
makeTopologyInterfaceMetrics(const Topology& topology, const NodeSpec& router) {
  std::vector<thrift::InterfaceMetricStatic> metrics;
  const auto add = [&](const LinkEndpointSpec& endpoint) {
    if (endpoint.nodeName != router.name) {
      return;
    }
    thrift::InterfaceMetricStatic metric;
    metric.interface_regexes() = {
        fmt::format("^{}$", RE2::QuoteMeta(endpoint.interfaceName))};
    metric.metric() = endpoint.metric;
    metrics.push_back(std::move(metric));
  };
  for (const auto& link : topology.links()) {
    add(link.first);
    add(link.second);
  }
  for (const auto& segment : topology.segments()) {
    for (const auto& endpoint : segment.endpoints) {
      add(endpoint);
    }
  }
  return metrics;
}

} // namespace openr::e2e
