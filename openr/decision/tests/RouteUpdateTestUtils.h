/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include <fmt/core.h>

#include <folly/IPAddress.h>
#include <folly/container/F14Set.h>

#include <openr/common/LsdbUtil.h>
#include <openr/decision/RibEntry.h>
#include <openr/if/gen-cpp2/Types_types.h>

/*
 * Builders shared by RouteUpdateTest and RouteUpdateBenchmark so the two cannot
 * drift in how they generate prefixes and RIB entries.
 */
namespace openr {

/*
 * Deterministic /24 for `index`, e.g. 0 -> 10.0.0.0/24, 256 -> 10.1.0.0/24.
 * Wraps after 65536 indices.
 */
inline folly::CIDRNetwork
makeTestPrefix(size_t index) {
  return folly::IPAddress::createNetwork(
      fmt::format("10.{}.{}.0/24", (index / 256) % 256, index % 256));
}

/*
 * Unicast entry for `network` with `numNextHops` distinct next-hops, so the
 * next-hop count can distinguish "which version won" after a merge. Caps at 4.
 */
inline RibUnicastEntry
makeUnicast(const folly::CIDRNetwork& network, int numNextHops) {
  static const std::vector<std::string> kAddrs = {
      "fe80::1", "fe80::2", "fe80::3", "fe80::4"};
  folly::F14FastSet<thrift::NextHopThrift> nhs;
  for (int i = 0; i < numNextHops; ++i) {
    nhs.insert(createNextHop(
        toBinaryAddress(folly::IPAddress(kAddrs.at(i))), "iface"));
  }
  return RibUnicastEntry(network, std::move(nhs));
}

inline RibUnicastEntry
makeUnicast(const std::string& cidr, int numNextHops) {
  return makeUnicast(folly::IPAddress::createNetwork(cidr), numNextHops);
}

inline RibMplsEntry
makeMpls(int32_t label, int numNextHops) {
  static const std::vector<std::string> kAddrs = {
      "fe80::1", "fe80::2", "fe80::3", "fe80::4"};
  folly::F14FastSet<thrift::NextHopThrift> nhs;
  for (int i = 0; i < numNextHops; ++i) {
    nhs.insert(createNextHop(
        toBinaryAddress(folly::IPAddress(kAddrs.at(i))), "iface"));
  }
  return RibMplsEntry(label, std::move(nhs));
}

} // namespace openr
