/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
namespace hack OpenrTypes
namespace rust openr_types_thrift

include "openr/if/Network.thrift"
include "openr/if/OpenrConfig.thrift"
include "thrift/annotation/cpp.thrift"
include "thrift/annotation/hack.thrift"

/*
 * [Spark Neighbor FSM]
 *
 * Define:
 *  1) SparkNeighState
 *  2) SparkNeighEvent
 *
 * This is used to define transition state for Spark neighbors.
 * Ref: https://openr.readthedocs.io/Protocol_Guide/Spark.html#finite-state-machine
 */
enum SparkNeighState {
  IDLE = 0,
  WARM = 1,
  NEGOTIATE = 2,
  ESTABLISHED = 3,
  RESTART = 4,
}

enum SparkNeighEvent {
  HELLO_RCVD_INFO = 0,
  HELLO_RCVD_NO_INFO = 1,
  HELLO_RCVD_RESTART = 2,
  HEARTBEAT_RCVD = 3,
  HANDSHAKE_RCVD = 4,
  HEARTBEAT_TIMER_EXPIRE = 5,
  NEGOTIATE_TIMER_EXPIRE = 6,
  GR_TIMER_EXPIRE = 7,
  NEGOTIATION_FAILURE = 8,
}

enum LinkStatusEnum {
  DOWN = 0,
  UP = 1,
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
 * Link status object to track timestamp at when link changes its status.
 */
struct LinkStatus {
  1: LinkStatusEnum status;
  2: i64 unixTs = 0;
}

/**
 * Dictionary of link status objects with key which is interface name
 * and value which is information about link status. We don't need
 * to keep node name here because LinkStatusRecords is inside AdjacencyDatabase
 * which already has name of the node.
 */
struct LinkStatusRecords {
  @cpp.Type{template = "std::unordered_map"}
  1: map<string/* interfaceName */ , LinkStatus> linkStatusMap;
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
@cpp.MinimizePadding
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
}

/**
 * Link-State (LS) of a node for a particular area. It is collection of all
 * established adjacencies with few more attributes. Announced in KvStore
 * with key prefix - "adj:"
 */
@cpp.MinimizePadding
struct AdjacencyDatabase {
  /**
   * Name of the node
   */
  1: string thisNodeName;

  /**
   * [Hard-drain]
   * Overload or drain bit. Indicates if node should be use for transit(false)
   * or not(true).
   */
  2: bool isOverloaded = false;

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

  /**
   * [Soft-drain]
   * Non zero value indicates that this node is soft-drained
   * Higher the value indicates this node is less preferred to be chosen a destination
   * of a route.
   */
  7: i32 nodeMetricIncrementVal = 0;

  /**
   * Optional attribute to store status of all links in router
   * which are up and even down.
   */
  8: optional LinkStatusRecords linkStatusRecords;
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
@cpp.MinimizePadding
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
   * Comparator: `prefer-lower`
   * Policy Compatibility: `transitive`, `mutable`
   * 0 means there is no drained devices in the forwarding path of this route
   * 1 if there are drained device(s)
   */
  5: i32 drain_metric = 0;

  /**
   * 2nd tie-breaker
   * Comparator: `prefer-higher`
   * Policy Compatibility: `transitive`, `mutable`
   * Network path preference for this route. This is set and updated as route
   * traverse the network.
   */
  2: i32 path_preference = 0;

  /**
   * 3rd tie-breaker
   * Comparator: `prefer-higher`
   * Policy Compatibility: `transitive`, `immutable`
   * User aka application preference of route. This is set at the origination
   * point and is never modified.
   */
  3: i32 source_preference = 0;

  /**
   * 4th tie-breaker
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
}

/**
 * PrefixEntry, a structure that represents an advertised route in the KvStore
 */
@cpp.MinimizePadding
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
   * If the number of nexthops for this prefix is below certain threshold,
   * Decision will not program/announce the routes. If this parameter is not set,
   * Decision will not do extra check # of nexthops.
   */
  8: optional i64 minNexthop;

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

  13: optional i64 weight;
}

/**
 * Route advertisement object in KvStore. All prefixes that are bound to a given
 * router announced under keys starting with "prefixes:".
 */
@cpp.MinimizePadding
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
}

/**
 * @deprecated - Map of node name to adjacency database. This is deprecated
 * and should go away once area migration is complete.
 */
@cpp.Type{
  name = "std::unordered_map<std::string, openr::thrift::AdjacencyDatabase>",
}
typedef map<string, AdjacencyDatabase> AdjDbs

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
@cpp.MinimizePadding
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
}

/**
 * Details about an interface in Open/R
 */
@cpp.MinimizePadding
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
   * [TO_BE_DEPRECATED]
   * All adjacencies over this interface will inherit this override metric if
   * specified. Metric override is often used for soft draining of links.
   */
  3: optional i32 metricOverride;

  /**
   * Backoff in milliseconds for this interface. Interface that flaps or goes
   * crazy will get penalized with longer backoff. See link-backoff
   * functionality in LinkMonitor documentation.
   */
  4: optional i64 linkFlapBackOffMs;

  /**
   * Link level metric increment
   *
   * NOTE: This metric is directional. Override should ideally be also set on
   * the other end of the interface.
   */
  5: i32 linkMetricIncrementVal;
}

/**
 * Information of all links of this node
 */
@cpp.MinimizePadding
struct DumpLinksReply {
  /**
   * @deprecated - Name of the node. This is no longer of any relevance.
   */
  1: string thisNodeName;

  /**
   * Node level metric increment
   */
  2: i32 nodeMetricIncrementVal = 0;

  /**
   * Overload or drain status of the node.
   */
  3: bool isOverloaded;

  /**
   * Details of all the interfaces on system.
   */
  @cpp.Type{template = "std::unordered_map"}
  6: map<string, InterfaceDetails> interfaceDetails;
}

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
 * Struct to store internal override states for links/adjacencies:
 *  - Hard-drain:
 *    - Node Overload - `isOverloaded`;
 *    - Link Overload - `overloadedLinks`;
 *  - Soft-drain:
 *    - Node Metric Inc - `nodeMetricIncrementVal`;
 *    - Link Metric Inc - `linkMetricIncrementMap`;
 *  - Adjacency Metric Override:
 *    - Adjacency Metric Override - `adjMetricOverrides`;
 *    - NOTE: this metric override will overthrow all previously accumulated
 *      metric value for this specific adjacency;
 */
@cpp.MinimizePadding
struct LinkMonitorState {
  /**
   * [HARD-DRAIN]
   *
   * Overload bit for Open-R. If set then this node is not available for
   * transit traffic at all.
   */
  1: bool isOverloaded = 0;

  /**
   * [HARD-DRAIN]
   *
   * Overloaded links. If set then no transit traffic will pass through the
   * link and will be unreachable.
   */
  2: set<string> overloadedLinks;

  /**
   * [TO_BE_DEPRECATED]
   * Custom metric override for links. Can be leveraged to soft-drain interfaces
   * with higher metric value.
   */
  3: map<string, i32> linkMetricOverrides;

  /**
   * [ADJACENCY METRIC OVERRIDE]
   *
   * Custom metric override for an adjacency
   */
  @hack.SkipCodegen{reason = "Invalid key type"}
  5: map<AdjKey, i32> adjMetricOverrides;

  /**
   * [TO_BE_DEPRECATED]
   * Node label allocated to node in each area. `0` indicates null value.
   */
  6: map<string, i32> nodeLabelMap;

  /**
   * [SOFT-DRAIN]
   *
   * Custom static metric increment value for ALL links of the node.
   */
  7: i32 nodeMetricIncrementVal = 0;

  /**
   * [SOFT-DRAIN]
   *
   * Custom metric increment for a specifc set of links.
   */
  8: map<string, i32> linkMetricIncrementMap;
}

/**
 * Struct representing build information. Attributes are described in detail
 * in `openr/common/BuildInfo.h`
 */
@cpp.MinimizePadding
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
}

/**
 * Struct to represent originated prefix from PrefixManager's view
 */
@cpp.MinimizePadding
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
}

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
@cpp.MinimizePadding
struct SparkHelloMsg {
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
}

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
@cpp.MinimizePadding
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
   * Area identifier for establishing adjacency with neighbor.
   */
  10: string area;

  /**
   * Recipient neighbor node for this handshake message.
   * Other nodes will ignore. If not set, then this will
   * be treated as a multicast and all nodes will process it.
   */
  11: optional string neighborNodeName;
}

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
@cpp.MinimizePadding
struct SparkNeighbor {
  /**
   * Name of the node sending hello packets
   */
  1: string nodeName;

  /**
   * latest neighbor state
   */
  2: string state;

  /**
   * latest neighbor event triggering state transition
   */
  3: string event;

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
   * areaId to form adjacency
   */
  7: string area;

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
   * timestamp of last sent SparkHelloMsg for this neighbor
   */
  11: i64 lastHelloMsgSentTimeDelta = 0;

  /**
   * timestamp of last sent SparkHandshakeMsg for this neighbor
   */
  12: i64 lastHandshakeMsgSentTimeDelta = 0;

  /**
   * timestamp of last sent SparkHandshakeMsg for this neighbor
   */
  13: i64 lastHeartbeatMsgSentTimeDelta = 0;
}
