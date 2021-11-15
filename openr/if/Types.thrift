/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.Types
namespace py openr.Types
namespace py3 openr.thrift
namespace lua openr.Types
namespace wiki Open_Routing.Thrift_APIs.Types

include "Network.thrift"
include "OpenrConfig.thrift"

/*
 * Events in OpenR initialization process.
 * Ref: https://openr.readthedocs.io/Protocol_Guide/Initialization_Process.html
 */
enum InitializationEvent {
  INITIALIZING = 0,
  AGENT_CONFIGURED = 1,
  LINK_DISCOVERED = 2,
  NEIGHBOR_DISCOVERED = 3,
  KVSTORE_SYNCED = 4,
  RIB_COMPUTED = 5,
  FIB_SYNCED = 6,
  PREFIX_DB_SYNCED = 7,
  INITIALIZED = 8,
}

/**
 * Event object to track the key attribute and timestamp used for performance
 * measurement.
 */
struct PerfEvent {
  1: string nodeName;
  2: string eventDescr;
  3: i64 unixTs = 0;
}

/**
 * Ordered list of events for a particular data object. New event is added to
 * list as data object is originated, observed by node. This helps in tracking
 * things like time it takes for link down event to route convergence in HW.
 */
struct PerfEvents {
  /**
   * Ordered list of event. Most recent event is appended at the back
   */
  1: list<PerfEvent> events;
}

/**
 * InterfaceDb is the entire interface state for this system providing link
 * status and IPv4 / IPv6 LinkLocal addresses. Spark subscribes the interface
 * updates to initiate sessions with neighbors and the addresses are used as
 * nextHops.
 */
struct InterfaceInfo {
  /**
   * Interface status
   */
  1: bool isUp;

  /**
   * Interface index from system (linux)
   */
  2: i64 ifIndex;

  /**
   * All (IPv4 and IPv6) interface addresses including link-local addresses.
   */
  5: list<Network.IpPrefix> networks;
}

/**
 * Relation or session with the neighbor is termed as Adjacency. This struct
 * represents an Adjacency object.
 */
struct Adjacency {
  /**
   * Neighbor or peer node name
   */
  1: string otherNodeName;

  /**
   * Local interface over which adjacency is established
   */
  2: string ifName;

  /**
   * IPv6 link local address of the neighbor over `ifName` interface
   */
  3: Network.BinaryAddress nextHopV6;

  /**
   * IPv4 interface address of the neighbor over `ifName` interface
   */
  5: Network.BinaryAddress nextHopV4;

  /**
   * Metric, aka cost, to reach to the neighbor. Depending on config this could
   * be static (1) or RTT (dynamically measured) or override metric.
   */
  4: i32 metric;

  /**
   * SR, Adjacency Segment label associated with this adjacency. This is
   * node-local label and programmed by originator only assigned from
   * non-global space. 0 is invalid value
   */
  6: i32 adjLabel = 0;

  /**
   * Overloaded or drain bit for adjacency. Indicates that this adjacency is not
   * available for any kind of transit traffic.
   */
  7: bool isOverloaded = 0;

  /**
   * Round trip time (RTT) to neighbor in micro-seconds
   */
  8: i32 rtt;

  /**
   * Timestamp at creation time in seconds since epoch. Indicates uptime of the
   * adjacency.
   */
  9: i64 timestamp;

  /**
   * Weight of this adj in the network when weighted ECMP is enabled
   * TODO: This is not yet supported and doesn't influence routing. Work is in
   * progress.
   */
  10: i64 weight = 1;

  /**
   * Interface name of other end of link over which this adjacency is
   * established.
   */
  11: string otherIfName = "";

  /**
   * If set to true, node->neighbor adj could ONLY be used by neighbor but not
   * other nodes in the area. Otherwise, all nodes in the area could use the
   * adj for route computation.
   */
  12: bool adjOnlyUsedByOtherNode = false;
} (cpp.minimize_padding)

/**
 * Link-State (LS) of a node for a particular area. It is collection of all
 * established adjacencies with few more attributes. Announced in KvStore
 * with key prefix - "adj:"
 */
struct AdjacencyDatabase {
  /**
   * Name of the node
   */
  1: string thisNodeName;

  /**
   * Overload or drain bit. Indicates if node should be use for transit(false)
   * or not(true).
   */
  2: bool isOverloaded = 0;

  /**
   * All adjacent neighbors for this node
   */
  3: list<Adjacency> adjacencies;

  /**
   * SR Nodal Segment label associated with this node. This is globally unique
   * label assigned from global static space. 0 is invalid value
   */
  4: i32 nodeLabel;

  /**
   * Optional attribute to measure convergence performance
   */
  5: optional PerfEvents perfEvents;

  /**
   * Area to which this adjacency database belongs.
   */
  6: string area;
} (cpp.minimize_padding)

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

/**
 * How to compare two MetricEntity
 * @deprecated
 */
enum CompareType {
  /**
   * If present only in one metric vector, route with this type will win
   */
  WIN_IF_PRESENT = 1,

  /**
   * If present only in one metric vector, route without this type will win
   */
  WIN_IF_NOT_PRESENT = 2,

  /**
   * If present only in one metric vector, this type will be ignored from
   * comparision and fall through to next
   */
  IGNORE_IF_NOT_PRESENT = 3,
}

/**
 * @deprecated
 */
struct MetricEntity {
  /**
   * Type identifying each entity. (Used only for identification)
   */
  1: i64 type;

  /**
   * Priority fields. Initially priorities are assigned as
   * 10000, 9000, 8000, 7000 etc, this enables us to add any priorities
   * in between two fields.
   * Higher value is higher priority.
   */
  2: i64 priority;

  /**
   * Compare type defines how to handle cases of backward compatibility and
   * scenario's where some fields are not populated
   */
  3: CompareType op;

  /**
   * All fields without this set will be used for multipath selection
   * Field/fields with this set will be used for best path tie breaking only
   */
  4: bool isBestPathTieBreaker;

  /**
   * List of int64's. Always > win's. -ve numbers will represent < wins
   */
  5: list<i64> metric;
} (cpp.minimize_padding)

/**
 * Expected to be sorted on priority
 * @deprecated
 */
struct MetricVector {
  /**
   * Only two metric vectors of same version will be compared.
   * If we want to come up with new scheme for metric vector at a later date.
   */
  1: i64 version;

  2: list<MetricEntity> metrics;
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
  /**
   * Version of prefix metrics. This should be updated everytime any changes
   * in this struct. It must be assigned automatically by the default value
   * here and code shouldn't try to set it to custom value. Decision module
   * can use the versioning information to appropriately respect backward
   * compatibility when new metric is introduced or old one is deprecated.
   */
  1: i32 version = 1;

  /**
   * 1st tie-breaker
   * Comparator: `prefer-higher`
   * Policy Compatibility: `transitive`, `mutable`
   * Network path preference for this route. This is set and updated as route
   * traverse the network.
   */
  2: i32 path_preference = 0;

  /**
   * 2nd tie-breaker
   * Comparator: `prefer-higher`
   * Policy Compatibility: `transitive`, `immutable`
   * User aka application preference of route. This is set at the origination
   * point and is never modified.
   */
  3: i32 source_preference = 0;

  /**
   * 3rd tie-breaker
   * Comparator: `prefer-lower`
   * Policy Compatibility: `transitive`, `mutable`
   * Intends to indicate the cost to reach the originating node from
   * re-originating node. Usually zero on originating node. However customized
   * policy can change the behavior.
   */
  4: i32 distance = 0;
/**
   * NOTE: Forwarding Algorithm set with `PrefixEntry` will be the subsequent
   * tie-breaker among the nodes advertising PrefixEntry with best metrics.
   * e.g. SP_ECMP (Shortest Path ECMP) - Will further tie-break nodes as per the
   * `igp_metric`. While `KSP2_ED_ECMP` will tie-break nodes as per nearest and
   * second nearest.
   * This information is inferred from topology and only intends to be local to
   * the node. Every node will have different behavior for forwarding algorithm
   * based on topology. And hence we're not include `igp_cost` metric in here
   */
} (cpp.minimize_padding)

/**
 * PrefixEntry, a structure that represents an advertised route in the KvStore
 */
struct PrefixEntry {
  /**
   * IPv4 or IPv6 prefix indicating reachability to CIDR network
   */
  1: Network.IpPrefix prefix;

  /**
   * Indicates the type of prefix. This have no use except to indicate the
   * source of origination. e.g. Interface route, BGP route etc.
   */
  2: Network.PrefixType type (deprecated);

  /**
   * Optional additional metadata. Encoding depends on PrefixType
   */
  3: optional binary data (deprecated);

  /**
   * Default mode of forwarding for prefix is IP. If `forwardingType` is
   * set to SR_MPLS, then packet will be encapsulated via IP -> MPLS route will
   * be programmed at LERs and LSR (middle-hops) will perform label switching
   * while preserving the label until packet reaches destination
   */
  4: OpenrConfig.PrefixForwardingType forwardingType = OpenrConfig.PrefixForwardingType.IP;
  /**
   * Default forwarding (route computation) algorithm is shortest path ECMP.
   * Open/R implements 2-shortest path edge disjoint algorithm for forwarding.
   * Forwarding type must be set to SR_MPLS. MPLS tunneling will be used for
   * forwarding on shortest paths
   */
  7: OpenrConfig.PrefixForwardingAlgorithm forwardingAlgorithm = OpenrConfig.PrefixForwardingAlgorithm.SP_ECMP;

  /**
   * TODO has: This is deprecated. Instead use `metrics` field, it is compact
   * and concise.
   * Metric vector for externally injected routes into openr
   */
  6: optional MetricVector mv (deprecated);

  /**
   * If the number of nexthops for this prefix is below certain threshold,
   * Decision will not program/announce the routes. If this parameter is not set,
   * Decision will not do extra check # of nexthops.
   */
  8: optional i64 minNexthop;

  /**
   * IP or MPLS next-hops of this prefix must have this label prepended
   */
  9: optional i32 prependLabel;

  /**
   * Metrics associated with this Prefix. Route advertisement from multiple
   * nodes is first tied up on Metrics (best path selection) and then next-hops
   * are computed towards the nodes announcing the best routes.
   */
  10: PrefixMetrics metrics;

  /**
   * Set of tags associated with this route. This is meta-data and intends to be
   * used by Policy. NOTE: There is no ordering on tags
   */
  11: set<string> tags;

  /**
   * List of areas, this route has traversed through. This is automatically
   * extended (*not prepend) as route gets re-distributed across the areas.
   * AreaID at index=0 indicates the originating area and at AreaID at the
   * end indicates the re-distributing area.
   * NOTE: This is immutable by Policy and only code can modify it. It is always
   * set to empty on origination
   */
  12: list<string> area_stack;
} (cpp.minimize_padding)

/**
 * Route advertisement object in KvStore. All prefixes that are bound to a given
 * router announced under keys starting with "prefixes:".
 */
struct PrefixDatabase {
  /**
   * Name of the node announcing route
   */
  1: string thisNodeName;

  /**
   * Route advertisement entries
   */
  3: list<PrefixEntry> prefixEntries;

  /**
   * Optional attribute to measure convergence performance
   */
  4: optional PerfEvents perfEvents;

  /**
   * Flag to indicate prefix(s) must be deleted
   */
  5: bool deletePrefix;

  /**
   * Openr area in which prefix is advertised
   */
  7: string area;
} (cpp.minimize_padding)

/**
 * @deprecated - Allocated prefix information. This is stored in the persistent
 * store and can be read via config get thrift API.
 */
struct AllocPrefix {
  /**
   * Seed prefix from which sub-prefixes are allocated
   */
  1: Network.IpPrefix seedPrefix;

  /**
   * Allocated prefix length
   */
  2: i64 allocPrefixLen;

  /**
   * My allocated prefix, i.e., index within seed prefix
   */
  3: i64 allocPrefixIndex;
}

/**
 * @deprecated - Prefix allocation configuration. This is set in KvStore by
 * remote controller. The PrefixAllocator learns its own prefix, assign it on
 * the interface, and advertise it in the KvStore.
 *
 * See PrefixAllocator documentation for static configuration mode.
 */
struct StaticAllocation {
  /**
   * Map of node to allocated prefix. This map usually contains entries for all
   * the nodes in the network.
   */
  1: map<string/* node-name */ , Network.IpPrefix> nodePrefixes;
}

/**
 * @deprecated - Map of node name to adjacency database. This is deprecated
 * and should go away once area migration is complete.
 */
typedef map<string, AdjacencyDatabase> (
  cpp.type = "std::unordered_map<std::string, openr::thrift::AdjacencyDatabase>",
) AdjDbs

/**
 * @deprecated - Map of node name to adjacency database. This is deprecated
 * in favor of `received-routes` and `advertised-routes` and should go away
 * once area migration is complete.
 */
typedef map<string, PrefixDatabase> (
  cpp.type = "std::unordered_map<std::string, openr::thrift::PrefixDatabase>",
) PrefixDbs

/**
 * Represents complete route database that is or should be programmed in
 * underlying platform.
 */
struct RouteDatabase {
  /**
   * Name of the node where these routes are to be programmed
   * @deprecated - This is not useful field and should be removed
   */
  1: string thisNodeName;

  /**
   * An ordered list of events that can be used to derive the convergence time
   * @deprecated TODO - This should be removed in favor of perfEvents in
   * RouteDatabaseDelta.
   */
  3: optional PerfEvents perfEvents;

  /**
   * IPv4 and IPv6 routes with forwarding information
   */
  4: list<Network.UnicastRoute> unicastRoutes;

  /**
   * Label routes with forwarding information
   */
  5: list<Network.MplsRoute> mplsRoutes;
}

/**
 * Structure repesenting incremental changes to route database.
 */
struct RouteDatabaseDelta {
  /**
   * IPv4 or IPv6 routes to add or update
   */
  2: list<Network.UnicastRoute> unicastRoutesToUpdate;

  /**
   * IPv4 or IPv6 routes to delete
   */
  3: list<Network.IpPrefix> unicastRoutesToDelete;

  /**
   * Label routes to add or update
   */
  4: list<Network.MplsRoute> mplsRoutesToUpdate;

  /**
   * Label routes to delete
   */
  5: list<i32> mplsRoutesToDelete;

  /**
   * An ordered list of events that leads to these route updates. It can be used
   * to derive the convergence time
   */
  6: optional PerfEvents perfEvents;
}

/**
 * Perf log buffer maintained by Fib
 */
struct PerfDatabase {
  /**
   * Name of local node.
   * @deprecated TODO - This field is of no relevance
   */
  1: string thisNodeName;

  /**
   * Ordered list of historical performance events in ascending order of time
   */
  2: list<PerfEvents> eventInfo;
} (cpp.minimize_padding)

/**
 * Details about an interface in Open/R
 */
struct InterfaceDetails {
  /**
   * Interface information such as name and addresses
   */
  1: InterfaceInfo info;

  /**
   * Overload or drain status of the interface
   */
  2: bool isOverloaded;

  /**
   * All adjacencies over this interface will inherit this override metric if
   * specified. Metric override is often used for soft draining of links.
   * NOTE: This metric is directional. Override should ideally be also set on
   * the other end of the interface.
   */
  3: optional i32 metricOverride;

  /**
   * Backoff in milliseconds for this interface. Interface that flaps or goes
   * crazy will get penalized with longer backoff. See link-backoff
   * functionality in LinkMonitor documentation.
   */
  4: optional i64 linkFlapBackOffMs;
} (cpp.minimize_padding)

/**
 * Information of all links of this node
 */
struct DumpLinksReply {
  /**
   * @deprecated - Name of the node. This is no longer of any relevance.
   */
  1: string thisNodeName;

  /**
   * Overload or drain status of the node.
   */
  3: bool isOverloaded;

  /**
   * Details of all the interfaces on system.
   */
  6: map<string, InterfaceDetails> (
    cpp.template = "std::unordered_map",
  ) interfaceDetails;
} (cpp.minimize_padding)

/**
 * Set of attributes to uniquely identify an adjacency. It is identified by
 * (neighbor-node, local-interface) tuple.
 * TODO: Move this to Types.cpp
 */
struct AdjKey {
  /**
   * Name of the neighbor node
   */
  1: string nodeName;

  /**
   * Name of local interface over which an adjacency is established
   */
  2: string ifName;
}

/**
 * Struct to store internal override states for links (e.g. metric, overloaded
 * state) etc. This is not currently exposed via any API
 * TODO: Move this to Types.cpp
 */
struct LinkMonitorState {
  /**
   * Overload bit for Open-R. If set then this node is not available for
   * transit traffic at all.
   */
  1: bool isOverloaded = 0;

  /**
   * Overloaded links. If set then no transit traffic will pass through the
   * link and will be unreachable.
   */
  2: set<string> overloadedLinks;

  /**
   * Custom metric override for links. Can be leveraged to soft-drain interfaces
   * with higher metric value.
   */
  3: map<string, i32> linkMetricOverrides;

  /**
   * Label allocated to node (via RangeAllocator). `0` indicates null value
   */
  4: i32 nodeLabel = 0 (deprecated);

  /**
   * Custom metric override for adjacency
   */
  5: map<AdjKey, i32> adjMetricOverrides;

  /**
   * Node label allocated to node in each area. `0` indicates null value.
   */
  6: map<string, i32> nodeLabelMap;
} (cpp.minimize_padding)

/**
 * Struct representing build information. Attributes are described in detail
 * in `openr/common/BuildInfo.h`
 */
struct BuildInfo {
  1: string buildUser;
  2: string buildTime;
  3: i64 buildTimeUnix;
  4: string buildHost;
  5: string buildPath;
  6: string buildRevision;
  7: i64 buildRevisionCommitTimeUnix;
  8: string buildUpstreamRevision;
  9: i64 buildUpstreamRevisionCommitTimeUnix;
  10: string buildPackageName;
  11: string buildPackageVersion;
  12: string buildPackageRelease;
  13: string buildPlatform;
  14: string buildRule;
  15: string buildType;
  16: string buildTool;
  17: string buildMode;
} (cpp.minimize_padding)

/**
 * Struct to represent originated prefix from PrefixManager's view
 */
struct OriginatedPrefixEntry {
  /**
   * Originating prefix information from config
   */
  1: OpenrConfig.OriginatedPrefix prefix;

  /**
   * List of supporting sub-prefixes for this route
   */
  2: list<string> supporting_prefixes = [];

  /**
   * Is this route installed in local FIB or not. Route is installed with the
   * drop next-hops.
   */
  3: bool installed = 0;
} (cpp.minimize_padding)

/**
 * Describe timestamp information about send/recv of hello packets. We use this
 * to determine RTT of a node.
 */
struct ReflectedNeighborInfo {
  /**
   * Last sequence number we heard from neighbor
   */
  1: i64 seqNum = 0;

  /**
   * Timestamp of last hello packet sent by sender to neighbor from which hello
   * packet is received
   */
  2: i64 lastNbrMsgSentTsInUs = 0;

  /**
   * Timestamp when the last packet was received by neighbor from which hello
   * packet is received
   */
  3: i64 lastMyMsgRcvdTsInUs = 0;
}

/**
 * Type alias for OpenR version
 */
typedef i32 OpenrVersion

/**
 * Open/R versioning for backward compatibility
 */
struct OpenrVersions {
  1: OpenrVersion version;
  2: OpenrVersion lowestSupportedVersion;
}

/**
 * SparkHelloMsg;
 *    - Functionality:
 *      1) To advertise its own existence and basic neighbor information;
 *      2) To ask for immediate response for quick adjacency establishment;
 *      3) To notify for its own "RESTART" to neighbors;
 *    - SparkHelloMsg will be sent per interface;
 */
struct SparkHelloMsg {
  /**
   * [TO BE DEPRECATED]
   * Name of the domain this Open/R belongs to.
   * ATTN: adapt to area concept and usage to replace this.
   */
  1: string domainName;

  /**
   * Name of the node originating this hello message.
   */
  2: string nodeName;

  /**
   * Local interface name from where this SparkHelloMsg is sent.
   */
  3: string ifName;

  /**
   * Sequence number growing monotonically.
   * ATTN: this is used to differentiate restarting of neighbors
   * with unexpected sequence number.
   */
  4: i64 seqNum;

  /**
   * Mapping from:
   *  nodeName -> neighbor info
   *
   * ATTN: Neighbor Discovery Process is bi-directional and will
   * promote to ESTABLISHED state ONLY when I see the fact that
   * neighbor is aware of myself.
   */
  5: map<string, ReflectedNeighborInfo> neighborInfos;

  /**
   * Version contains current version and lowest version of messages
   * supported.
   *
   * See Versioning for more details.
   *
   * https://openr.readthedocs.io/Operator_Guide/Versions.html
   */
  6: OpenrVersion version;

  /**
   * Flag to ask for immediate response for Fast-Neighbor-Discovery
   *
   * https://openr.readthedocs.io/Protocol_Guide/Spark.html#fast-neighbor-discovery
   */
  7: bool solicitResponse = 0;

  /**
   * Flag to indicating the node is going Graceful Restart(GR)
   */
  8: bool restarting = 0;

  /**
   * Timestamp to indicate when this helloMsg is sent for RTT calculation
   */
  9: i64 sentTsInUs;
} (cpp.minimize_padding)

/**
 * SparkHeartbeatMsg
 *    - Functionality:
 *      To notify its own aliveness by advertising msg periodically;
 *    - SparkHeartbeatMsg will be sent per interface;
 */
struct SparkHeartbeatMsg {
  /**
   * Name of the node originating this heartbeat message
   */
  1: string nodeName;

  /**
   * Sequence number growing monotonically
   */
  2: i64 seqNum;

  /**
   * Flag to notify neighbor to unblock adjacency hold
   *
   * For details of Open/R Initialization Process, please refer to:
   *
   * https://openr.readthedocs.io/Protocol_Guide/Initialization_Process.html
   */
  3: bool holdAdjacency = false;
}

/**
 * SparkHandshakeMsg;
 *    - Functionality:
 *      To exchange param information to establish adjacency;
 *    - SparkHandshakeMsg will be sent per (interface, neighbor)
 */
struct SparkHandshakeMsg {
  /**
   * Name of the node originating this handshake message
   */
  1: string nodeName;

  /**
   * Used as signal to keep/stop sending handshake msg
   */
  2: bool isAdjEstablished;

  /**
   * Heartbeat expiration time
   */
  3: i64 holdTime;

  /**
   * Graceful-restart expiration time
   */
  4: i64 gracefulRestartTime;

  /**
   * Transport addresses of local interface. Open/R exchanges link-local
   * addresses only for V6.
   */
  5: Network.BinaryAddress transportAddressV6;
  6: Network.BinaryAddress transportAddressV4;

  /**
   * Neighbor's thrift server port
   */
  7: i32 openrCtrlThriftPort;

  /**
   * @deprecated - Neighbor's kvstore global CMD port, for ZMQ communication.
   */
  9: i32 kvStoreCmdPort;

  /**
   * Area identifier for establishing adjacency with neighbor.
   */
  10: string area;

  /**
   * Recipient neighbor node for this handshake message.
   * Other nodes will ignore. If not set, then this will
   * be treated as a multicast and all nodes will process it.
   */
  11: optional string neighborNodeName;

  12: optional bool enableFloodOptimization;
} (cpp.minimize_padding)

/**
 * SparkHelloPacket will define 3 types of messages inside thrift structure:
 *  - SparkHelloMsg;
 *  - SparkHeartbeatMsg;
 *  - SparkHandshakeMsg;
 */
struct SparkHelloPacket {
  /**
   * - Msg to announce node's presence on link with its
   *   own params;
   * - Send out periodically and on receipt of hello msg
   *   with solicitation flag set;
   */
  3: optional SparkHelloMsg helloMsg;

  /**
   * - Msg to announce nodes's aliveness.
   * - Send out periodically on intf where there is at
   *   least one neighbor in ESTABLISHED state;
   */
  4: optional SparkHeartbeatMsg heartbeatMsg;

  /**
   * - Msg to exchange params to establish adjacency
   *   with neighbors;
   * - Send out periodically and on receipt of handshake msg;
   */
  5: optional SparkHandshakeMsg handshakeMsg;
}

/**
 * Data structure to send with SparkNeighborEvent to convey
 * info for a single unique neighbor for upper module usage
 */
struct SparkNeighbor {
  /**
   * Name of the node sending hello packets
   */
  1: string nodeName;

  /**
   * neighbor state
   */
  2: string state;

  /**
   * areaId to form adjacency
   */
  3: string area;

  /**
   * Transport addresses of local interface. Open/R exchanges link-local
   * addresses only for V6.
   */
  4: Network.BinaryAddress transportAddressV6;
  5: Network.BinaryAddress transportAddressV4;

  /**
   * Neighbor's thrift server port
   */
  6: i32 openrCtrlThriftPort = 0;

  /**
   * @deprecated - Neighbor's kvstore global CMD port, for ZMQ communication.
   */
  7: i32 kvStoreCmdPort = 0;

  /**
   * Remote interface name
   */
  8: string remoteIfName;

  /**
   * Local interface name
   */
  9: string localIfName;

  /**
   * Round-trip-time of a packet over the physical link. It is deduced by
   * exchanging hello packets between neighbor nodes.
   */
  10: i64 rttUs;

  /**
   * Adjacency label for segment routing. It is derived from the ifIndex and
   * ensured to be unique for each adjacency (neighbor, interface). See
   * Source Routing documentation for more information.
   */
  11: i32 label;

  /**
   * Flag to support flood optimization or not
   */
  12: bool enableFloodOptimization = 0;

  /**
   * Flag to mark attribute in thrift::Adjacency. See definition for details.
   */
  13: bool adjOnlyUsedByOtherNode = 0;
} (cpp.minimize_padding)
