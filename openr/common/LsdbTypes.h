/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <variant>

#include <boost/serialization/strong_typedef.hpp>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/policy/PolicyStructs.h>
#include <re2/re2.h>

namespace openr {

//
// Aliases for data-structures
//

using AdjacencyKey = std::
    pair<std::string /* remoteNodeName */, std::string /* localInterfaceName*/>;
using NodeAndArea = std::pair<std::string, std::string>;
using PrefixEntries =
    std::unordered_map<NodeAndArea, std::shared_ptr<thrift::PrefixEntry>>;

// markers for some of KvStore keys
BOOST_STRONG_TYPEDEF(std::string, AdjacencyDbMarker);
BOOST_STRONG_TYPEDEF(std::string, PrefixDbMarker);
BOOST_STRONG_TYPEDEF(std::string, AllocPrefixMarker);

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
   * Spark neighbor name
   */
  std::string remoteNodeName;

  /**
   * v4/v6 neighbor address
   */
  thrift::BinaryAddress neighborAddrV4;
  thrift::BinaryAddress neighborAddrV6;

  /**
   * local/remote interface name
   */
  std::string localIfName;
  std::string remoteIfName;

  /**
   * areaId in which this neighbor is discovered
   */
  std::string area;

  /**
   * port information for TCP connection establishement
   */
  int32_t ctrlThriftPort;

  /**
   * round-trip-time(RTT) for this neighbor
   */
  int64_t rttUs;

  /**
   * misc flags
   */
  bool adjOnlyUsedByOtherNode{false};

  NeighborEvent(
      const NeighborEventType& eventType,
      const std::string& nodeName,
      const thrift::BinaryAddress& v4Addr,
      const thrift::BinaryAddress& v6Addr,
      const std::string& localIfName,
      const std::string& remoteIfName,
      const std::string& area,
      int32_t ctrlThriftPort,
      int64_t rttUs,
      bool adjOnlyUsedByOtherNode = false)
      : eventType(eventType),
        remoteNodeName(nodeName),
        neighborAddrV4(v4Addr),
        neighborAddrV6(v6Addr),
        localIfName(localIfName),
        remoteIfName(remoteIfName),
        area(area),
        ctrlThriftPort(ctrlThriftPort),
        rttUs(rttUs),
        adjOnlyUsedByOtherNode(adjOnlyUsedByOtherNode) {}
};

/**
 * TODO: Deprecate NeighborEvents and use NeighborEvent in NeighborInitEvent
 * directly.
 * Dependency: Complete the migration from batching neighbor update events to
 * incremental neighbor update events.
 */
using NeighborEvents = std::vector<NeighborEvent>;

/**
 * Structure representing:
 *  1) KvStoreSyncEvent;
 *  2) Open/R Initialization event;
 */
using NeighborInitEvent = std::variant<
    NeighborEvents /* Neighbor sync event */,
    thrift::InitializationEvent /* KVSTORE_SYNCED or KVSTORE_SYNC_ERROR */>;

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
 * Event for indicating that a IPv6 addr's resolvabllity to Spark, with
 * the interface information. Only used for inter module communication
 */
struct AddressEvent {
  // Whether is address is resolvable (can be reached directly).
  bool resolvable = false;

  // The IPv6 Address of intrest.
  thrift::BinaryAddress v6Addr;

  // The interface name that this address belong to.
  std::string ifName;
};

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

  /* True if decision have already made calculations with the local routes and
   * concluded that this is the best route
   */
  bool preferredForRedistribution{false};

  PrefixEntry() = default;
  PrefixEntry(
      std::shared_ptr<thrift::PrefixEntry>&& tPrefixEntryIn,
      std::unordered_set<std::string>&& dstAreas,
      std::optional<OpenrPolicyActionData> policyActionData = std::nullopt,
      OpenrPolicyMatchData policyMatchData = OpenrPolicyMatchData(),
      bool preferredForRedistribution = false)
      : tPrefixEntry(std::move(tPrefixEntryIn)),
        dstAreas(std::move(dstAreas)),
        network(toIPNetwork(*tPrefixEntry->prefix())),
        policyActionData(policyActionData),
        policyMatchData(policyMatchData),
        preferredForRedistribution(preferredForRedistribution) {}

  PrefixEntry(
      std::shared_ptr<thrift::PrefixEntry>&& tPrefixEntryIn,
      std::unordered_set<std::string>&& dstAreas,
      std::optional<std::unordered_set<thrift::NextHopThrift>> nexthops)
      : tPrefixEntry(std::move(tPrefixEntryIn)),
        dstAreas(std::move(dstAreas)),
        network(toIPNetwork(*tPrefixEntry->prefix())),
        nexthops(std::move(nexthops)) {}

  apache::thrift::field_ref<const thrift::PrefixMetrics&>
  metrics() const& {
    return tPrefixEntry->metrics();
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
        network == other.network && policyMatchData == other.policyMatchData &&
        policyActionData == other.policyActionData;
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
 * Structure to represent interface information from the system, including
 * link status/addresses/etc.
 */
struct InterfaceInfo {
  /**
   * Interface name
   */
  std::string ifName;

  /**
   * Link status
   */
  bool isUp{false};

  /**
   * Unix timestamp at when link status changed.
   */
  int64_t statusChangeTimestamp{0};

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
    return (ifName == other.ifName) && (isUp == other.isUp) &&
        (ifIndex == other.ifIndex) && (networks == other.networks);
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
      if (ntwk.first.isV6() && ntwk.first.isLinkLocal()) {
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
    info.isUp() = isUp;
    info.ifIndex() = ifIndex;
    info.networks() = std::move(prefixes);
    return info;
  }
};

/**
 * Structure of the entire interface snapshot in the system
 */
using InterfaceDatabase = std::vector<InterfaceInfo>;

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
struct std::hash<openr::PrefixKey> {
  size_t
  operator()(openr::PrefixKey const& prefixKey) const {
    return folly::hash::hash_combine(
        prefixKey.getNodeName(),
        prefixKey.getCIDRNetwork(),
        prefixKey.getPrefixArea());
  }
};
