/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>
#include <vector>

#include <folly/IPAddress.h>

#include <openr/common/Util.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/RibPolicy.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

/*
 * Generic structure to represent a route update. There are various sources and
 * consumers of route updates,
 * - Decision produces routes updates, consumed by Fib;
 * - Fib produces programmed routes, consumed by PrefixManager/BgpSpeaker;
 * - BgpSpeaker produces static MPLS prepend label routes, consumed by Decision;
 * - PrefixManager produces static unicast routes, consumed by Decision.
 */
struct DecisionRouteUpdate {
  enum Type {
    // Incremental route updates.
    INCREMENTAL,
    // Full-sync route updates after openr (re)starts.
    FULL_SYNC,
  };

  Type type{INCREMENTAL}; // Incremental route update is default behavior
  std::unordered_map<folly::CIDRNetwork /* prefix */, RibUnicastEntry>
      unicastRoutesToUpdate;
  std::vector<folly::CIDRNetwork> unicastRoutesToDelete;
  // TODO: Establish parity with `unicastRoutesToUpdate`
  std::vector<RibMplsEntry> mplsRoutesToUpdate;
  std::vector<int32_t> mplsRoutesToDelete;
  std::optional<thrift::PerfEvents> perfEvents = std::nullopt;

  bool
  empty() const {
    return (
        unicastRoutesToUpdate.empty() and unicastRoutesToDelete.empty() and
        mplsRoutesToUpdate.empty() and mplsRoutesToDelete.empty());
  }

  size_t
  size() const {
    return unicastRoutesToUpdate.size() + unicastRoutesToDelete.size() +
        mplsRoutesToUpdate.size() + mplsRoutesToDelete.size();
  }

  void
  addRouteToUpdate(RibUnicastEntry const& route) {
    CHECK(!unicastRoutesToUpdate.count(route.prefix));
    unicastRoutesToUpdate.emplace(route.prefix, route);
  }

  void
  addRouteToUpdate(RibUnicastEntry&& route) {
    auto prefix = route.prefix;
    CHECK(!unicastRoutesToUpdate.count(prefix));
    unicastRoutesToUpdate.emplace(std::move(prefix), std::move(route));
  }

  // TODO: rename this func
  thrift::RouteDatabaseDelta
  toThrift() {
    thrift::RouteDatabaseDelta delta;

    // unicast
    for (const auto& [_, route] : unicastRoutesToUpdate) {
      delta.unicastRoutesToUpdate_ref()->emplace_back(route.toThrift());
    }
    for (const auto& route : unicastRoutesToDelete) {
      delta.unicastRoutesToDelete_ref()->emplace_back(toIpPrefix(route));
    }
    // mpls
    for (const auto& route : mplsRoutesToUpdate) {
      delta.mplsRoutesToUpdate_ref()->emplace_back(route.toThrift());
    }
    *delta.mplsRoutesToDelete_ref() = mplsRoutesToDelete;
    delta.perfEvents_ref().from_optional(perfEvents);

    return delta;
  }

  // TODO: rename this func
  thrift::RouteDatabaseDeltaDetail
  toThriftDetail() {
    thrift::RouteDatabaseDeltaDetail deltaDetail;

    // unicast
    for (const auto& [_, route] : unicastRoutesToUpdate) {
      deltaDetail.unicastRoutesToUpdate_ref()->emplace_back(
          route.toThriftDetail());
    }
    for (const auto& route : unicastRoutesToDelete) {
      deltaDetail.unicastRoutesToDelete_ref()->emplace_back(toIpPrefix(route));
    }
    // mpls
    for (const auto& route : mplsRoutesToUpdate) {
      deltaDetail.mplsRoutesToUpdate_ref()->emplace_back(
          route.toThriftDetail());
    }
    *deltaDetail.mplsRoutesToDelete_ref() = mplsRoutesToDelete;

    return deltaDetail;
  }

  /**
   * Process FIB update error. It removes all the entries to add/update from
   * this update that failed to program.
   *
   * NOTE: We don't remove all the entries that failed to remove, rather we
   * keep them as removed. It is better to inform that route is removed instead
   * of not informing that it is not removed.
   */
  void
  processFibUpdateError(thrift::PlatformFibUpdateError const& fibError) {
    // Delete unicast routes that failed to program. Also mark them as deleted
    for (auto& [_, prefixes] : *fibError.vrf2failedAddUpdatePrefixes_ref()) {
      for (auto& prefix : prefixes) {
        auto network = toIPNetwork(prefix);
        unicastRoutesToUpdate.erase(network);
        unicastRoutesToDelete.emplace_back(network);
      }
    }

    // Delete mpls routes that failed to program. Also mark them as deleted
    // TODO has: Improve the complexity by changing data-structure of
    // mplsRoutesToUpdate
    std::unordered_set<int32_t> failedMplsRouteToUpdate{
        fibError.failedAddUpdateMplsLabels_ref()->begin(),
        fibError.failedAddUpdateMplsLabels_ref()->end()};
    for (auto it = mplsRoutesToUpdate.begin();
         it != mplsRoutesToUpdate.end();) {
      if (failedMplsRouteToUpdate.count(it->label)) {
        it = mplsRoutesToUpdate.erase(it);
      } else {
        ++it;
      }
    }
    mplsRoutesToDelete.insert(
        mplsRoutesToDelete.end(),
        failedMplsRouteToUpdate.begin(),
        failedMplsRouteToUpdate.end());
  }
};

} // namespace openr
