/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <variant>

#include <boost/heap/priority_queue.hpp>
#include <boost/serialization/strong_typedef.hpp>
#include <fmt/core.h>
#include <folly/Expected.h>
#include <folly/IPAddress.h>
#include <re2/re2.h>
#include <re2/set.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/policy/PolicyStructs.h>

namespace openr {

//
// Aliases for data-structures
//
using NodeAndArea = std::pair<std::string, std::string>;
using PrefixEntries =
    std::unordered_map<NodeAndArea, std::shared_ptr<thrift::PrefixEntry>>;

// KvStore URLs
BOOST_STRONG_TYPEDEF(std::string, KvStoreGlobalCmdUrl);

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

  /**
   * Neighbor is UP with adjacency database synced
   */
  NEIGHBOR_ADJ_SYNCED = 6,
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

using NeighborEvents = std::vector<NeighborEvent>;

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

struct TtlCountdownQueueEntry {
  std::chrono::steady_clock::time_point expiryTime;
  std::string key;
  int64_t version{0};
  int64_t ttlVersion{0};
  std::string originatorId;

  bool
  operator>(TtlCountdownQueueEntry other) const {
    return expiryTime > other.expiryTime;
  }
};

// TODO: migrate to std::priority_queue
using TtlCountdownQueue = boost::heap::priority_queue<
    TtlCountdownQueueEntry,
    // Always returns smallest first
    boost::heap::compare<std::greater<TtlCountdownQueueEntry>>,
    boost::heap::stable<true>>;

/**
 * Prefix entry with their destination areas and nexthops
 * if dstAreas become empty, entry should be withdrawn.
 */
struct PrefixEntry {
  /**
   * Prefix attributes that needs to be advertised to KvStore. Area may copy &
   * modify these attributes before advertisement.
   */
  std::shared_ptr<thrift::PrefixEntry> tPrefixEntry;
  /**
   * Set of area IDs to which this prefix should be advertised. Leave empty to
   * advertise to all configured areas
   */
  std::unordered_set<std::string> dstAreas;
  /**
   * CIDR network of the prefix
   */
  folly::CIDRNetwork network;
  /**
   * Optional list of next-hops that should be programmed if this route
   * qualifies for advertisement to KvStore.
   * - Null would indicate no programming
   * - An empty list could indicate a NULL route programming
   */
  std::optional<std::unordered_set<thrift::NextHopThrift>> nexthops;
  /**
   * Optional data applied in area policy actions when advertising the prefix to
   * KvStore.
   */
  std::optional<OpenrPolicyActionData> policyActionData{std::nullopt};

  OpenrPolicyMatchData policyMatchData{};

  PrefixEntry() = default;
  PrefixEntry(
      std::shared_ptr<thrift::PrefixEntry>&& tPrefixEntryIn,
      std::unordered_set<std::string>&& dstAreas,
      std::optional<OpenrPolicyActionData> policyActionData = std::nullopt,
      OpenrPolicyMatchData policyMatchData = OpenrPolicyMatchData())
      : tPrefixEntry(std::move(tPrefixEntryIn)),
        dstAreas(std::move(dstAreas)),
        network(toIPNetwork(*tPrefixEntry->prefix_ref())),
        policyActionData(policyActionData),
        policyMatchData(policyMatchData) {}

  PrefixEntry(
      std::shared_ptr<thrift::PrefixEntry>&& tPrefixEntryIn,
      std::unordered_set<std::string>&& dstAreas,
      std::optional<std::unordered_set<thrift::NextHopThrift>> nexthops)
      : tPrefixEntry(std::move(tPrefixEntryIn)),
        dstAreas(std::move(dstAreas)),
        network(toIPNetwork(*tPrefixEntry->prefix_ref())),
        nexthops(std::move(nexthops)) {}

  apache::thrift::field_ref<const thrift::PrefixMetrics&>
  metrics_ref() const& {
    return tPrefixEntry->metrics_ref();
  }

  /*
   * Util function for route-agg check
   * Note that route will only be installed to fib when both installToFib is set
   * to true and has valid nexthops.
   */
  bool
  shouldInstall() const {
    return nexthops.has_value();
  }

  bool
  operator==(const PrefixEntry& other) const {
    return *tPrefixEntry == *other.tPrefixEntry && dstAreas == other.dstAreas &&
        network == other.network && policyMatchData == other.policyMatchData;
  }
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
   * Source of prefix update request. Each source must use a unique type.
   */
  thrift::PrefixType type;

  /**
   * List of prefix-entries of above `type` to advertise or withdraw.
   */
  std::vector<thrift::PrefixEntry> prefixes{};

  /**
   * List of PrefixEntry to advertise or withdraw
   */
  std::vector<PrefixEntry> prefixEntries{};

  /**
   * Destination areas to inject prefixes to
   * ATTN: empty list = inject to all configured areas
   */
  std::unordered_set<std::string> dstAreas{};

  /**
   * Origination policy to be ran before applying area policy
   */
  std::optional<std::string> policyName = std::nullopt;

  explicit PrefixEvent(
      const PrefixEventType& eventType,
      const thrift::PrefixType type,
      std::vector<thrift::PrefixEntry> prefixes = {},
      std::unordered_set<std::string> dstAreas = {},
      const std::optional<std::string>& policyName = std::nullopt)
      : eventType(eventType),
        type(type),
        prefixes(std::move(prefixes)),
        dstAreas(std::move(dstAreas)),
        policyName(policyName) {}
};

/**
 * Structure defining KvStore peer update event in one area.
 */
struct AreaPeerEvent {
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

  explicit AreaPeerEvent(
      const thrift::PeersMap& peersToAdd,
      const std::vector<std::string>& peersToDel)
      : peersToAdd(peersToAdd), peersToDel(peersToDel) {}
};

/**
 * Define peer update event in all areas.
 */
using PeerEvent = std::unordered_map<std::string, AreaPeerEvent>;

/**
 * Advertise the key-value into consumer with specified version.
 */
class SetKeyValueRequest {
 public:
  SetKeyValueRequest(
      const AreaId& area,
      const std::string& key,
      const std::string& value,
      const uint32_t version = 0)
      : area(area), key(key), value(value), version(version) {}

  inline AreaId const&
  getArea() const {
    return area;
  }

  inline std::string const&
  getKey() const {
    return key;
  }

  inline std::string const&
  getValue() const {
    return value;
  }

  inline uint32_t const&
  getVersion() const {
    return version;
  }

 private:
  /**
   * Area identifier.
   */
  AreaId area;
  /**
   * Key to advertise to the consumer.
   */
  std::string key;
  /**
   * Value to advertise to the consumer.
   */
  std::string value;
  /**
   * Version of the key-value pair advertised. Consumer uses the version to
   * determine which value to keep/advertise in case of conflict.
   *
   * If version is not specified, the one greater than the latest known
   * will be used.
   */
  uint32_t version;
};

/**
 * Authoritative request to set specified key-value into consumer.
 *
 * If someone else advertises the same key we try to win over it
 * by re-advertising this key-value with a higher version.
 */
class PersistKeyValueRequest {
 public:
  PersistKeyValueRequest(
      const AreaId& area, const std::string& key, const std::string& value)
      : area(area), key(key), value(value) {}

  inline AreaId const&
  getArea() const {
    return area;
  }

  inline std::string const&
  getKey() const {
    return key;
  }

  inline std::string const&
  getValue() const {
    return value;
  }

 private:
  /**
   * Area identifier. By default key is published to default area kvstore
   * instance.
   */
  AreaId area;
  /**
   * Key to advertise to the consumer.
   */
  std::string key;
  /**
   * Value to advertise to the consumer.
   */
  std::string value;
};

/**
 * Request to erase key-value from consumer by setting default value of empty
 * string or value passed by the caller, cancel ttl timers, and advertise
 * with higher version.
 */
class ClearKeyValueRequest {
 public:
  ClearKeyValueRequest(
      const AreaId& area,
      const std::string& key,
      const std::string& value = "",
      const bool setValue = false)
      : area(area), key(key), value(value), setValue(setValue) {
    // Value is required if `setValue` flag is true.
    CHECK(not(setValue && value.empty()))
        << "Must specify value in ClearKeyValueRequest when setValue flag is set to true.";
  }

  inline AreaId const&
  getArea() const {
    return area;
  }

  inline std::string const&
  getKey() const {
    return key;
  }

  inline std::string const&
  getValue() const {
    return value;
  }

  inline bool const&
  getSetValue() const {
    return setValue;
  }

 private:
  /**
   * Area identifier.
   */
  AreaId area;
  /**
   * Key to clear from consumer.
   */
  std::string key;
  /**
   * New value to set to key in consumer.
   */
  std::string value;
  /**
   * Flag to set a new value for given key. Value must be provided.
   */
  bool setValue{false};
};

using KeyValueRequest = std::
    variant<SetKeyValueRequest, PersistKeyValueRequest, ClearKeyValueRequest>;
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
 * Structure representing:
 *  1) thrift::Publication;
 *  2) Open/R Initialization event;
 */
using KvStorePublication = std::variant<
    thrift::Publication /* publication reflecting key-val change */,
    thrift::InitializationEvent /* KVSTORE_SYNCED */>;

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
  // constructor using node, prefix and area
  PrefixKey(
      std::string const& node,
      folly::CIDRNetwork const& prefix,
      const std::string& area);

  // construct PrefixKey object from a give key string
  static folly::Expected<PrefixKey, std::string> fromStr(
      const std::string& key,
      const std::string& area = Constants::kDefaultArea.toString());

  static const RE2&
  getPrefixRE2V2() {
    static const RE2 prefixKeyPatternV2{fmt::format(
        "{}(?P<node>[a-zA-Z\\d\\.\\-\\_]+):"
        "\\[(?P<IPAddr>[a-fA-F\\d\\.\\:]+)/"
        "(?P<plen>[\\d]{{1,3}})\\]",
        Constants::kPrefixDbMarker.toString())};
    return prefixKeyPatternV2;
  }

  // return node name and area pair
  inline NodeAndArea const&
  getNodeAndArea() const {
    return nodeAndArea_;
  }

  // return node name
  inline std::string const&
  getNodeName() const {
    return nodeAndArea_.first;
  }

  // return prefix sub type
  inline std::string const&
  getPrefixArea() const {
    return nodeAndArea_.second;
  }

  // return the CIDR network address
  inline folly::CIDRNetwork const&
  getCIDRNetwork() const {
    return prefix_;
  }

  // return raw prefix key string v2 from kvstore
  inline std::string const&
  getPrefixKeyV2() const {
    return prefixKeyStringV2_;
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

  // raw key string from KvStore
  std::string const prefixKeyStringV2_;
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
