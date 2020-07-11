/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <folly/IPAddress.h>
#include <openr/common/Util.h>
#include <openr/decision/RibEntry.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace openr {

// Route updates published by Decision
// consumed by PrefixManager, BgpSpeaker, Fib.
struct DecisionRouteUpdate {
  std::vector<RibUnicastEntry> unicastRoutesToUpdate;
  std::vector<folly::CIDRNetwork> unicastRoutesToDelete;
  std::vector<RibMplsEntry> mplsRoutesToUpdate;
  std::vector<int32_t> mplsRoutesToDelete;
  std::optional<thrift::PerfEvents> perfEvents = std::nullopt;

  thrift::RouteDatabaseDelta
  toThrift() {
    thrift::RouteDatabaseDelta delta;

    // unicast
    for (const auto& route : unicastRoutesToUpdate) {
      delta.unicastRoutesToUpdate.emplace_back(route.toThrift());
    }
    for (const auto& route : unicastRoutesToDelete) {
      delta.unicastRoutesToDelete.emplace_back(toIpPrefix(route));
    }
    // mpls
    for (const auto& route : mplsRoutesToUpdate) {
      delta.mplsRoutesToUpdate.emplace_back(route.toThrift());
    }
    delta.mplsRoutesToDelete = mplsRoutesToDelete;
    fromStdOptional(delta.perfEvents_ref(), perfEvents);

    return delta;
  }
};

} // namespace openr
