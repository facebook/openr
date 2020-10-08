/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.Lsdb
namespace py openr.Lsdb
namespace py3 openr.thrift
namespace php OpenR_Lsdb
namespace lua openr.Lsdb

include "Network.thrift"
include "KvStore.thrift"
include "OpenrConfig.thrift"

//
// Performance measurement related structs
//

struct PerfEvent {
  1: string nodeName;
  2: string eventDescr;
  3: i64 unixTs = 0;
}

struct PerfEvents {
  1: list<PerfEvent> events;
}

//
// Interfaces
//

//
// InterfaceDb is the entire interface state
// for this system providing link status and
// Ipv4 / IPv6 LinkLocal addresses
// Spark uses this to initiate sessions with neighbors
// and the addresses are used as nextHops
// Fib uses interface state to update RouteDb and program
// next best routes
//
struct InterfaceInfo {
  1: required bool isUp
  2: required i64 ifIndex
  // ip prefixes of all v4 and v6 link local addresses
  5: list<Network.IpPrefix> networks
}

//
// InterfaceDatabase is a map if interfaces
// keyed by ifName as string
//
struct InterfaceDatabase {
  1: string thisNodeName
  2: map<string, InterfaceInfo> interfaces

  // Optional attribute to measure convergence performance
  3: optional PerfEvents perfEvents;
}

//
// Adjacencies
//

// describes a specific adjacency to a neighbor node
struct Adjacency {
  // must match a name bound to another node
  1: string otherNodeName

  // interface for the peer
  2: string ifName

  // peer's link-local addresses
  3: Network.BinaryAddress nextHopV6
  5: Network.BinaryAddress nextHopV4

  // metric to reach to the neighbor
  4: i32 metric

  // SR Adjacency Segment label associated with this adjacency. This is
  // node-local label and programmed by originator only assigned from
  // non-global space. 0 is invalid value
  6: i32 adjLabel = 0

  // Overloaded bit for adjacency. Indicates that this adjacency is not
  // available for any kind of transit traffic
  7: bool isOverloaded = 0

  // rtt to neighbor in us
  8: i32 rtt

  // timestamp at creation time in s since epoch, used to indicate uptime
  9: i64 timestamp;

  // weight of this adj in the network when weighted ECMP is enabled
  10: i64 weight = 1

  // interface the originator (peer) discover this node
  11: string otherIfName = ""
}

// full link state information of a single router
// announced under keys starting with "adj:"
struct AdjacencyDatabase {
  // must use the same name as used in the key
  1: string thisNodeName

  // overload bit. Indicates if node should be use for transit(false)
  // or not(true).
  2: bool isOverloaded = 0

  // all adjacent neighbors for this node
  3: list<Adjacency> adjacencies

  // SR Nodal Segment label associated with this node. This is globally unique
  // label assigned from global static space. 0 is invalid value
  4: i32 nodeLabel

  // Optional attribute to measure convergence performance
  5: optional PerfEvents perfEvents;

  // Adjacency is always a part of an area.
  6: string area
}

//
// Prefixes
//

/**
 * @deprecated
 * Metric entity type
 */
enum MetricEntityType {
  LOCAL_PREFERENCE = 0,
  LOCAL_ROUTE = 1,
  AS_PATH_LEN = 2,
  ORIGIN_CODE = 3,
  EXTERNAL_ROUTE = 4,
  CONFED_EXTERNAL_ROUTE = 5,
  ROUTER_ID = 6,
  CLUSTER_LIST_LEN = 7,
  PEER_IP = 8,
}

/**
 * @deprecated
 * Metric entity priorities.
 * Large gaps are provided so that in future, we can place other fields
 * in between if needed
 */
enum MetricEntityPriority {
  LOCAL_PREFERENCE = 9000,
  LOCAL_ROUTE = 8000,
  AS_PATH_LEN = 7000,
  ORIGIN_CODE = 6000,
  EXTERNAL_ROUTE = 5000,
  CONFED_EXTERNAL_ROUTE = 4000,
  ROUTER_ID = 3000,
  CLUSTER_LIST_LEN = 2000,
  PEER_IP = 1000,
}

// How to compare two MetricEntity
// @deprecated
enum CompareType {
  // if present only in one metric vector, route with this type will win
  WIN_IF_PRESENT = 1,
  // if present only in one metric vector, route without this type will win
  WIN_IF_NOT_PRESENT = 2,
  // if present only in one metric vector,
  // this type will be ignored from comparision and fall through to next
  IGNORE_IF_NOT_PRESENT = 3,
}

// @deprecated
struct MetricEntity {
  // Type identifying each entity. (Used only for identification)
  1: i64 type

  // Priority fields. Initially priorities are assigned as
  // 10000, 9000, 8000, 7000 etc, this enables us to add any priorities
  // in between two fields.
  // Higher value is higher priority.
  2: i64 priority

  // Compare type defines how to handle cases of backward compatibility and
  // scenario's where some fields are not populated
  3: CompareType op

  // All fields without this set will be used for multipath selection
  // Field/fields with this set will be used for best path tie breaking only
  4: bool isBestPathTieBreaker

  // List of int64's. Always > win's. -ve numbers will represent < wins
  5: list<i64> metric
}

// Expected to be sorted on priority
// @deprecated
struct MetricVector {
  // Only two metric vectors of same version will be compared.
  // If we want to come up with new scheme for metric vector at a later date.
  1: i64 version

  2: list<MetricEntity> metrics
}

/**
 * PrefixMetrics - Structs represents the core set of metrics used in best
 * prefix selection (aka best path selection). Overall goal of metric is to
 * capture the preference of advertised route. The winning PrefixEntry will
 * be used to compute the next-hops towards winning nodes and will be
 * re-distributed.
 *
 * `transitive` => Policy will retain the attribute value by default on route
 *                 re-distribution
 * `immutable` => The attribute is transitive and can't be modified by the
 *                Policy on route re-distribution. These attribute once set on
 *                origination can never be modified after-wards. `mutable`
 *                attributes can be modified by policy on re-distribution
 */
struct PrefixMetrics {
  // Version of prefix metrics. This should be updated everytime any changes
  // in this struct. It must be assigned automatically by the default value
  // here and code shouldn't try to set it to custom value. Decision module
  // can use the versioning information to appropriately respect backward
  // compatibility when new metric is introduced or old one is deprecated.
  1: i32 version = 1

  // 1st tie-breaker
  // Comparator: `prefer-higher`
  // Policy Compatibility: `transitive`, `mutable`
  // Network path preference for this route. This is set and updated as route
  // traverse the network.
  2: i32 path_preference = 0

  // 2nd tie-breaker
  // Comparator: `prefer-higher`
  // Policy Compatibility: `transitive`, `immutable`
  // User aka application preference of route. This is set at the origination
  // point and is never modified.
  3: i32 source_preference = 0

  // 3rd tie-breaker
  // Comparator: `prefer-lower`
  // Policy Compatibility: `transitive`, `mutable`
  // Intends to indicate the cost to reach the originating node from
  // re-originating node. Usually zero on originating node. However customized
  // policy can change the behavior.
  4: i32 distance = 0

  //
  // NOTE: Forwarding Algorithm set with `PrefixEntry` will be the subsequent
  // tie-breaker among the nodes advertising PrefixEntry with best metrics.
  // e.g. SP_ECMP (Shortest Path ECMP) - Will further tie-break nodes as per the
  // `igp_metric`. While `KSP2_ED_ECMP` will tie-break nodes as per nearest and
  // second nearest.
  // This information is inferred from topology and only intends to be local to
  // the node. Every node will have different behavior for forwarding algorithm
  // based on topology. And hence we're not include `igp_cost` metric in here
  //
}

struct PrefixEntry {
  1: Network.IpPrefix prefix

  // Indicates the type of prefix. This have no use except to indicate the
  // source of origination. e.g. Interface route, BGP route etc.
  2: Network.PrefixType type (deprecated)

  // Optional additional metadata. Encoding depends on PrefixType
  3: optional binary data (deprecated)

  // Default mode of forwarding for prefix is IP. If `forwardingType` is
  // set to SR_MPLS, then packet will be encapsulated via IP -> MPLS route will
  // be programmed at LERs and LSR (middle-hops) will perform label switching
  // while preserving the label until packet reaches destination
  4: OpenrConfig.PrefixForwardingType forwardingType =
    OpenrConfig.PrefixForwardingType.IP

  # Default forwarding (route computation) algorithm is shortest path ECMP.
  # Open/R implements 2-shortest path edge disjoint algorithm for forwarding.
  # Forwarding type must be set to SR_MPLS. MPLS tunneling will be used for
  # forwarding on shortest paths
  7: OpenrConfig.PrefixForwardingAlgorithm forwardingAlgorithm =
    OpenrConfig.PrefixForwardingAlgorithm.SP_ECMP

  // TODO has: This field is deprecated, it is only meaningful for the local
  // node advertising this prefix and no use for modules that receives this
  // information.
  // Indicates if the prefix entry is ephemeral or persistent.
  // If optional value is not present, then entry is persistent.
  // Ephemeral entries are not saved into persistent store(file) and will be
  // lost with restart, if not refreshed before cold start time.
  5: optional bool ephemeral (deprecated)

  // TODO has: This is deprecated. Instead use `metrics` field, it is compact
  // and concise.
  // Metric vector for externally injected routes into openr
  6: optional MetricVector mv (deprecated)

  // If the # of nethops for this prefix is below certain threshold, Decision
  // will not program/anounce the routes. If this parameter is not set, Decision
  // will not do extra check # of nexthops.
  8: optional i64 minNexthop

  // the IP or MPLS next-hops of this prefix must have this label prepended
  9: optional i32 prependLabel

  // Metrics associated with this Prefix. Route advertisement from multiple
  // nodes is first tied up on Metrics (best path selection) and then next-hops
  // are computed towards the nodes announcing the best routes.
  10: PrefixMetrics metrics

  // Set of tags associated with this route. This is meta-data and intends to be
  // used by Policy. NOTE: There is no ordering on tags
  11: set<string> tags

  // List of areas, this route has traversed through. This is automatically
  // extended (*not prepend) as route gets re-distributed across the areas.
  // AreaID at index=0 indicates the originating area and at AreaID at the
  // end indicates the re-distributing area.
  // NOTE: This is immutable by Policy and only code can modify it. It is always
  // set to empty on origination
  12: list<string> area_stack
}

// all prefixes that are bound to a given router
// announced under keys starting with "prefixes:"
struct PrefixDatabase {
  // must be the same as used in the key
  1: string thisNodeName
  // numbering is intentional
  3: list<PrefixEntry> prefixEntries
  // flag to indicate prefix(s) must be deleted
  5: bool deletePrefix

  // Optional attribute to measure convergence performance
  4: optional PerfEvents perfEvents;
  // Set to true if prefix was added by 'per prefix key' format.
  // this value is local to the node, and not used by remote nodes
  6: optional bool perPrefixKey
  // openr area in which prefix is advertised
  7: string area = KvStore.kDefaultArea
}
