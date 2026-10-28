/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyMutator.h>

#include <algorithm>
#include <type_traits>
#include <utility>

#include <fmt/core.h>

namespace openr::e2e {

namespace {

NodeSpec&
node(std::vector<NodeSpec>& nodes, const std::string& name) {
  const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto& n) {
    return n.name == name;
  });
  if (it == nodes.end()) {
    throw TopologyError(fmt::format("unknown node '{}'", name));
  }
  return *it;
}

LinkSpec&
link(std::vector<LinkSpec>& links, const std::string& name) {
  const auto it = std::find_if(links.begin(), links.end(), [&](const auto& l) {
    return l.name == name;
  });
  if (it == links.end()) {
    throw TopologyError(fmt::format("unknown link '{}'", name));
  }
  return *it;
}

template <typename F>
void
updateEndpoints(LinkSpec& link, const std::optional<std::string>& node, F fn) {
  if (!node.has_value()) {
    fn(link.first);
    fn(link.second);
  } else if (link.first.nodeName == *node) {
    fn(link.first);
  } else if (link.second.nodeName == *node) {
    fn(link.second);
  } else {
    throw TopologyError(
        fmt::format("node '{}' is not on link '{}'", *node, link.name));
  }
}

} // namespace

TopologyChangeSet
TopologyMutator::apply(
    const Topology& topology, const std::vector<TopologyMutation>& mutations) {
  std::vector<NodeSpec> nodes;
  for (const auto& name : topology.nodeNames()) {
    nodes.push_back(topology.node(name));
  }
  auto links = topology.links();
  auto segments = topology.segments();
  folly::F14FastSet<std::string> affected;
  const auto mark = [&](const std::string& name) { affected.emplace(name); };
  const auto markLink = [&](const LinkSpec& changed) {
    mark(changed.first.nodeName);
    mark(changed.second.nodeName);
  };

  for (const auto& mutation : mutations) {
    std::visit(
        [&](const auto& change) {
          using T = std::decay_t<decltype(change)>;
          if constexpr (std::is_same_v<T, SetLinkState>) {
            auto& item = link(links, change.link);
            item.operational = change.operational;
            markLink(item);
          } else if constexpr (std::is_same_v<T, SetLinkMetric>) {
            auto& item = link(links, change.link);
            updateEndpoints(item, change.node, [&](auto& ep) {
              ep.metric = change.metric;
              mark(ep.nodeName);
            });
          } else if constexpr (std::is_same_v<T, SetLinkLatency>) {
            auto& item = link(links, change.link);
            updateEndpoints(item, change.node, [&](auto& ep) {
              ep.latency = change.latency;
              mark(ep.nodeName);
            });
          } else if constexpr (std::is_same_v<T, SetLinkOverload>) {
            auto& item = link(links, change.link);
            updateEndpoints(item, change.node, [&](auto& ep) {
              ep.overloaded = change.overloaded;
              mark(ep.nodeName);
            });
          } else if constexpr (std::is_same_v<T, SetNodeState>) {
            node(nodes, change.node).operational = change.operational;
            mark(change.node);
            for (const auto& item : links) {
              if (item.first.nodeName == change.node) {
                mark(item.second.nodeName);
              } else if (item.second.nodeName == change.node) {
                mark(item.first.nodeName);
              }
            }
            for (const auto& segment : segments) {
              const auto attached = std::any_of(
                  segment.endpoints.begin(),
                  segment.endpoints.end(),
                  [&](const auto& endpoint) {
                    return endpoint.nodeName == change.node;
                  });
              if (attached) {
                for (const auto& endpoint : segment.endpoints) {
                  if (endpoint.nodeName != change.node) {
                    mark(endpoint.nodeName);
                  }
                }
              }
            }
          } else if constexpr (std::is_same_v<T, SetNodeOverload>) {
            node(nodes, change.node).overloaded = change.overloaded;
            mark(change.node);
          } else if constexpr (std::is_same_v<T, AdvertisePrefix>) {
            node(nodes, change.node).prefixes.push_back(change.prefix);
            mark(change.node);
          } else if constexpr (std::is_same_v<T, WithdrawPrefix>) {
            auto& prefixes = node(nodes, change.node).prefixes;
            const auto it = std::find_if(
                prefixes.begin(), prefixes.end(), [&](const auto& prefix) {
                  return prefix.prefix == change.prefix &&
                      prefix.area == change.area && prefix.type == change.type;
                });
            if (it == prefixes.end()) {
              throw TopologyError("prefix withdrawal did not match a prefix");
            }
            prefixes.erase(it);
            mark(change.node);
          } else if constexpr (std::is_same_v<T, SetPartition>) {
            for (const auto& name : change.links) {
              auto& item = link(links, name);
              item.operational = !change.partitioned;
              markLink(item);
            }
          }
        },
        mutation);
  }
  return {
      Topology{topology.name(), nodes, std::move(links), std::move(segments)},
      std::move(affected)};
}

} // namespace openr::e2e
