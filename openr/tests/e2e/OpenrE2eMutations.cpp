/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrE2eInternal.h>

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <folly/IPAddress.h>

#include <openr/tests/e2e/TopologyMaterializer.h>
#include <openr/tests/e2e/TopologyMutator.h>
#include <openr/tests/mocks/MockIoProvider.h>

namespace openr::e2e {

void
OpenrE2eLab::Impl::advertisePrefix(
    const std::string& name, const std::string& cidr, PrefixOptions options) {
  requireStarted();
  if (!topology_) {
    if (options.area || !options.tags.empty() ||
        options.type != thrift::PrefixType::LOOPBACK) {
      throw std::invalid_argument("Prefix options require a topology lab");
    }
    peer(name).synthetic->advertisePrefix(cidr);
    return;
  }
  const auto& node = topology_->node(name);
  if (node.kind == NodeKind::REAL) {
    throw std::invalid_argument("Use advertiseLocalPrefix for the real router");
  }
  mutateTopology(
      AdvertisePrefix{
          name,
          PrefixSpec{
              folly::IPAddress::createNetwork(cidr),
              options.type,
              options.area.value_or(node.areas.front()),
              std::move(options.tags)}});
}

void
OpenrE2eLab::Impl::withdrawPrefix(
    const std::string& name,
    const std::string& cidr,
    const PrefixOptions& options) {
  requireStarted();
  if (!topology_) {
    if (options.area || !options.tags.empty() ||
        options.type != thrift::PrefixType::LOOPBACK) {
      throw std::invalid_argument("Prefix options require a topology lab");
    }
    peer(name).synthetic->withdrawPrefix(cidr);
    return;
  }
  const auto& node = topology_->node(name);
  if (node.kind == NodeKind::REAL) {
    throw std::invalid_argument("Use withdrawLocalPrefix for the real router");
  }
  mutateTopology(
      WithdrawPrefix{
          name,
          folly::IPAddress::createNetwork(cidr),
          options.area.value_or(node.areas.front()),
          options.type});
}

void
OpenrE2eLab::Impl::setLinkUp(const std::string& name, bool isUp) {
  requireStarted();
  const auto& p = peer(name);
  if (topology_ && !p.linkName.empty()) {
    mutateTopology(SetLinkState{p.linkName, isUp});
    return;
  }
  platform_.setLink(p.dutIfName, p.dutIfIndex, isUp);
}

void
OpenrE2eLab::Impl::mutateTopology(TopologyMutation change) {
  requireStarted();
  if (!topology_) {
    throw std::logic_error("Named topology changes require a topology lab");
  }
  std::vector<TopologyMutation> changes;
  changes.emplace_back(std::move(change));
  auto result = TopologyMutator::apply(*topology_, changes);
  TopologyMaterializer materializer{topologyVersions_};
  const auto updates =
      materializer.diff(*topology_, result.topology, result.affectedNodes);

  if (const auto* metric = std::get_if<SetLinkMetric>(&changes.front());
      metric && metric->node &&
      topology_->node(*metric->node).kind == NodeKind::REAL) {
    const auto& link = result.topology.link(metric->link);
    const auto& endpoint =
        link.first.nodeName == *metric->node ? link.first : link.second;
    controlClient_->sync_setInterfaceMetric(
        endpoint.interfaceName, metric->metric);
  }

  bool connectionsChanged{false};
  for (const auto& link : topology_->links()) {
    const auto& next = result.topology.link(link.name);
    if (link.operational == next.operational) {
      continue;
    }
    for (auto& [_, peer] : peers_) {
      if (peer.linkName == link.name) {
        peer.linkUp = next.operational;
        peer.synthetic->setOperational(peer.linkUp);
        connectionsChanged = true;
      }
    }
  }
  if (connectionsChanged) {
    ConnectedIfPairs links;
    std::map<std::string, bool> interfaceUp;
    for (const auto& [_, peer] : peers_) {
      interfaceUp[peer.dutIfName] |= peer.linkUp;
      if (peer.linkUp) {
        links[peer.dutIfName].emplace_back(peer.peerIfName, 0);
        links[peer.peerIfName].emplace_back(peer.dutIfName, 0);
      }
    }
    ioProvider_->setConnectedPairs(std::move(links));
    for (const auto& [_, peer] : peers_) {
      platform_.setLink(
          peer.dutIfName, peer.dutIfIndex, interfaceUp.at(peer.dutIfName));
    }
  }

  std::map<std::string, thrift::KeyVals> byArea;
  for (const auto& update : updates) {
    if (update.value) {
      byArea[update.key.area].emplace(update.key.key, *update.value);
    }
  }
  for (auto& [_, peer] : peers_) {
    const auto it = byArea.find(peer.area);
    if (it != byArea.end()) {
      peer.synthetic->publishKeyVals(peer.area, it->second);
    }
  }
  topology_ = std::move(result.topology);
}

void
OpenrE2eLab::setLinkMetric(
    const std::string& link, const std::string& endpoint, std::int32_t metric) {
  impl_->mutateTopology(SetLinkMetric{link, endpoint, metric});
}

void
OpenrE2eLab::setLinkState(const std::string& link, bool isUp) {
  impl_->mutateTopology(SetLinkState{link, isUp});
}

void
OpenrE2eLab::setLinkUp(const std::string& peer, bool isUp) {
  impl_->setLinkUp(peer, isUp);
}

void
OpenrE2eLab::setPartition(
    const std::vector<std::string>& links, bool isPartitioned) {
  impl_->mutateTopology(SetPartition{links, isPartitioned});
}

void
OpenrE2eLab::advertisePrefix(const std::string& node, const std::string& cidr) {
  impl_->advertisePrefix(node, cidr, {});
}

void
OpenrE2eLab::advertisePrefix(
    const std::string& node, const std::string& cidr, PrefixOptions options) {
  impl_->advertisePrefix(node, cidr, std::move(options));
}

void
OpenrE2eLab::withdrawPrefix(const std::string& node, const std::string& cidr) {
  impl_->withdrawPrefix(node, cidr, {});
}

void
OpenrE2eLab::withdrawPrefix(
    const std::string& node,
    const std::string& cidr,
    const PrefixOptions& options) {
  impl_->withdrawPrefix(node, cidr, options);
}

} // namespace openr::e2e
