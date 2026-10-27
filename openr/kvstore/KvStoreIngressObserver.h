/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <string>

namespace openr {

/*
 * Told how many bytes of peer updates are waiting for KvStore, by the peer
 * that sent them.
 */
class KvStoreIngressObserver {
 public:
  virtual ~KvStoreIngressObserver() = default;
  /*
   * `bytes` of updates from `peerName` are now waiting for KvStore to merge
   * them.
   */
  virtual void addOpenrIngressKvStorePendingMergeBytes(
      const std::string& peerName, size_t bytes) = 0;
  /*
   * `bytes` of updates from `peerName` are merged, replaced or dropped, and no
   * longer waiting.
   */
  virtual void releaseOpenrIngressKvStorePendingMergeBytes(
      const std::string& peerName, size_t bytes) = 0;
};

} // namespace openr
