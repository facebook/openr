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
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

class PrefixState {
 public:
  std::unordered_map<folly::CIDRNetwork, PrefixEntries> const&
  prefixes() const {
    return prefixes_;
  }

  // returns set of changed prefixes (i.e. a node started advertising or any
  // attributes changed)
  folly::F14FastSet<folly::CIDRNetwork> updatePrefix(
      PrefixKey const& key, thrift::PrefixEntry const& entry);

  // returns set of changed prefixes (i.e. a node withdrew a prefix) will be
  // empty if node/area did not previosuly advertise
  folly::F14FastSet<folly::CIDRNetwork> deletePrefix(PrefixKey const& key);

  std::vector<thrift::ReceivedRouteDetail> getReceivedRoutesFiltered(
      thrift::ReceivedRouteFilter const& filter) const;

  /**
   * Filter routes only the <type> attribute
   */
  static void filterAndAddReceivedRoute(
      std::vector<thrift::ReceivedRouteDetail>& routes,
      apache::thrift::optional_field_ref<const std::string&> const& nodeFilter,
      apache::thrift::optional_field_ref<const std::string&> const& areaFilter,
      folly::CIDRNetwork const& prefix,
      PrefixEntries const& prefixEntries);

 private:
  // TODO: Also maintain clean list of reachable prefix entries. A node might
  // become un-reachable we might still have their prefix entries, until gets
  // expired in KvStore. This will simplify logic in route computation where
  // we exclude unreachable nodes.

  // Data structure to maintain mapping from:
  //  IpPrefix -> collection of originator(i.e. [node, area] combination)
  std::unordered_map<folly::CIDRNetwork, PrefixEntries> prefixes_;
};
} // namespace openr
