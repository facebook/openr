/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/DirectMutationPlanner.h>

#include <algorithm>
#include <utility>

#include <folly/container/F14Set.h>

namespace openr::e2e {

namespace {

void
addIfReal(
    std::vector<DirectAction>& actions,
    const Topology& topology,
    const std::string& node,
    TopologyMutation mutation) {
  if (topology.node(node).kind == NodeKind::REAL) {
    actions.push_back({DirectActionTarget::NODE, node, std::move(mutation)});
  }
}

void
addVirtualNetworkAction(
    std::vector<DirectAction>& actions,
    const Topology& topology,
    const LinkSpec& link,
    TopologyMutation mutation,
    const std::string& direction) {
  if (topology.node(link.first.nodeName).kind == NodeKind::REAL ||
      topology.node(link.second.nodeName).kind == NodeKind::REAL) {
    actions.push_back(
        {DirectActionTarget::VIRTUAL_NETWORK, direction, std::move(mutation)});
  }
}

bool
samePrefix(const PrefixSpec& first, const PrefixSpec& second) {
  return first.prefix == second.prefix && first.area == second.area &&
      first.type == second.type;
}

void
planPrefixes(
    std::vector<DirectAction>& actions,
    const NodeSpec& before,
    const NodeSpec& after) {
  if (after.kind != NodeKind::REAL) {
    return;
  }
  for (const auto& prefix : after.prefixes) {
    const auto old = std::find_if(
        before.prefixes.begin(), before.prefixes.end(), [&](const auto& item) {
          return samePrefix(item, prefix);
        });
    if (old == before.prefixes.end() || old->tags != prefix.tags) {
      actions.push_back(
          {DirectActionTarget::NODE,
           after.name,
           AdvertisePrefix{after.name, prefix}});
    }
  }
  for (const auto& prefix : before.prefixes) {
    const auto current = std::find_if(
        after.prefixes.begin(), after.prefixes.end(), [&](const auto& item) {
          return samePrefix(item, prefix);
        });
    if (current == after.prefixes.end()) {
      actions.push_back(
          {DirectActionTarget::NODE,
           after.name,
           WithdrawPrefix{
               after.name, prefix.prefix, prefix.area, prefix.type}});
    }
  }
}

void
notifyRealNeighbors(
    std::vector<DirectAction>& actions,
    const Topology& topology,
    const SetNodeState& change) {
  folly::F14FastSet<std::string> notified;
  const auto notify = [&](const std::string& node) {
    if (notified.emplace(node).second) {
      addIfReal(actions, topology, node, change);
    }
  };
  for (const auto& link : topology.links()) {
    if (link.first.nodeName == change.node) {
      notify(link.second.nodeName);
    } else if (link.second.nodeName == change.node) {
      notify(link.first.nodeName);
    }
  }
  for (const auto& segment : topology.segments()) {
    const auto attached = std::any_of(
        segment.endpoints.begin(),
        segment.endpoints.end(),
        [&](const auto& ep) { return ep.nodeName == change.node; });
    if (attached) {
      for (const auto& endpoint : segment.endpoints) {
        if (endpoint.nodeName != change.node) {
          notify(endpoint.nodeName);
        }
      }
    }
  }
}

void
planEndpoint(
    std::vector<DirectAction>& actions,
    const Topology& topology,
    const LinkSpec& link,
    const LinkEndpointSpec& before,
    const LinkEndpointSpec& after) {
  if (before.metric != after.metric) {
    addIfReal(
        actions,
        topology,
        after.nodeName,
        SetLinkMetric{link.name, after.nodeName, after.metric});
  }
  if (before.latency != after.latency) {
    addVirtualNetworkAction(
        actions,
        topology,
        link,
        SetLinkLatency{link.name, after.nodeName, after.latency},
        after.nodeName);
  }
  if (before.overloaded != after.overloaded) {
    addIfReal(
        actions,
        topology,
        after.nodeName,
        SetLinkOverload{link.name, after.nodeName, after.overloaded});
  }
}

} // namespace

std::vector<DirectAction>
planDirectMutations(const Topology& before, const Topology& after) {
  std::vector<DirectAction> actions;
  for (const auto& name : before.nodeNames()) {
    const auto& oldNode = before.node(name);
    const auto& newNode = after.node(name);
    if (oldNode.operational != newNode.operational) {
      const SetNodeState change{name, newNode.operational};
      addIfReal(actions, after, name, change);
      if (newNode.kind == NodeKind::SYNTHETIC) {
        notifyRealNeighbors(actions, after, change);
      }
    }
    if (oldNode.overloaded != newNode.overloaded) {
      addIfReal(
          actions, after, name, SetNodeOverload{name, newNode.overloaded});
    }
    planPrefixes(actions, oldNode, newNode);
  }
  for (const auto& oldLink : before.links()) {
    const auto& newLink = after.link(oldLink.name);
    if (oldLink.operational != newLink.operational) {
      const SetLinkState change{newLink.name, newLink.operational};
      addIfReal(actions, after, newLink.first.nodeName, change);
      addIfReal(actions, after, newLink.second.nodeName, change);
    }
    planEndpoint(actions, after, newLink, oldLink.first, newLink.first);
    planEndpoint(actions, after, newLink, oldLink.second, newLink.second);
  }
  return actions;
}

} // namespace openr::e2e
