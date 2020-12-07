/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <set>
#include <unordered_map>
#include <vector>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/Decision_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>

namespace openr {

class PrefixState {
 public:
  std::unordered_map<thrift::IpPrefix, PrefixEntries> const&
  prefixes() const {
    return prefixes_;
  }

  // returns set of changed prefixes (i.e. a node started advertising or any
  // attributes changed)
  std::unordered_set<thrift::IpPrefix> updatePrefix(
      PrefixKey const& key, thrift::PrefixEntry const& entry);

  // returns set of changed prefixes (i.e. a node withdrew a prefix) will be
  // empty if node/area did not previosuly advertise
  std::unordered_set<thrift::IpPrefix> deletePrefix(PrefixKey const& key);

  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
  getPrefixDatabases() const;

  std::vector<thrift::ReceivedRouteDetail> getReceivedRoutesFiltered(
      thrift::ReceivedRouteFilter const& filter) const;

  /**
   * Filter routes only the <type> attribute
   */
  static void filterAndAddReceivedRoute(
      std::vector<thrift::ReceivedRouteDetail>& routes,
      apache::thrift::optional_field_ref<const std::string&> const& nodeFilter,
      apache::thrift::optional_field_ref<const std::string&> const& areaFilter,
      thrift::IpPrefix const& prefix,
      PrefixEntries const& prefixEntries);

  /**
   * Function returns true if all prefix entries do not agree on the same
   * forwarding algorithm and type.
   */
  static bool hasConflictingForwardingInfo(PrefixEntries const& prefixEntries);

 private:
  // TODO: Also maintain clean list of reachable prefix entries. A node might
  // become un-reachable we might still have their prefix entries, until gets
  // expired in KvStore. This will simplify logic in route computation where
  // we exclude unreachable nodes.

  // TODO: Maintain shared_ptr for `thrift::PrefixEntry` within
  // `PrefixEntries` to avoid data-copy for best metric selection and
  // route re-distribution

  // Data structure to maintain mapping from:
  //  IpPrefix -> collection of originator(i.e. [node, area] combination)
  std::unordered_map<thrift::IpPrefix, PrefixEntries> prefixes_;

  // (Reverse Mapping) Data structure to maintain mapping from:
  //  [node, area] combination -> set of IpPrefix
  std::unordered_map<NodeAndArea, std::set<thrift::IpPrefix>> nodeToPrefixes_;
};
} // namespace openr
