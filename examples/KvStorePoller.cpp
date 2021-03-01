/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/Constants.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/public_tld/examples/KvStorePoller.h>

namespace openr {

KvStorePoller::KvStorePoller(std::vector<folly::SocketAddress>& sockAddrs)
    : sockAddrs_(sockAddrs) {}

std::pair<
    std::optional<std::unordered_map<std::string, thrift::AdjacencyDatabase>>,
    std::vector<folly::SocketAddress> /* unreached url */>
KvStorePoller::getAdjacencyDatabases(std::chrono::milliseconds pollTimeout) {
  return openr::dumpAllWithPrefixMultipleAndParse<thrift::AdjacencyDatabase>(
      AreaId{"my_area_name"},
      sockAddrs_,
      Constants::kAdjDbMarker.toString(),
      Constants::kServiceConnTimeout,
      pollTimeout);
}

std::pair<
    std::optional<std::unordered_map<std::string, thrift::PrefixDatabase>>,
    std::vector<folly::SocketAddress> /* unreached url */>
KvStorePoller::getPrefixDatabases(std::chrono::milliseconds pollTimeout) {
  return openr::dumpAllWithPrefixMultipleAndParse<thrift::PrefixDatabase>(
      AreaId{"my_area_name"},
      sockAddrs_,
      Constants::kPrefixDbMarker.toString(),
      Constants::kServiceConnTimeout,
      pollTimeout);
}

} // namespace openr
