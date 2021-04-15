/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
#include <fmt/core.h>
#include <folly/Expected.h>
#include <folly/IPAddress.h>
#include <re2/re2.h>
#include <re2/set.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

//
// Aliases for data-structures
//
using NodeAndArea = std::pair<std::string, std::string>;
using PrefixEntries =
    std::unordered_map<NodeAndArea, std::shared_ptr<thrift::PrefixEntry>>;

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
 * Enum indicating type of event to LinkMonitor. Only used for inter
 * module (within a process) communication.
 */
enum class NeighborEventType {
  /**
   * Neighbor UP event
   */
  NEIGHBOR_UP = 1,

  /**
   * Neighbor DOWN event
   */
  NEIGHBOR_DOWN = 2,

  /**
   * Neighbor comes back from graceful restart(GR) mode
   */
  NEIGHBOR_RESTARTED = 3,

  /**
   * Round-Trip-Time(RTT) changed for neighbor
   */
  NEIGHBOR_RTT_CHANGE = 4,

  /**
   * Neighbor goes into graceful restart(GR) mode
   */
  NEIGHBOR_RESTARTING = 5,
};

/**
 * Event for indicating neighbor UP/DOWN/RESTART to LinkMonitor, containing
 * ENUM type with detailed neighbor information. Only used for inter module
 * (within a process) communication.
 */
struct NeighborEvent {
  /**
   * ENUM type of event
   */
  NeighborEventType eventType;

  /**
   * Detailed information about neighbor from Spark
   */
  thrift::SparkNeighbor info;

  NeighborEvent(
      const NeighborEventType& eventType, const thrift::SparkNeighbor& info)
      : eventType(eventType), info(info) {}
};

/**
 * Enum indicating type of request to PrefixManager. Only used for inter
 * module (within a process) communication.
 */
enum class PrefixEventType {
  /**
   * Add listed prefixes for advertisement of specified type
   */
  ADD_PREFIXES = 1,

  /**
   * Withdraw listed prefixes of specified type
   */
  WITHDRAW_PREFIXES = 2,

  /**
   * Withdraw ALL existing prefixes of specified type
   */
  WITHDRAW_PREFIXES_BY_TYPE = 3,

  /**
   * Replace existing prefixes with new list of prefixes of specified type. The
   * PrefixManager will compute delta and gracefully apply it. e.g. specifying
   * same list of prefixes twice for SYNC will result in no updates second time.
   */
  SYNC_PREFIXES_BY_TYPE = 4,
};

/**
 * Request for advertising, updating or withdrawing route advertisements to
 * PrefixManager. Only used for inter module (within a process) communication.
 */
struct PrefixEvent {
  /**
   * ENUM type of event
   */
  PrefixEventType eventType;

  /**
   * Source of prefix update request. Each source must use a unique type
   */
  std::optional<thrift::PrefixType> type = std::nullopt;

  /**
   * List of prefix-entries to advertise or withdraw
   */
  std::vector<thrift::PrefixEntry> prefixes{};

  /**
   * Destination areas to inject prefixes to
   * ATTN: empty list = inject to all configured areas
   */
  std::unordered_set<std::string> dstAreas{};

  /**
   * List of originatedPrefix-entries to advertise or withdraw.
   * Support `min_supporting_route` and `install_to_fib` features.
   */
  std::vector<thrift::OriginatedPrefix> originatedPrefixes{};

  explicit PrefixEvent(
      const PrefixEventType& eventType,
      const std::optional<thrift::PrefixType>& type = std::nullopt,
      std::vector<thrift::PrefixEntry> prefixes = {},
      std::unordered_set<std::string> dstAreas = {},
      std::vector<thrift::OriginatedPrefix> originatedPrefixes = {})
      : eventType(eventType),
        type(type),
        prefixes(std::move(prefixes)),
        dstAreas(std::move(dstAreas)),
        originatedPrefixes(std::move(originatedPrefixes)) {}
};

/**
 * Structure defining KvStore peer update event.
 */
struct PeerEvent {
  /**
   * Area identifier
   */
  std::string area{""};

  /**
   * Map from nodeName to peer spec, which is expected to be
   * learnt from Spark neighbor discovery. Information will
   * be used for TCP session establishing between KvStore.
   */
  thrift::PeersMap peersToAdd{};

  /**
   * List of nodeName to delete
   */
  std::vector<std::string> peersToDel{};

  explicit PeerEvent(
      const std::string& area,
      const thrift::PeersMap& peersToAdd,
      const std::vector<std::string>& peersToDel)
      : area(area), peersToAdd(peersToAdd), peersToDel(peersToDel) {}
};

/**
 * Structure to represent interface information from the system, including
 * link status/addresses/etc.
 */
struct InterfaceInfo {
  /**
   * Interface name
   */
  std::string ifName{""};

  /**
   * Link status
   */
  bool isUp{false};

  /**
   * Interface index
   */
  int64_t ifIndex{-1};

  /**
   * List of networks associated with this interface
   */
  std::unordered_set<folly::CIDRNetwork> networks{};

  InterfaceInfo() {}

  InterfaceInfo(
      const std::string& ifName,
      const bool isUp,
      const int64_t ifIndex,
      const std::unordered_set<folly::CIDRNetwork>& networks)
      : ifName(ifName), isUp(isUp), ifIndex(ifIndex), networks(networks) {}

  inline bool
  operator==(const InterfaceInfo& other) const {
    return (ifName == other.ifName) and (isUp == other.isUp) and
        (ifIndex == other.ifIndex) and (networks == other.networks);
  }

  // Utility function to retrieve v4 addresses
  inline std::set<folly::CIDRNetwork>
  getSortedV4Addrs() const {
    std::set<folly::CIDRNetwork> v4Addrs;
    for (auto const& ntwk : networks) {
      if (ntwk.first.isV4()) {
        v4Addrs.insert(ntwk);
      }
    }
    return v4Addrs;
  }

  // Utility function to retrieve v6 link local addresses
  inline std::set<folly::CIDRNetwork>
  getSortedV6LinkLocalAddrs() const {
    std::set<folly::CIDRNetwork> v6Addrs;
    for (auto const& ntwk : networks) {
      if (ntwk.first.isV6() and ntwk.first.isLinkLocal()) {
        v6Addrs.insert(ntwk);
      }
    }
    return v6Addrs;
  }

  // TODO: thrift::InterfaceInfo to be deprecated
  thrift::InterfaceInfo
  toThrift() const {
    std::vector<thrift::IpPrefix> prefixes;
    for (const auto& network : networks) {
      prefixes.emplace_back(toIpPrefix(network));
    }

    thrift::InterfaceInfo info;
    info.isUp_ref() = isUp;
    info.ifIndex_ref() = ifIndex;
    info.networks_ref() = std::move(prefixes);
    return info;
  }
};

/**
 * Structure of the entire interface snapshot in the system
 */
using InterfaceDatabase = std::vector<InterfaceInfo>;

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
    static const RE2 prefixKeyPattern{fmt::format(
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
