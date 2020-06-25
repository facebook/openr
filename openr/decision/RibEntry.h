/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <folly/IPAddress.h>
#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>

namespace openr {

struct RibEntry {
  std::unordered_set<thrift::NextHopThrift> nexthops;

  // constructor
  explicit RibEntry(std::unordered_set<thrift::NextHopThrift> nexthops)
      : nexthops(std::move(nexthops)) {}

  RibEntry() = default;

  bool
  operator==(const RibEntry& other) const {
    return nexthops == other.nexthops;
  }
};

struct RibUnicastEntry : RibEntry {
  const folly::CIDRNetwork prefix;
  thrift::PrefixEntry bestPrefixEntry;
  // install to fib or not
  bool doNotInstall{false};
  // best nexthop fields for BGP route advertising
  std::optional<thrift::NextHopThrift> bestNexthop{std::nullopt};

  // constructor
  explicit RibUnicastEntry(const folly::CIDRNetwork& prefix) : prefix(prefix) {}

  RibUnicastEntry(
      const folly::CIDRNetwork& prefix,
      std::unordered_set<thrift::NextHopThrift> nexthops)
      : RibEntry(std::move(nexthops)), prefix(prefix) {}

  RibUnicastEntry(
      const folly::CIDRNetwork& prefix,
      std::unordered_set<thrift::NextHopThrift> nexthops,
      thrift::PrefixEntry bestPrefixEntry,
      bool doNotInstall,
      thrift::NextHopThrift bestNexthop)
      : RibEntry(std::move(nexthops)),
        prefix(prefix),
        bestPrefixEntry(std::move(bestPrefixEntry)),
        doNotInstall(doNotInstall),
        bestNexthop(std::move(bestNexthop)) {}

  bool
  operator==(const RibUnicastEntry& other) const {
    return prefix == other.prefix && bestPrefixEntry == other.bestPrefixEntry &&
        bestNexthop == other.bestNexthop &&
        doNotInstall == other.doNotInstall && RibEntry::operator==(other);
  }

  thrift::UnicastRoute
  toTUnicastRoute() const {
    thrift::UnicastRoute tUnicast;
    tUnicast.dest = toIpPrefix(prefix);
    tUnicast.nextHops =
        std::vector<thrift::NextHopThrift>(nexthops.begin(), nexthops.end());
    tUnicast.doNotInstall = doNotInstall;
    if (bestPrefixEntry.type == thrift::PrefixType::BGP) {
      // TODO: ideally Open/R should not program routes using other admin
      // distance remove tUnicast.adminDistance_ref() = EBGP;
      tUnicast.adminDistance_ref() = thrift::AdminDistance::EBGP;
      tUnicast.prefixType_ref() = thrift::PrefixType::BGP;
      tUnicast.data_ref() = *bestPrefixEntry.data_ref();
      tUnicast.bestNexthop_ref() = bestNexthop.value();
    }
    return tUnicast;
  }
};

struct RibMplsEntry : RibEntry {
  const int32_t label{0};

  explicit RibMplsEntry(int32_t label) : label(label) {}

  // constructor
  RibMplsEntry(
      int32_t label, std::unordered_set<thrift::NextHopThrift> nexthops)
      : RibEntry(std::move(nexthops)), label(label) {}

  bool
  operator==(const RibMplsEntry& other) const {
    return label == other.label && RibEntry::operator==(other);
  }

  thrift::MplsRoute
  toTMplsRoute() const {
    thrift::MplsRoute tMpls;
    tMpls.topLabel = label;
    tMpls.nextHops =
        std::vector<thrift::NextHopThrift>(nexthops.begin(), nexthops.end());
    return tMpls;
  }
};
} // namespace openr
