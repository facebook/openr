/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyAddressAllocator.h>

#include <optional>

#include <fmt/core.h>
#include <folly/container/F14Set.h>

namespace openr::e2e {

namespace {

std::optional<folly::CIDRNetwork>
address(const LinkEndpointSpec& endpoint, bool v4) {
  for (const auto& network : endpoint.addresses) {
    if (network.first.isV4() == v4) {
      return network;
    }
  }
  return std::nullopt;
}

folly::CIDRNetwork
defaultAddress(size_t subnet, size_t host, bool v4, uint8_t v4Mask) {
  if (subnet > 0xFFFF || host > 0xFF) {
    throw TopologyError("deterministic address space exhausted");
  }
  const auto text = v4
      ? fmt::format(
            "10.{}.{}.{}/{}", (subnet >> 8) & 0xFF, subnet & 0xFF, host, v4Mask)
      : fmt::format("fe80::{:x}:{:x}/64", subnet, host + 1);
  return folly::IPAddress::createNetwork(text, -1, false);
}

uint32_t
v4Value(const folly::IPAddress& address) {
  const auto bytes = address.asV4().toByteArray();
  return static_cast<uint32_t>(bytes[0]) << 24 |
      static_cast<uint32_t>(bytes[1]) << 16 |
      static_cast<uint32_t>(bytes[2]) << 8 | bytes[3];
}

folly::CIDRNetwork
addressInSubnet(
    const folly::CIDRNetwork& subnet, folly::F14FastSet<uint32_t>& used) {
  if (!subnet.first.isV4() || subnet.second == 32) {
    throw TopologyError("explicit IPv4 subnet has no address to allocate");
  }
  const auto base = v4Value(subnet.first.mask(subnet.second));
  const uint64_t capacity = uint64_t{1} << (32 - subnet.second);
  const uint64_t firstHost = subnet.second <= 30 ? 1 : 0;
  const uint64_t endHost = subnet.second <= 30 ? capacity - 1 : capacity;
  for (uint64_t offset = firstHost; offset < endHost; ++offset) {
    const auto candidate = static_cast<uint32_t>(base + offset);
    if (!used.emplace(candidate).second) {
      continue;
    }
    const folly::ByteArray4 bytes{
        static_cast<uint8_t>(candidate >> 24),
        static_cast<uint8_t>(candidate >> 16),
        static_cast<uint8_t>(candidate >> 8),
        static_cast<uint8_t>(candidate)};
    return {folly::IPAddress{folly::IPAddressV4{bytes}}, subnet.second};
  }
  throw TopologyError("explicit IPv4 subnet has no free address");
}

void
fillV6(LinkEndpointSpec& endpoint, size_t subnet, size_t host) {
  if (!address(endpoint, false).has_value()) {
    endpoint.addresses.push_back(defaultAddress(subnet, host, false, 0));
  }
}

void
fillV4(
    std::vector<LinkEndpointSpec*> endpoints,
    size_t subnet,
    uint8_t defaultMask) {
  std::optional<folly::CIDRNetwork> explicitSubnet;
  folly::F14FastSet<uint32_t> used;
  for (const auto* endpoint : endpoints) {
    if (const auto v4 = address(*endpoint, true)) {
      explicitSubnet = explicitSubnet.value_or(*v4);
      if (v4->second != explicitSubnet->second ||
          v4->first.mask(v4->second) !=
              explicitSubnet->first.mask(explicitSubnet->second)) {
        throw TopologyError("explicit IPv4 addresses use different subnets");
      }
      used.emplace(v4Value(v4->first));
    }
  }
  for (size_t i = 0; i < endpoints.size(); ++i) {
    if (address(*endpoints[i], true).has_value()) {
      continue;
    }
    endpoints[i]->addresses.push_back(
        explicitSubnet.has_value()
            ? addressInSubnet(*explicitSubnet, used)
            : defaultAddress(
                  subnet, defaultMask == 31 ? i : i + 1, true, defaultMask));
  }
}

} // namespace

void
allocateLinkAddresses(LinkSpec& link, size_t subnet) {
  fillV6(link.first, subnet, 0);
  fillV6(link.second, subnet, 1);
  fillV4({&link.first, &link.second}, subnet, 31);
}

void
allocateSegmentAddresses(SegmentSpec& segment, size_t subnet) {
  if (segment.endpoints.size() > 254) {
    throw TopologyError("shared segment has too many endpoints");
  }
  std::vector<LinkEndpointSpec*> endpoints;
  for (size_t i = 0; i < segment.endpoints.size(); ++i) {
    fillV6(segment.endpoints[i], subnet, i);
    endpoints.push_back(&segment.endpoints[i]);
  }
  fillV4(std::move(endpoints), subnet, 24);
}

} // namespace openr::e2e
