/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/gen/Base.h>
#include <folly/logging/xlog.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/link-monitor/AdjacencyEntry.h>

namespace openr {

AdjacencyEntry::AdjacencyEntry(
    AdjacencyKey const& adjKey,
    thrift::PeerSpec const& peerSpec,
    thrift::Adjacency const& adj,
    int32_t baseMetric,
    bool isRestarting,
    bool onlyUsedByOtherNode)
    : adjKey_(adjKey),
      peerSpec_(peerSpec),
      adj_(adj),
      baseMetric_(baseMetric),
      isRestarting_(isRestarting),
      onlyUsedByOtherNode_(onlyUsedByOtherNode) {}

bool
AdjacencyEntry::isActive() {
  const auto lastErrorTime = backoff_.getLastErrorTime();
  const auto now = std::chrono::steady_clock::now();
  if (now - lastErrorTime > backoff_.getMaxBackoff()) {
    backoff_.reportSuccess();
  }
  return backoff_.canTryNow();
}

std::chrono::milliseconds
AdjacencyEntry::getBackoffDuration() const {
  return backoff_.getTimeRemainingUntilRetry();
}

} // namespace openr
