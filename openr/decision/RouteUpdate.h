/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sstream>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>

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
 * - PrefixManager produces static unicast routes, consumed by Decision.
 */
struct DecisionRouteUpdate {
  enum Type {
    // Incremental route updates.
    INCREMENTAL,
    // Full-sync route updates after openr (re)starts.
    FULL_SYNC,
  };

  // Type of this route update. Client should reset state if type of received
  // route update is FULL_SYNC
  Type type{INCREMENTAL}; // Incremental route update is default behavior

  // Unicast routes
  folly::F14FastMap<folly::CIDRNetwork /* prefix */, RibUnicastEntry>
      unicastRoutesToUpdate;
  std::vector<folly::CIDRNetwork> unicastRoutesToDelete;

  // MPLS routes
  folly::F14FastMap<int32_t, RibMplsEntry> mplsRoutesToUpdate;
  std::vector<int32_t> mplsRoutesToDelete;

  // Optional prefix type whose unicast/label routes are included in the struct.
  // Used in OpenR initialization process.
  std::optional<thrift::PrefixType> prefixType{std::nullopt};

  // Optional perf events associated with this route update
  std::optional<thrift::PerfEvents> perfEvents{std::nullopt};

  bool
  empty() const {
    return (
        unicastRoutesToUpdate.empty() && unicastRoutesToDelete.empty() &&
        mplsRoutesToUpdate.empty() && mplsRoutesToDelete.empty());
  }

  size_t
  size() const {
    return unicastRoutesToUpdate.size() + unicastRoutesToDelete.size() +
        mplsRoutesToUpdate.size() + mplsRoutesToDelete.size();
  }

  /**
   * Fold a subsequent INCREMENTAL update `other` into this one so a stream of
   * per-prefix deltas collapses to its net latest value. Later state wins per
   * key: a later update supersedes a prior delete of the same prefix/label, and
   * a later delete supersedes a prior update.
   *
   * `other` MUST be INCREMENTAL: a FULL_SYNC carries whole-table semantics and
   * cannot be expressed as a delta layered on top of another update. `this`,
   * however, may be either type, and its semantics are preserved:
   *  - INCREMENTAL base: the result is the combined delta -- deletes accumulate
   *    in unicastRoutesToDelete/mplsRoutesToDelete.
   *  - FULL_SYNC base: the result stays a whole-table snapshot. A deleted key
   * is simply removed from the snapshot's update map (absence == not in table);
   *    no explicit delete entry is added, because a delete list is meaningless
   *    on a full snapshot. This lets the Decision->Fib push-time coalescer fold
   *    later incrementals into a pending full-sync while keeping whole-table
   *    semantics intact (see the getReader coalescer wiring in Main.cpp).
   */
  void
  mergeInPlace(DecisionRouteUpdate&& other) {
    if (type == FULL_SYNC) {
      // Whole-table snapshot: apply the delta directly onto the update maps. A
      // delete just drops the key from the snapshot (absence == deleted); we
      // never accumulate a delete list on a full-sync.
      for (auto& [prefix, route] : other.unicastRoutesToUpdate) {
        unicastRoutesToUpdate.insert_or_assign(prefix, std::move(route));
      }
      for (const auto& prefix : other.unicastRoutesToDelete) {
        unicastRoutesToUpdate.erase(prefix);
      }
      for (auto& [label, route] : other.mplsRoutesToUpdate) {
        mplsRoutesToUpdate.insert_or_assign(label, std::move(route));
      }
      for (const auto& label : other.mplsRoutesToDelete) {
        mplsRoutesToUpdate.erase(label);
      }
    } else {
      // Delta base: reconcile the delete lists via sets so add/delete ordering
      // is correct.
      folly::F14FastSet<folly::CIDRNetwork> unicastDeletes(
          unicastRoutesToDelete.begin(), unicastRoutesToDelete.end());
      folly::F14FastSet<int32_t> mplsDeletes(
          mplsRoutesToDelete.begin(), mplsRoutesToDelete.end());

      // Unicast: a newer update supersedes a prior delete of the same prefix.
      for (auto& [prefix, route] : other.unicastRoutesToUpdate) {
        unicastDeletes.erase(prefix);
        unicastRoutesToUpdate.insert_or_assign(prefix, std::move(route));
      }
      // Unicast: a newer delete supersedes a prior update of the same prefix.
      for (const auto& prefix : other.unicastRoutesToDelete) {
        unicastRoutesToUpdate.erase(prefix);
        unicastDeletes.insert(prefix);
      }
      // MPLS: same reconciliation.
      for (auto& [label, route] : other.mplsRoutesToUpdate) {
        mplsDeletes.erase(label);
        mplsRoutesToUpdate.insert_or_assign(label, std::move(route));
      }
      for (const auto& label : other.mplsRoutesToDelete) {
        mplsRoutesToUpdate.erase(label);
        mplsDeletes.insert(label);
      }

      unicastRoutesToDelete.assign(
          unicastDeletes.begin(), unicastDeletes.end());
      mplsRoutesToDelete.assign(mplsDeletes.begin(), mplsDeletes.end());
    }

    // Keep the freshest perf events / prefix type from the later update.
    if (other.perfEvents.has_value()) {
      perfEvents = std::move(other.perfEvents);
    }
    if (other.prefixType.has_value()) {
      prefixType = other.prefixType;
    }
  }

  /**
   * Add unicast route.
   * NOTE: Parameter is by value that can be constructed from `const&` as well
   * as rvalue. In case of later it'll ensure zero-copy.
   */
  void
  addRouteToUpdate(RibUnicastEntry route) {
    auto prefix = route.prefix; // NOTE: Intended copy
    unicastRoutesToUpdate.insert_or_assign(prefix, std::move(route));
  }

  /**
   * Add mpls route.
   * NOTE: Parameter is by value that can be constructed from `const&` as well
   * as rvalue. In case of later it'll ensure zero-copy.
   */
  void
  addMplsRouteToUpdate(RibMplsEntry route) {
    auto label = route.label; // NOTE: Intended copy
    mplsRoutesToUpdate.insert_or_assign(label, std::move(route));
  }

  // TODO: rename this func
  thrift::RouteDatabaseDelta
  toThrift() {
    thrift::RouteDatabaseDelta delta;

    // unicast
    for (const auto& [_, route] : unicastRoutesToUpdate) {
      delta.unicastRoutesToUpdate()->emplace_back(route.toThrift());
    }
    for (const auto& route : unicastRoutesToDelete) {
      delta.unicastRoutesToDelete()->emplace_back(toIpPrefix(route));
    }
    // mpls
    for (const auto& [_, route] : mplsRoutesToUpdate) {
      delta.mplsRoutesToUpdate()->emplace_back(route.toThrift());
    }
    *delta.mplsRoutesToDelete() = mplsRoutesToDelete;
    delta.perfEvents().from_optional(perfEvents);

    return delta;
  }

  // TODO: rename this func
  thrift::RouteDatabaseDeltaDetail
  toThriftDetail() {
    thrift::RouteDatabaseDeltaDetail deltaDetail;

    // unicast
    for (const auto& [_, route] : unicastRoutesToUpdate) {
      deltaDetail.unicastRoutesToUpdate()->emplace_back(route.toThriftDetail());
    }
    for (const auto& route : unicastRoutesToDelete) {
      deltaDetail.unicastRoutesToDelete()->emplace_back(toIpPrefix(route));
    }
    // mpls
    for (const auto& [_, route] : mplsRoutesToUpdate) {
      deltaDetail.mplsRoutesToUpdate()->emplace_back(route.toThriftDetail());
    }
    *deltaDetail.mplsRoutesToDelete() = mplsRoutesToDelete;

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
    for (auto& [_, prefixes] : *fibError.vrf2failedAddUpdatePrefixes()) {
      for (auto& prefix : prefixes) {
        auto network = toIPNetwork(prefix);
        unicastRoutesToUpdate.erase(network);
        unicastRoutesToDelete.emplace_back(network);
      }
    }

    // Delete mpls routes that failed to program. Also mark them as deleted
    for (auto& label : *fibError.failedAddUpdateMplsLabels()) {
      mplsRoutesToUpdate.erase(label);
      mplsRoutesToDelete.emplace_back(label);
    }
  }

  /**
   * Print to log for debugging
   */
  std::string
  str() {
    std::stringstream ss;
    ss << "DecisionRouteUpdate follows" << std::boolalpha;
    ss << "\n  Sync: " << (type == DecisionRouteUpdate::FULL_SYNC);
    for (auto const& [prefix, _] : unicastRoutesToUpdate) {
      ss << "\n  ADD prefix " << folly::IPAddress::networkToString(prefix);
    }
    for (auto const& [label, _] : mplsRoutesToUpdate) {
      ss << "\n  ADD label " << label;
    }
    for (auto const& prefix : unicastRoutesToDelete) {
      ss << "\n  DEL prefix " << folly::IPAddress::networkToString(prefix);
    }
    for (auto const& label : mplsRoutesToDelete) {
      ss << "\n  DEL label " << label;
    }
    return ss.str();
  }
};

} // namespace openr
