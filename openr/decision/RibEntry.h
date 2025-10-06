/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "openr/if/gen-cpp2/Network_types.h"

#include <folly/IPAddress.h>
#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/OpenrCtrl.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

struct RibEntry {
  // TODO: should this be map<area, nexthops>?
  std::unordered_set<thrift::NextHopThrift> nexthops;

  // igp cost of all routes (ecmp) or of lowest cost route (if ucmp)
  unsigned int igpCost;

  // constructor
  explicit RibEntry(
      std::unordered_set<thrift::NextHopThrift> nexthops,
      unsigned int igpCost = 0)
      : nexthops(std::move(nexthops)), igpCost(igpCost) {}

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
  // Counter Id assigned to this route. Assignment comes from the
  // RibPolicyStatement that matches to this route.
  std::optional<thrift::RouteCounterID> counterID{std::nullopt};
  bool localRouteConsidered{false};

  // constructor
  explicit RibUnicastEntry() = default;
  explicit RibUnicastEntry(const folly::CIDRNetwork& prefix) : prefix(prefix) {}

  RibUnicastEntry(
      const folly::CIDRNetwork& prefix,
      std::unordered_set<thrift::NextHopThrift> nexthops)
      : RibEntry(std::move(nexthops)), prefix(prefix) {}

  RibUnicastEntry(
      const folly::CIDRNetwork& prefix,
      std::unordered_set<thrift::NextHopThrift> nexthops,
      thrift::PrefixEntry bestPrefixEntryThrift,
      const std::string& bestArea,
      bool doNotInstall = false,
      unsigned int igpCost = 0,
      const std::optional<int64_t>& ucmpWeight = std::nullopt,
      bool localRouteConsidered = false)
      : RibEntry(std::move(nexthops), igpCost),
        prefix(prefix),
        bestPrefixEntry(std::move(bestPrefixEntryThrift)),
        bestArea(bestArea),
        doNotInstall(doNotInstall),
        localRouteConsidered(localRouteConsidered) {
    bestPrefixEntry.weight().from_optional(ucmpWeight);
  }

  bool
  operator==(const RibUnicastEntry& other) const {
    return prefix == other.prefix && bestPrefixEntry == other.bestPrefixEntry &&
        doNotInstall == other.doNotInstall && counterID == other.counterID &&
        localRouteConsidered == other.localRouteConsidered &&
        RibEntry::operator==(other);
  }

  bool
  operator!=(const RibUnicastEntry& other) const {
    return !(*this == other);
  }

  // TODO: rename this func
  thrift::UnicastRoute
  toThrift() const {
    thrift::UnicastRoute tUnicast;
    tUnicast.dest() = toIpPrefix(prefix);
    tUnicast.nextHops() =
        std::vector<thrift::NextHopThrift>(nexthops.begin(), nexthops.end());
    tUnicast.counterID().from_optional(counterID);
    return tUnicast;
  }

  // TODO: rename this func
  thrift::UnicastRouteDetail
  toThriftDetail() const {
    thrift::UnicastRouteDetail tUnicastDetail;
    tUnicastDetail.unicastRoute() = toThrift();
    tUnicastDetail.bestRoute() = bestPrefixEntry;
    return tUnicastDetail;
  }
};

struct RibMplsEntry : RibEntry {
  int32_t label{0};

  explicit RibMplsEntry(int32_t label) : label(label) {}

  // constructor
  explicit RibMplsEntry() = default;
  RibMplsEntry(
      int32_t label, std::unordered_set<thrift::NextHopThrift> nexthops)
      : RibEntry(std::move(nexthops)), label(label) {}

  static RibMplsEntry
  fromThrift(const thrift::MplsRoute& tMpls) {
    return RibMplsEntry(
        *tMpls.topLabel(),
        std::unordered_set<thrift::NextHopThrift>(
            tMpls.nextHops()->begin(), tMpls.nextHops()->end()));
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
    tMpls.topLabel() = label;
    tMpls.nextHops() =
        std::vector<thrift::NextHopThrift>(nexthops.begin(), nexthops.end());
    return tMpls;
  }

  thrift::MplsRouteDetail
  toThriftDetail() const {
    thrift::MplsRouteDetail tMplsDetail;
    tMplsDetail.mplsRoute() = toThrift();
    return tMplsDetail;
  }

  /**
   * MPLS Action can be specified per next-hop. However, HW can only specify a
   * single action on a next-hop group. For this reason we filter next-hops
   * to a unique MPLS action.
   */
  void
  filterNexthopsToUniqueAction() {
    // Optimization for single nexthop case. POP_AND_LOOKUP is supported by
    // this optimization
    if (nexthops.size() <= 1) {
      return;
    }

    // Deduce a unique MPLS action
    thrift::MplsActionCode mplsActionCode{thrift::MplsActionCode::SWAP};
    for (auto const& nextHop : nexthops) {
      CHECK(nextHop.mplsAction().has_value());
      auto& action = *nextHop.mplsAction()->action();
      // Action can't be push (we don't push labels in MPLS routes)
      // or POP with multiple nexthops. It must be either SWAP or PHP
      CHECK(
          action == thrift::MplsActionCode::SWAP ||
          action == thrift::MplsActionCode::PHP);
      if (action == thrift::MplsActionCode::PHP) {
        mplsActionCode = thrift::MplsActionCode::PHP;
      }
    }

    // Filter nexthop that do not match selected MPLS action
    for (auto it = nexthops.begin(); it != nexthops.end();) {
      if (mplsActionCode != *it->mplsAction()->action()) {
        it = nexthops.erase(it);
      } else {
        ++it;
      }
    }
  }
};
} // namespace openr
