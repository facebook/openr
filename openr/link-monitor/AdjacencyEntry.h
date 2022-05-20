/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncTimeout.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/LsdbTypes.h>

namespace openr {

/**
 * Holds adjacency attributes along with complex information like backoffs. All
 * updates made into this object is reflected asynchronously via throttled
 * callback or timers provided in constructor.
 *
 * - Any change will always trigger throttled callback
 * - Adjacency transition from Active to Inactive schedules immediate timeout
 *   for fast reactions to DOWN events.
 */
class AdjacencyEntry final {
 public:
  AdjacencyEntry(
      AdjacencyKey const& adjKey,
      thrift::PeerSpec const& peerSpec,
      thrift::Adjacency const& adj,
      int32_t baseMetric,
      bool isRestarting = false,
      bool onlyUsedByOtherNode = false);

  /*
   * Util method to check if adjacency is active.
   *
   * Adjacency is ONLY active when it is NOT in backed off state.
   */
  bool isActive();

  /*
   * Util method to get backoff time
   */
  std::chrono::milliseconds getBackoffDuration() const;

  // Backoff variables
  ExponentialBackoff<std::chrono::milliseconds> backoff_;

  // Data-structure representing adj key and value information
  AdjacencyKey adjKey_;
  thrift::PeerSpec peerSpec_;
  thrift::Adjacency adj_;

  // metric info from Spark module
  int32_t baseMetric_;

  // flag for WARM_BOOT(GR) and COLD_BOOT processing
  bool isRestarting_{false};
  bool onlyUsedByOtherNode_{false};
};

} // namespace openr
