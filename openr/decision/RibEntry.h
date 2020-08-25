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
  // TODO: should this be map<area, nexthops>?
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
  folly::CIDRNetwork prefix;
  thrift::PrefixEntry bestPrefixEntry;
  std::string bestArea;
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
      const std::string& bestArea,
      bool doNotInstall = false,
      std::optional<thrift::NextHopThrift> bestNexthop = std::nullopt)
      : RibEntry(std::move(nexthops)),
        prefix(prefix),
        bestPrefixEntry(std::move(bestPrefixEntry)),
        bestArea(bestArea),
        doNotInstall(doNotInstall),
        bestNexthop(std::move(bestNexthop)) {}

  bool
  operator==(const RibUnicastEntry& other) const {
    return prefix == other.prefix && bestPrefixEntry == other.bestPrefixEntry &&
        bestNexthop == other.bestNexthop &&
        doNotInstall == other.doNotInstall && RibEntry::operator==(other);
  }

  bool
  operator!=(const RibUnicastEntry& other) const {
    return !(*this == other);
  }

  thrift::UnicastRoute
  toThrift() const {
    thrift::UnicastRoute tUnicast;
    tUnicast.dest = toIpPrefix(prefix);
    *tUnicast.nextHops_ref() =
        std::vector<thrift::NextHopThrift>(nexthops.begin(), nexthops.end());
    *tUnicast.doNotInstall_ref() = doNotInstall;
    if (*bestPrefixEntry.type_ref() == thrift::PrefixType::BGP) {
      tUnicast.prefixType_ref() = thrift::PrefixType::BGP;
      if (bestPrefixEntry.data_ref()) {
        tUnicast.data_ref() = *bestPrefixEntry.data_ref();
      }
      tUnicast.bestNexthop_ref() = bestNexthop.value();
    }
    return tUnicast;
  }
};

struct RibMplsEntry : RibEntry {
  int32_t label{0};

  explicit RibMplsEntry(int32_t label) : label(label) {}

  // constructor
  RibMplsEntry(
      int32_t label, std::unordered_set<thrift::NextHopThrift> nexthops)
      : RibEntry(std::move(nexthops)), label(label) {}

  static RibMplsEntry
  fromThrift(const thrift::MplsRoute& tMpls) {
    return RibMplsEntry(
        tMpls.topLabel,
        std::unordered_set<thrift::NextHopThrift>(
            tMpls.nextHops_ref()->begin(), tMpls.nextHops_ref()->end()));
  }

  bool
  operator==(const RibMplsEntry& other) const {
    return label == other.label && RibEntry::operator==(other);
  }

  bool
  operator!=(const RibMplsEntry& other) const {
    return !(*this == other);
  }

  thrift::MplsRoute
  toThrift() const {
    thrift::MplsRoute tMpls;
    tMpls.topLabel = label;
    *tMpls.nextHops_ref() =
        std::vector<thrift::NextHopThrift>(nexthops.begin(), nexthops.end());
    return tMpls;
  }
};
} // namespace openr
