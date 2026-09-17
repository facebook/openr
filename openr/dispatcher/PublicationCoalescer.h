/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <utility>
#include <variant>

#include <folly/container/F14Set.h>
#include <folly/logging/xlog.h>

#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/messaging/Queue.h>

namespace openr {

namespace detail {

/*
 * Fold `incoming`'s key-values into `existing`, latest value winning per key.
 *
 * A TTL refresh carries NO payload (`Value.value` is unset) and only bumps
 * `ttlVersion` for an otherwise unchanged (version, originatorId). Assigning
 * one over a pending value update would drop that payload entirely for a
 * reader that has not drained yet, so refresh the liveness fields in place
 * instead of overwriting the entry.
 *
 * Only the unfiltered reader can hit this case: DispatcherQueue::filterKeys
 * drops value-less keys for any reader with a non-empty prefix filter.
 */
/*
 * A value carrying no payload is a TTL-only update.
 *
 * NOTE: mirrors `isTtlUpdate` in KvStoreUtil.cpp, which is the same predicate
 * but sits in an anonymous namespace and so cannot be reused here. Promoting
 * it to KvStoreUtil.h would give both a single definition.
 */
inline bool
isTtlOnlyUpdate(const thrift::Value& value) {
  return !value.value().has_value();
}

/*
 * Refresh liveness on a pending entry without disturbing its payload, and only
 * ever forwards. A stale refresh is dropped so the merge is monotonic
 * regardless of arrival order.
 */
inline void
applyTtlRefresh(thrift::Value& pending, const thrift::Value& refresh) {
  if (*refresh.ttlVersion() > *pending.ttlVersion()) {
    pending.ttl() = *refresh.ttl();
    pending.ttlVersion() = *refresh.ttlVersion();
  }
}

/*
 * True when `refresh` is a TTL-only update for exactly the value `pending`
 * already holds, i.e. it refreshes liveness rather than replacing state.
 */
inline bool
refreshesPendingValue(
    const thrift::Value& pending, const thrift::Value& refresh) {
  return isTtlOnlyUpdate(refresh) && !isTtlOnlyUpdate(pending) &&
      *pending.version() == *refresh.version() &&
      *pending.originatorId() == *refresh.originatorId();
}

inline void
mergeKeyValIntoPending(
    thrift::KeyVals& pendingKeyVals,
    const std::string& key,
    thrift::Value& value) {
  auto pendingIt = pendingKeyVals.find(key);
  if (pendingIt != pendingKeyVals.end() &&
      refreshesPendingValue(pendingIt->second, value)) {
    applyTtlRefresh(pendingIt->second, value);
  } else {
    pendingKeyVals.insert_or_assign(key, std::move(value));
  }
}

/*
 * Fold incoming key-value and expiry deltas into pending, following the same
 * update-then-delete structure as DecisionRouteUpdate::mergeInPlace. A later
 * update clears its pending expiry, while a later expiry removes its pending
 * value. Processing expiries last also makes delete win if one incoming
 * publication contains the same key in both collections.
 *
 * Publication.expiredKeys is a Thrift list rather than the F14 set used by
 * DecisionRouteUpdate. Use an owning temporary set while merging, then rebuild
 * the list; expiration ordering has no semantic meaning.
 */
inline void
mergePublicationInPlace(
    thrift::Publication& pending, thrift::Publication& incoming) {
  auto& pendingKeyVals = *pending.keyVals();
  auto& pendingExpiredKeys = *pending.expiredKeys();

  if (pendingExpiredKeys.empty() && incoming.expiredKeys()->empty()) {
    for (auto& [key, value] : *incoming.keyVals()) {
      mergeKeyValIntoPending(pendingKeyVals, key, value);
    }
    return;
  }

  folly::F14FastSet<std::string> expiredKeys;
  expiredKeys.reserve(
      pendingExpiredKeys.size() + incoming.expiredKeys()->size());

  for (auto& key : pendingExpiredKeys) {
    expiredKeys.emplace(std::move(key));
  }
  pendingExpiredKeys.clear();

  for (auto& [key, value] : *incoming.keyVals()) {
    if (expiredKeys.contains(key)) {
      if (isTtlOnlyUpdate(value)) {
        continue;
      }
      expiredKeys.erase(key);
    }

    mergeKeyValIntoPending(pendingKeyVals, key, value);
  }

  for (auto& key : *incoming.expiredKeys()) {
    pendingKeyVals.erase(key);
    expiredKeys.emplace(std::move(key));
  }

  pendingExpiredKeys.reserve(expiredKeys.size());
  for (const auto& key : expiredKeys) {
    pendingExpiredKeys.emplace_back(key);
  }
}

} // namespace detail

/*
 * Suppression classifier for a DispatcherQueue reader. Publications are keyed
 * by area, so the backlog is bounded at one pending element per area.
 *
 * MERGE_PENDING is required because a publication is a batch of independent
 * per-key deltas, not a snapshot of its area. REPLACE_PENDING would drop every
 * key the newer publication does not mention, and KvStore floods each change
 * once and never resends it.
 *
 * An InitializationEvent is a KEY_BARRIER under the empty key, which resets
 * suppression history for every area. It must survive because KVSTORE_SYNCED
 * and ADJACENCY_DB_SYNCED gate the Open/R initialization sequence, and
 * publications after the event form a new coalescing epoch.
 *
 * The empty key is collision-free: a publication carries exactly one area and
 * Decision CHECKs that it is non-empty, so no publication can ever share this
 * barrier's key.
 */
inline messaging::StateSuppressionKey
classifyKvStorePublication(const KvStorePublication& value) {
  if (auto* pub = std::get_if<thrift::Publication>(&value)) {
    return {*pub->area(), messaging::StateSuppressionAction::MERGE_PENDING};
  }
  return {std::string{}, messaging::StateSuppressionAction::KEY_BARRIER};
}

/*
 * Push-time coalescer for a DispatcherQueue reader: folds `incoming` into a
 * pending element so a stalled reader's backlog collapses to the net latest
 * value per key instead of accumulating every intermediate publication. Wired
 * per-reader in Main.cpp behind Config::isQueueCoalescingEnabled().
 *
 * Returns true when `incoming` was absorbed (nothing is appended), false to
 * append it as its own element.
 *
 * classifyKvStorePublication guarantees both sides are publications in the
 * same area. Merging across areas would apply a publication whose keys span
 * areas against the wrong per-area state, since Decision keeps a LinkState per
 * area.
 *
 * An InitializationEvent still returns false rather than asserting: the variant
 * alternative has to be inspected to reach the publication at all, and a
 * KVSTORE_SYNCED or ADJACENCY_DB_SYNCED merged away would break the Open/R
 * initialization sequence.
 *
 * NOTE: this runs under the reader queue's lock, so it must stay cheap.
 */
inline bool
coalesceKvStorePublications(
    KvStorePublication& existing, KvStorePublication& incoming) {
  auto* existingPub = std::get_if<thrift::Publication>(&existing);
  auto* incomingPub = std::get_if<thrift::Publication>(&incoming);

  if (!existingPub || !incomingPub) {
    return false;
  }

  XDCHECK_EQ(*existingPub->area(), *incomingPub->area())
      << "callers must screen with matchKvStorePublicationArea";

  detail::mergePublicationInPlace(*existingPub, *incomingPub);

  return true;
}

/*
 * StateSuppressionPolicy::mergeIntoPending adapter.
 * classifyKvStorePublication only issues MERGE_PENDING for publications, and
 * the keyed queue only pairs equal areas, so the coalescer cannot decline here
 * and its bool return carries no information.
 */
inline void
mergeIntoPending(KvStorePublication& pending, KvStorePublication& incoming) {
  const bool merged = coalesceKvStorePublications(pending, incoming);
  XDCHECK(merged) << "classifier should only pair mergeable publications";
}

/*
 * Ready-made policy for a DispatcherQueue reader.
 *
 * activationThreshold is 0: the threshold exists so a burst stays observable
 * while REPLACE_PENDING is dropping state, and merging drops nothing.
 */
inline messaging::StateSuppressionPolicy<KvStorePublication>
getKvStorePublicationSuppressionPolicy() {
  return messaging::StateSuppressionPolicy<KvStorePublication>{
      classifyKvStorePublication,
      0 /* activationThreshold */,
      mergeIntoPending};
}

} // namespace openr
