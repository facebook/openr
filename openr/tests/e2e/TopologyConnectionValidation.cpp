/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyConnectionValidation.h>

#include <algorithm>
#include <cstdint>
#include <optional>

#include <fmt/core.h>
#include <folly/container/F14Set.h>
#include <openr/tests/e2e/TopologyValidation.h>

namespace openr::e2e {

namespace {

constexpr uint8_t kIpv4{1};
constexpr uint8_t kIpv6{2};

std::optional<folly::CIDRNetwork>
v4Address(const LinkEndpointSpec& endpoint) {
  const auto it = std::find_if(
      endpoint.addresses.begin(),
      endpoint.addresses.end(),
      [](const auto& net) { return net.first.isV4(); });
  return it == endpoint.addresses.end()
      ? std::nullopt
      : std::optional<folly::CIDRNetwork>{*it};
}

bool
sameV4Subnet(const LinkEndpointSpec& first, const LinkEndpointSpec& second) {
  const auto firstV4 = v4Address(first);
  const auto secondV4 = v4Address(second);
  return !firstV4.has_value() || !secondV4.has_value() ||
      (firstV4->second == secondV4->second &&
       firstV4->first.mask(firstV4->second) ==
           secondV4->first.mask(secondV4->second));
}

class EndpointValidator {
 public:
  explicit EndpointValidator(
      const folly::F14FastMap<std::string, NodeSpec>& nodes)
      : nodes_{nodes} {}

  uint8_t
  validate(const LinkEndpointSpec& endpoint, const std::string& owner) {
    const auto nodeIt = nodes_.find(endpoint.nodeName);
    if (nodeIt == nodes_.end() || endpoint.interfaceName.empty()) {
      throw TopologyError(fmt::format("{} has an invalid endpoint", owner));
    }
    if (std::find(
            nodeIt->second.areas.begin(),
            nodeIt->second.areas.end(),
            endpoint.area) == nodeIt->second.areas.end()) {
      throw TopologyError(fmt::format("{} uses an unknown area", owner));
    }
    if (endpoint.metric <= 0 || endpoint.latency.count() < 0 ||
        (endpoint.adjacencyLabel.has_value() &&
         *endpoint.adjacencyLabel <= 0)) {
      throw TopologyError(fmt::format("{} has invalid link attributes", owner));
    }
    if (!interfaceNames_[endpoint.nodeName]
             .emplace(endpoint.interfaceName)
             .second) {
      throw TopologyError(
          fmt::format(
              "node '{}' repeats interface '{}'",
              endpoint.nodeName,
              endpoint.interfaceName));
    }
    if (endpoint.interfaceIndex.has_value() &&
        (*endpoint.interfaceIndex <= 0 ||
         !interfaceIndexes_[endpoint.nodeName]
              .emplace(*endpoint.interfaceIndex)
              .second)) {
      throw TopologyError(
          fmt::format(
              "node '{}' has an invalid interface index", endpoint.nodeName));
    }

    uint8_t families{0};
    folly::F14FastSet<std::string> addresses;
    for (const auto& address : endpoint.addresses) {
      validateNetwork(address, owner, false);
      const auto text = folly::IPAddress::networkToString(address);
      if (!addresses.emplace(text).second) {
        throw TopologyError(
            fmt::format("{} repeats address '{}'", owner, text));
      }
      families |= address.first.isV4() ? kIpv4 : kIpv6;
    }
    return families;
  }

 private:
  const folly::F14FastMap<std::string, NodeSpec>& nodes_;
  folly::F14FastMap<std::string, folly::F14FastSet<std::string>>
      interfaceNames_;
  folly::F14FastMap<std::string, folly::F14FastSet<int32_t>> interfaceIndexes_;
};

} // namespace

void
validateConnections(
    const folly::F14FastMap<std::string, NodeSpec>& nodes,
    const std::vector<LinkSpec>& links,
    const std::vector<SegmentSpec>& segments) {
  EndpointValidator validator{nodes};
  folly::F14FastSet<std::string> names;

  for (const auto& link : links) {
    if (link.name.empty() || !names.emplace(link.name).second ||
        link.first.nodeName == link.second.nodeName) {
      throw TopologyError("link name and endpoints must be unique");
    }
    const auto first = validator.validate(link.first, link.name);
    const auto second = validator.validate(link.second, link.name);
    if (link.first.area != link.second.area ||
        !sameV4Subnet(link.first, link.second) ||
        (first != 0 && second != 0 && (first & second) == 0)) {
      throw TopologyError(
          fmt::format("link '{}' has incompatible endpoints", link.name));
    }
  }

  for (const auto& segment : segments) {
    if (segment.name.empty() || !names.emplace(segment.name).second ||
        segment.endpoints.size() < 2) {
      throw TopologyError("segment name and endpoints must be valid");
    }
    const auto& area = segment.endpoints.front().area;
    folly::F14FastSet<std::string> endpointNodes;
    uint8_t commonFamilies{kIpv4 | kIpv6};
    bool allHaveAddresses{true};
    const auto& firstEndpoint = segment.endpoints.front();
    for (const auto& endpoint : segment.endpoints) {
      const auto families = validator.validate(endpoint, segment.name);
      commonFamilies &= families;
      allHaveAddresses &= families != 0;
      if (endpoint.area != area || !sameV4Subnet(firstEndpoint, endpoint) ||
          !endpointNodes.emplace(endpoint.nodeName).second) {
        throw TopologyError(
            fmt::format(
                "segment '{}' has incompatible endpoints", segment.name));
      }
    }
    if (allHaveAddresses && commonFamilies == 0) {
      throw TopologyError(
          fmt::format(
              "segment '{}' has no common address family", segment.name));
    }
  }
}

} // namespace openr::e2e
