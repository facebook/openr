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

  /*
   * Type of this route update. Client should reset state if type of received
   * route update is FULL_SYNC
   */
  Type type{INCREMENTAL}; // Incremental route update is default behavior

  // Unicast routes
  folly::F14FastMap<folly::CIDRNetwork /* prefix */, RibUnicastEntry>
      unicastRoutesToUpdate;
  folly::F14FastSet<folly::CIDRNetwork> unicastRoutesToDelete;

  // MPLS routes
  folly::F14FastMap<int32_t, RibMplsEntry> mplsRoutesToUpdate;
  folly::F14FastSet<int32_t> mplsRoutesToDelete;

  /*
   * Bitmask of prefix types whose initial routes are included in the struct,
   * one bit per thrift::PrefixType value (e.g. CONFIG = 8 sets bit 8). Merging
   * ORs the masks so coalescing never drops a type that Decision's
   * initialization process is waiting for.
   *
   * 32 bits are enough: the highest thrift::PrefixType value is 25 (TYPE_5,
   * one of the reserved placeholders), and only CONFIG is set today. A new
   * type has the unused values 11-20 and the placeholders to take first. If a
   * value of 32 or more is ever added, the static_assert below fails the
   * build, and this field should be widened to uint64_t.
   */
  uint32_t prefixTypes{0};
  static_assert(
      static_cast<uint32_t>(
          apache::thrift::TEnumTraits<thrift::PrefixType>::max()) < 32);

  static constexpr uint32_t
  prefixTypeBit(thrift::PrefixType prefixType) {
    return uint32_t{1} << static_cast<uint32_t>(prefixType);
  }

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
      for (auto& [prefix, route] : other.unicastRoutesToUpdate) {
        unicastRoutesToDelete.erase(prefix);
        unicastRoutesToUpdate.insert_or_assign(prefix, std::move(route));
      }
      for (const auto& prefix : other.unicastRoutesToDelete) {
        unicastRoutesToUpdate.erase(prefix);
        unicastRoutesToDelete.insert(prefix);
      }
      for (auto& [label, route] : other.mplsRoutesToUpdate) {
        mplsRoutesToDelete.erase(label);
        mplsRoutesToUpdate.insert_or_assign(label, std::move(route));
      }
      for (const auto& label : other.mplsRoutesToDelete) {
        mplsRoutesToUpdate.erase(label);
        mplsRoutesToDelete.insert(label);
      }
    }

    if (other.perfEvents.has_value()) {
      perfEvents = std::move(other.perfEvents);
    }
    prefixTypes |= other.prefixTypes;
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
    for (const auto& label : mplsRoutesToDelete) {
      delta.mplsRoutesToDelete()->emplace_back(label);
    }
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
    for (const auto& label : mplsRoutesToDelete) {
      deltaDetail.mplsRoutesToDelete()->emplace_back(label);
    }

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
        unicastRoutesToDelete.emplace(network);
      }
    }

    // Delete mpls routes that failed to program. Also mark them as deleted
    for (auto& label : *fibError.failedAddUpdateMplsLabels()) {
      mplsRoutesToUpdate.erase(label);
      mplsRoutesToDelete.emplace(label);
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

/*
 * Mark `update` as carrying routes of `prefixType`. Types already set are kept,
 * e.g. setting VIP on a CONFIG update leaves both set.
 */
inline void
setPrefixType(DecisionRouteUpdate& update, thrift::PrefixType prefixType) {
  update.prefixTypes |= DecisionRouteUpdate::prefixTypeBit(prefixType);
}

inline bool
isPrefixType(thrift::PrefixType prefixType, const DecisionRouteUpdate& update) {
  return update.prefixTypes & DecisionRouteUpdate::prefixTypeBit(prefixType);
}

/*
 * Preserve the distinction between an authoritative whole-table snapshot and
 * a delta produced by recomputing routes. Only a genuine FULL_SYNC may replace
 * the pending queue entry; every incremental update must be folded into it so
 * a slow Fib consumer cannot lose an earlier route change.
 */
inline bool
coalesceDecisionRouteUpdates(
    DecisionRouteUpdate& existing, DecisionRouteUpdate& incoming) {
  if (incoming.type == DecisionRouteUpdate::FULL_SYNC) {
    incoming.prefixTypes |= existing.prefixTypes;
    existing = std::move(incoming);
  } else {
    existing.mergeInPlace(std::move(incoming));
  }
  return true;
}

/*
 * Coalescer for consumers that CANNOT distinguish a whole-table snapshot from a
 * delta, i.e. the Fib snoop stream: DecisionRouteUpdate::toThrift() produces a
 * thrift::RouteDatabaseDelta, which carries no `type` field.
 *
 * Merges only INCREMENTAL into INCREMENTAL and never touches a FULL_SYNC on
 * either side. Folding an incremental into a pending FULL_SYNC (what
 * coalesceDecisionRouteUpdates does) would lose information for such a
 * consumer: mergeInPlace applies a delete by dropping the key from the
 * snapshot's map WITHOUT recording it in unicastRoutesToDelete, which is
 * correct whole-table semantics but invisible to a client that can only apply
 * the message as a delta -- it would silently retain the withdrawn route.
 *
 * Fib emits FULL_SYNC only on its initial sync, so in practice this bounds the
 * snoop reader at two elements: the pending FULL_SYNC plus one merged
 * incremental.
 */
inline bool
coalesceIncrementalRouteUpdates(
    DecisionRouteUpdate& existing, DecisionRouteUpdate& incoming) {
  if (existing.type == DecisionRouteUpdate::FULL_SYNC ||
      incoming.type == DecisionRouteUpdate::FULL_SYNC) {
    return false;
  }
  existing.mergeInPlace(std::move(incoming));
  return true;
}

} // namespace openr
