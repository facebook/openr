/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Set.h>

#include <openr/common/LsdbTypes.h>
#include <openr/common/NetworkUtil.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

inline messaging::StateSuppressionKey
getPrefixUpdateStateSuppressionKey(const PrefixEvent& event) {
  if (event.type != thrift::PrefixType::LOOPBACK) {
    return messaging::StateSuppressionKey{
        "", messaging::StateSuppressionAction::DROP};
  }
  switch (event.eventType) {
  case PrefixEventType::ADD_PREFIXES:
    return messaging::StateSuppressionKey{
        "", messaging::StateSuppressionAction::PURGEABLE};
  case PrefixEventType::WITHDRAW_PREFIXES:
    return messaging::StateSuppressionKey{
        "withdraw", messaging::StateSuppressionAction::MERGE_PENDING_AND_PURGE};
  case PrefixEventType::WITHDRAW_PREFIXES_BY_TYPE:
  case PrefixEventType::SYNC_PREFIXES_BY_TYPE:
    return messaging::StateSuppressionKey{
        "", messaging::StateSuppressionAction::DROP};
  }
  return messaging::StateSuppressionKey{
      "", messaging::StateSuppressionAction::DROP};
}

inline void
mergePrefixWithdrawIntoPending(PrefixEvent& pending, PrefixEvent& incoming) {
  folly::F14FastSet<folly::CIDRNetwork> prefixes;
  prefixes.reserve(pending.prefixes.size() + incoming.prefixes.size());
  for (const auto& prefix : pending.prefixes) {
    prefixes.emplace(toIPNetwork(*prefix.prefix()));
  }
  for (auto& prefix : incoming.prefixes) {
    if (prefixes.emplace(toIPNetwork(*prefix.prefix())).second) {
      pending.prefixes.emplace_back(std::move(prefix));
    }
  }

  folly::F14FastSet<folly::CIDRNetwork> prefixEntries;
  prefixEntries.reserve(
      pending.prefixEntries.size() + incoming.prefixEntries.size());
  for (const auto& prefixEntry : pending.prefixEntries) {
    prefixEntries.emplace(prefixEntry.network);
  }
  for (auto& prefixEntry : incoming.prefixEntries) {
    if (prefixEntries.emplace(prefixEntry.network).second) {
      pending.prefixEntries.emplace_back(std::move(prefixEntry));
    }
  }
}

inline messaging::RQueue<PrefixEvent>
getPrefixUpdatesQueueReader(
    messaging::ReplicateQueue<PrefixEvent>& queue, bool enableQueueCoalescing) {
  if (!enableQueueCoalescing) {
    return queue.getReader("prefixManager");
  }
  return queue.getReader(
      "prefixManager",
      messaging::StateSuppressionPolicy<PrefixEvent>{
          getPrefixUpdateStateSuppressionKey,
          0 /* activationThreshold */,
          mergePrefixWithdrawIntoPending});
}

} // namespace openr
