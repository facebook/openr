/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrE2eInternal.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <openr/tests/e2e/TopologyAllocator.h>
#include <openr/tests/e2e/TopologyMaterializer.h>

namespace openr::e2e {
namespace {

std::string
realNodeName(const Topology& topology) {
  if (topology.realNodeCount() != 1) {
    throw TopologyError("Topology E2E fixture requires one real router");
  }
  for (const auto& name : topology.nodeNames()) {
    if (topology.node(name).kind == NodeKind::REAL) {
      return name;
    }
  }
  throw TopologyError("Topology E2E fixture has no real router");
}

std::string
ipv6Address(const LinkEndpointSpec& endpoint) {
  for (const auto& [address, _] : endpoint.addresses) {
    if (address.isV6()) {
      return address.str();
    }
  }
  throw TopologyError("Topology link has no allocated IPv6 address");
}

} // namespace

OpenrE2eLab::Impl::Impl(const Topology& topology)
    : Impl(realNodeName(topology)) {
  topology_.emplace(TopologyAllocator::allocate(topology));
  TopologyMaterializer materializer{topologyVersions_};
  std::map<std::string, thrift::KeyVals> initialByArea;
  for (const auto& update : materializer.materialize(*topology_)) {
    if (update.value) {
      initialByArea[update.key.area].emplace(update.key.key, *update.value);
    }
  }

  std::map<std::pair<std::string, std::string>, int> interfaceIndexes;
  const auto indexOf = [&](const std::string& node,
                           const std::string& interfaceName) {
    return interfaceIndexes
        .try_emplace(
            std::make_pair(node, interfaceName),
            static_cast<int>(interfaceIndexes.size()) + 100)
        .first->second;
  };
  std::set<std::string> usedInterfaceNames;
  for (const auto& link : topology_->links()) {
    usedInterfaceNames.insert(link.first.interfaceName);
    usedInterfaceNames.insert(link.second.interfaceName);
  }
  for (const auto& segment : topology_->segments()) {
    for (const auto& endpoint : segment.endpoints) {
      usedInterfaceNames.insert(endpoint.interfaceName);
    }
  }
  size_t transportOrdinal{0};
  const auto transportIfName = [&] {
    std::string name;
    do {
      name = "e2e-transport-" + std::to_string(transportOrdinal++);
    } while (!usedInterfaceNames.insert(name).second);
    return name;
  };
  const auto attach = [&](const LinkEndpointSpec& real,
                          const LinkEndpointSpec& synthetic,
                          const std::string& linkName,
                          bool operational) {
    const auto name = synthetic.nodeName;
    if (peers_.count(name)) {
      throw TopologyError(
          "Synthetic peer has multiple direct attachments: " + name);
    }
    Peer entry;
    entry.name = name;
    entry.dutIfName = real.interfaceName;
    entry.peerIfName = transportIfName();
    entry.dutIfIndex = indexOf(nodeName_, real.interfaceName);
    entry.peerIfIndex = indexOf(name, synthetic.interfaceName);
    entry.dutV6 = ipv6Address(real);
    entry.peerV6 = ipv6Address(synthetic);
    entry.area = synthetic.area;
    entry.linkName = linkName;
    entry.linkUp = operational && topology_->node(name).operational;
    SyntheticPeerConfig config;
    config.nodeName = nodeName_;
    config.peerName = name;
    config.dutIfName = entry.dutIfName;
    config.peerIfName = synthetic.interfaceName;
    config.transportIfName = entry.peerIfName;
    config.dutIfIndex = entry.dutIfIndex;
    config.peerIfIndex = entry.peerIfIndex;
    config.dutV6Address = entry.dutV6;
    config.peerV6Address = entry.peerV6;
    config.dutKvStorePort = kvStoreService_.port();
    config.ioProvider = ioProvider_;
    config.area = synthetic.area;
    config.initialKeyVals = initialByArea.at(synthetic.area);
    entry.synthetic = std::make_unique<SyntheticPeer>(std::move(config));
    peers_.emplace(name, std::move(entry));
  };
  for (const auto& link : topology_->links()) {
    const bool firstReal =
        topology_->node(link.first.nodeName).kind == NodeKind::REAL;
    const bool secondReal =
        topology_->node(link.second.nodeName).kind == NodeKind::REAL;
    if (firstReal != secondReal) {
      attach(
          firstReal ? link.first : link.second,
          firstReal ? link.second : link.first,
          link.name,
          link.operational);
    }
  }
  for (const auto& segment : topology_->segments()) {
    const auto real = std::find_if(
        segment.endpoints.begin(),
        segment.endpoints.end(),
        [&](const auto& ep) { return ep.nodeName == nodeName_; });
    if (real == segment.endpoints.end()) {
      continue;
    }
    for (const auto& endpoint : segment.endpoints) {
      if (endpoint.nodeName != nodeName_) {
        attach(*real, endpoint, segment.name, true);
      }
    }
  }
}

OpenrE2eLab::OpenrE2eLab(const Topology& topology)
    : impl_{std::make_unique<Impl>(topology)} {}

} // namespace openr::e2e
