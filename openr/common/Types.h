/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/serialization/strong_typedef.hpp>
#include <folly/Expected.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <re2/re2.h>
#include <re2/set.h>

#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

//
// Aliases for data-structures
//
using NodeAndArea = std::pair<std::string, std::string>;
using PrefixEntries = std::unordered_map<NodeAndArea, thrift::PrefixEntry>;

// KvStore URLs
BOOST_STRONG_TYPEDEF(std::string, KvStoreGlobalCmdUrl);

// KvStore TCP ports
BOOST_STRONG_TYPEDEF(uint16_t, KvStoreCmdPort);

// OpenrCtrl Thrift port
BOOST_STRONG_TYPEDEF(uint16_t, OpenrCtrlThriftPort);

// markers for some of KvStore keys
BOOST_STRONG_TYPEDEF(std::string, AdjacencyDbMarker);
BOOST_STRONG_TYPEDEF(std::string, PrefixDbMarker);
BOOST_STRONG_TYPEDEF(std::string, AllocPrefixMarker);

BOOST_STRONG_TYPEDEF(std::string, AreaId);

/**
 * Structure defining neighbor event, which consists of:
 *  - Event Type (ENUM)
 *  - Info (Thrift struct)
 *
 * NeighborEvent is used uni-directionally from
 * `Spark` -> `LinkMonitor` to indicate events like:
 *  - adjacency change
 *  - rtt change
 *  - GR
 */
enum class NeighborEventType {
  NEIGHBOR_UP = 1,
  NEIGHBOR_DOWN = 2,
  NEIGHBOR_RESTARTED = 3,
  NEIGHBOR_RTT_CHANGE = 4,
  NEIGHBOR_RESTARTING = 5,
};

struct NeighborEvent {
  NeighborEventType eventType;
  thrift::SparkNeighbor info;

  NeighborEvent(
      const NeighborEventType& eventType, const thrift::SparkNeighbor& info)
      : eventType(eventType), info(info) {}
};

/**
 * Structure defining prefix update event, which consists of:
 *  - Event Type (ENUM)
 *  - Prefix Type (ENUM)
 *  - prefixes: a vector of thrift::PrefixEntry
 *  - dstAreas: a set of string indicating destination areas
 */
enum class PrefixEventType {
  ADD_PREFIXES = 1,
  WITHDRAW_PREFIXES = 2,
  WITHDRAW_PREFIXES_BY_TYPE = 3,
  SYNC_PREFIXES_BY_TYPE = 4,
};

struct PrefixEvent {
  PrefixEventType eventType;
  std::optional<thrift::PrefixType> type;
  std::vector<thrift::PrefixEntry> prefixes;
  std::unordered_set<std::string> dstAreas;

  explicit PrefixEvent(
      const PrefixEventType& eventType,
      const std::optional<thrift::PrefixType>& type = std::nullopt,
      const std::vector<thrift::PrefixEntry>& prefixes = {},
      const std::unordered_set<std::string>& dstAreas = {})
      : eventType(eventType),
        type(type),
        prefixes(prefixes),
        dstAreas(dstAreas) {}
};

/**
 * Structure defining KvStore peer sync event, published to subscribers.
 * LinkMonitor subscribes this event for signal adjancency UP event propagation
 */
struct KvStoreSyncEvent {
  std::string nodeName;
  std::string area;

  KvStoreSyncEvent(const std::string& nodeName, const std::string& area)
      : nodeName(nodeName), area(area) {}

  inline bool
  operator==(const KvStoreSyncEvent& other) const {
    return (nodeName == other.nodeName) && (area == other.area);
  }
};

/**
 * Provides match capability on list of regexes. Will default to prefix match
 * if regex is normal string.
 */
class RegexSet {
 public:
  /**
   * Create regex set from list of regexes
   */
  explicit RegexSet(std::vector<std::string> const& regexOrPrefixList);

  /**
   * Match key with regex set
   */
  bool match(std::string const& key) const;

 private:
  std::unique_ptr<re2::RE2::Set> regexSet_;
};

/**
 * PrefixKey class to form and parse a PrefixKey. PrefixKey can be instantiated
 * by passing parameters to form a key, or by passing the key string to parse
 * and populate the parameters. In case the parsing fails all the parameters
 * are set to std::nullopt
 */
class PrefixKey {
 public:
  // constructor using IP address, type and and subtype
  PrefixKey(
      std::string const& node,
      folly::CIDRNetwork const& prefix,
      const std::string& area);

  // construct PrefixKey object from a give key string
  static folly::Expected<PrefixKey, std::string> fromStr(
      const std::string& key);

  NodeAndArea const& getNodeAndArea() const;

  // return node name
  std::string const& getNodeName() const;

  // return the CIDR network address
  folly::CIDRNetwork const& getCIDRNetwork() const;

  // return prefix sub type
  std::string const& getPrefixArea() const;

  // return prefix key string to be used to flood to kvstore
  std::string const& getPrefixKey() const;

  // return thrift::IpPrefix
  thrift::IpPrefix getIpPrefix() const;

  static const RE2&
  getPrefixRE2() {
    static const RE2 prefixKeyPattern{folly::sformat(
        "{}(?P<node>[a-zA-Z\\d\\.\\-\\_]+):"
        "(?P<area>[a-zA-Z0-9\\.\\_\\-]+):"
        "\\[(?P<IPAddr>[a-fA-F\\d\\.\\:]+)/"
        "(?P<plen>[\\d]{{1,3}})\\]",
        Constants::kPrefixDbMarker.toString())};
    return prefixKeyPattern;
  }

  bool
  operator==(openr::PrefixKey const& other) const {
    return prefix_ == other.prefix_ && nodeAndArea_ == other.nodeAndArea_;
  }

 private:
  // node name
  NodeAndArea const nodeAndArea_;

  // IP address
  folly::CIDRNetwork const prefix_;

  // key string used for KvStore
  std::string const prefixKeyString_;
};

} // namespace openr

template <>
struct std::hash<openr::AreaId> {
  size_t
  operator()(openr::AreaId const& areaId) const {
    return hash<string>()(areaId);
  }
};

template <>
struct std::hash<openr::PrefixKey> {
  size_t
  operator()(openr::PrefixKey const& prefixKey) const {
    return folly::hash::hash_combine(
        prefixKey.getNodeName(),
        prefixKey.getCIDRNetwork(),
        prefixKey.getPrefixArea());
  }
};
