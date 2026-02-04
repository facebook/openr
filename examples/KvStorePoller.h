/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/SocketAddress.h>
#include <folly/container/F14Map.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

class KvStorePoller {
 public:
  /* implicit */ KvStorePoller(std::vector<folly::SocketAddress>& sockAddrs);

  ~KvStorePoller() = default;

  std::pair<
      std::optional<folly::F14FastMap<std::string, thrift::AdjacencyDatabase>>,
      std::vector<folly::SocketAddress> /* unreachable url */>
  getAdjacencyDatabases(std::chrono::milliseconds pollTimeout);

  std::pair<
      std::optional<folly::F14FastMap<std::string, thrift::PrefixDatabase>>,
      std::vector<folly::SocketAddress> /* unreachable url */>
  getPrefixDatabases(std::chrono::milliseconds pollTimeout);

 private:
  std::vector<folly::SocketAddress> sockAddrs_;

}; // class KvStorePoller
} // namespace openr
