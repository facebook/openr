/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.OpenrConfig
namespace py openr.OpenrConfig
namespace py.asyncio openr.OpenrConfig
namespace py3 openr.thrift
namespace wiki Open_Routing.Thrift_APIs.OpenrConfig

include "openr/if/BgpConfig.thrift"
include "configerator/structs/neteng/config/routing_policy.thrift"
include "configerator/structs/neteng/config/vip_service_config.thrift"

enum PrefixForwardingType {
  /** IP nexthop is used for routing to the prefix. */
  IP = 0,
  /** MPLS label nexthop is used for routing to the prefix. */
  SR_MPLS = 1,
}

enum PrefixForwardingAlgorithm {
  /** Use Shortest path ECMP as prefix forwarding algorithm. */
  SP_ECMP = 0,
  /**
   * Use 2-Shortest-Path Edge Disjoint ECMP as prefix forwarding algorithm. It
   * computes edge disjoint second shortest ECMP paths for each destination
   * prefix. MPLS SR based tunneling is used for forwarding traffic over
   * non-shortest paths. This can be computationally expensive for networks
   * exchanging large number of routes. As per current implementation it will
   * incur one extra SPF run per destination prefix. SR_MPLS must be set if
   * KSP2_ED_ECMP is set.
   */
  KSP2_ED_ECMP = 1,
}

exception ConfigError {
  1: string message;
} (message = "message")

struct KvstoreFloodRate {
  1: i32 flood_msg_per_sec;
  2: i32 flood_msg_burst_size;
}

struct KvstoreConfig {
  /**
   * Set the TTL (in ms) of a key in the KvStore. For larger networks where
   * burst of updates can be high having high value makes sense. For smaller
   * networks where burst of updates are low, having low value makes more sense.
   */
  1: i32 key_ttl_ms = 300000;
  3: i32 ttl_decrement_ms = 1;

  4: optional KvstoreFloodRate flood_rate;

  /**
   * Sometimes a node maybe a leaf node and have only one path in to network.
   * This node does not require to keep track of the entire topology. In this
   * case, it may be useful to optimize memory by reducing the amount of
   * key/vals tracked by the node. Setting this flag enables key prefix filters
   * defined by key_prefix_filters. A node only tracks keys in kvstore that
   * matches one of the prefixes in key_prefix_filters.
   */
  5: optional bool set_leaf_node;
  /**
   * This comma separated string is used to set the key prefixes when key prefix
   * filter is enabled (See set_leaf_node). It is also set when requesting KEY_DUMP
   * from peer to request keys that match one of these prefixes.
   */
  6: optional list<string> key_prefix_filters;
  7: optional list<string> key_originator_id_filters;
} (cpp.minimize_padding)

/*
 * Enum to customize the best route selection algorithm amongst all areas. SHORTEST_DISTANCE
 * is the default algorithm. K_SHORTEST_DISTANCE_2 and PER_AREA_SHORTEST_DISTANCE
 * will select both shortest and non shortest distance routes to help form non shortest
 * path LSPs across areas
 */
enum RouteSelectionAlgorithm {
  /*
   * In order of priority, selects the best routes with the best:
   *  1) Path preference
   *  2) Source preference
   *  3) Distance
   *
   *  Default algorithm.
   */
  SHORTEST_DISTANCE = 0,

  /*
   * In order of priority, selects the best routes with the best:
   *  1) Path preference
   *  2) Source preference
   *  3) Best and second best distance
   */
  K_SHORTEST_DISTANCE_2 = 1,

  /*
   * In order of priority, selects the best routes with the best:
   *  1) Path preference
   *  2) Source preference
   *  3) Best distance for each area
   */
  PER_AREA_SHORTEST_DISTANCE = 2,
}

/* Path computation rules for a specific area */
struct AreaPathComputationRules {
  1: PrefixForwardingAlgorithm forwardingAlgo = PrefixForwardingAlgorithm.SP_ECMP;
  2: PrefixForwardingType forwardingType = PrefixForwardingType.IP;
} (cpp.minimize_padding)

struct RouteComputationRules {
  /* Route selection algorithm the route will use */
  1: RouteSelectionAlgorithm routeSelectionAlgo = RouteSelectionAlgorithm.SHORTEST_DISTANCE;

  /* Map of path computation rules per area. Key is the areaId string */
  2: map<string, AreaPathComputationRules> areaPathComputationRules;
} (cpp.minimize_padding)

struct DecisionConfig {
  /** Fast reaction time to update decision SPF upon receiving adj db update
  (in milliseconds). */
  1: i32 debounce_min_ms = 10;
  /** Decision debounce time to update SPF in frequent adj db update
    (in milliseconds). */
  2: i32 debounce_max_ms = 250;
  /** Fast reaction time to save rib policy upon receiving setRibPolicy request
  (in milliseconds). */
  3: i32 save_rib_policy_min_ms = 10000;
  /** Decision time to save rib policy  in frequent setRibPolicy requests
  (in milliseconds). */
  4: i32 save_rib_policy_max_ms = 60000;
}

struct LinkMonitorConfig {
  /**
   * When link goes down after being stable/up for long time, then the backoff
   * is applied. If link goes up and down in backoff state then it’s backoff
   * gets doubled (Exponential backoff). Backoff clear timer will start from
   * latest down event. When backoff is cleared then actual status of link is
   * used. If link is up, neighbor discovery is performed. Otherwise, Open/R
   * will wait for the link to come up.
   */
  1: i32 linkflap_initial_backoff_ms = 60000;

  /**
   * This serves two purposes. This is the maximum backoff a link gets penalized
   * with by consecutive flaps. When the first backoff time has passed and the
   * link is in UP state and the next time the link goes down within
   * linkflap_max_backoff_ms, the new backoff becomes double of the previous
   * value. If the link remains stable for linkflap_max_backoff_ms, all of its
   * history is erased and link gets linkflap_initial_backoff_ms next time when
   * it goes down.
   */
  2: i32 linkflap_max_backoff_ms = 300000;

  /**
   * Compute and use RTT of a link as a metric value. If set to false, cost of
   * a link is 1 and cost of a path is hop count.
   */
  3: bool use_rtt_metric = true;

  /**
  * Enable convergence performance measurement for adjacency updates.
  */
  7: bool enable_perf_measurement = true;
}

struct StepDetectorConfig {
  1: i64 fast_window_size = 10;
  2: i64 slow_window_size = 60;
  3: i32 lower_threshold = 2;
  4: i32 upper_threshold = 5;
  5: i64 ads_threshold = 500;
}

struct SparkConfig {
  1: i32 neighbor_discovery_port = 6666;
  /** How often to send SparkHelloMsg to neighbors. */
  2: i32 hello_time_s = 20;
  /**
   * When interface is detected UP, Open/R can perform fast initial neighbor
   * discovery. Default value is 500 which means neighbor will be discovered
   * within 1s on a link.
   */
  3: i32 fastinit_hello_time_ms = 500;

  /**
   * SparkHeartbeatMsg are used to detect if neighbor is up running when
   * adjacency is established.
   */
  4: i32 keepalive_time_s = 2 (deprecated);

  /**
   * Expiration time if node does NOT receive ‘keepAlive’ info from neighbor node.
   */
  5: i32 hold_time_s = 30;
  /**
   * Max time in seconds to hold neighbor's adjacency after receiving neighbor
   * restarting message
   */
  6: i32 graceful_restart_time_s = 30;

  7: StepDetectorConfig step_detector_conf;

  /**
   * The minimum time that must be elapsed, during Open/R Initialization process,
   * before NEIGHBOR_DISCOVERED signal can be published to LinkMonitor.
   */
  8: i32 min_neighbor_discovery_interval_s = 2;

  /**
   * The max interval, at the expiry of which NEIGHBOR_DISCOVERED signal must be
   * published to LinkMonitor during Open/R Initialization process.
   */
  9: i32 max_neighbor_discovery_interval_s = 15;
}

struct WatchdogConfig {
  /** Watchdog thread healthcheck interval. */
  1: i32 interval_s = 20;
  /** Watchdog thread aliveness threshold. */
  2: i32 thread_timeout_s = 300;
  /**
   * Enforce upper limit on amount of memory in mega-bytes that Open/R process
   * can use. Above this limit watchdog thread will trigger crash. Service can
   * be auto-restarted via system or some kind of service manager. This is very
   * useful to guarantee protocol doesn’t cause trouble to other services on
   * device where it runs and takes care of slow memory leak kind of issues. */
  3: i32 max_memory_mb = 800;
}

struct MonitorConfig {
  /** Max number for storing recent event logs. */
  1: i32 max_event_log = 100;
  /** If set, will enable Monitor::processEventLog() to submit the event logs. */
  2: bool enable_event_log_submission = true;
}

struct MemoryProfilingConfig {
  /** Knob to enable or disable memory profiling.
      If enabled, it will dump the heap profile every heap_dump_interval_s second. */
  1: bool enable_memory_profiling = false;
  2: i32 heap_dump_interval_s = 300;
}

enum VerifyClientType {
  // Request a cert and verify it. Fail if verification fails or no
  // cert is presented
  ALWAYS = 0,
  // Request a cert from the peer and verify if one is presented.
  // Will fail if verification fails.
  // Do not fail if no cert is presented.
  IF_PRESENTED = 1,
  // No verification is done and no cert is requested.
  DO_NOT_REQUEST = 2,
}

enum VerifyServerType {
  // Server cert will be presented unless anon cipher,
  // Verficiation will happen and a failure will result in termination
  IF_PRESENTED = 0,
  // Server cert will be presented unless anon cipher,
  // Verification will happen but the result will be ignored
  IGNORE_VERIFY_RESULT = 1,
}

struct ThriftServerConfig {
  /**
   * Bind address of thrift server.
   */
  1: string listen_addr = "::";

  /**
   * Port of thrift server.
   */
  2: i32 openr_ctrl_port = 2018;

  /**
   * Knob to enable/disable TLS thrift server.
   */
  3: bool enable_secure_thrift_server = false;

  /**
   * Certificate authority path for verifying peers.
   */
  4: optional string x509_ca_path;

  /**
   * Certificate path for the associated wangle::SSLContextConfig.
   */
  5: optional string x509_cert_path;

  /**
   * The key path for the associated wangle::SSLContextConfig.
   * If unspecified, will use x509_cert_path
   */
  6: optional string x509_key_path;

  /**
   * eccCurveName for the associated wangle::SSLContextConfig
   */
  7: optional string ecc_curve_name;

  /**
   * A comma separated list of strings. Strings are x509 common names to
   * accept SSL connections from. If an empty string is provided, the server
   * will accept connections from any authenticated peer.
   */
  8: optional string acceptable_peers;

  /**
   * TLS ticket seed file path to use for client session resumption.
   */
  9: optional string ticket_seed_path;

  /**
   * Verify type for client when enabling secure server.
   */
  10: optional VerifyClientType verify_client_type;

  /**
   * Knob to enable/disable non-default VRF thrift server.
   */
  11: optional bool enable_non_default_vrf_thrift_server;

  /**
   * Timeout (4 seconds by default) of Cpp2Worker join in Thrift server.
   */
  12: i32 workers_join_timeout = 4;

  /**
   * User specified non-default vrf list to start thrift server from.
   *
   * Attention: this flag will be in use only when
   *  `enable_non_default_vrf_thrift_server = true`
   */
  13: list<string> vrf_names = [];

  /**
   * User specified default vrf name to pivot the thrift server to.
   * This VRF is only used when pivoting back from a non-default VRF
   */
  14: optional string default_vrf_name;
}

struct ThriftClientConfig {
  /** Knob to enable/disable TLS thrift client. */
  1: bool enable_secure_thrift_client = false;
  /** Verify type for server when enabling secure client. */
  2: VerifyServerType verify_server_type;
}

struct OriginatedPrefix {
  1: string prefix;

  2: PrefixForwardingType forwardingType = PrefixForwardingType.IP;

  3: PrefixForwardingAlgorithm forwardingAlgorithm = PrefixForwardingAlgorithm.SP_ECMP;

  /**
   * Minimum number of supporting routes to advertise this prefix. Prefix will
   * be advertised/withdrawn when the number of supporting routes change.
   */
  4: i16 minimum_supporting_routes = 0;

  /** If set to true, program the prefix to FIB. */
  5: optional bool install_to_fib;

  6: optional i32 source_preference;

  7: optional i32 path_preference;

  /**
   * Set of tags associated with this route. This is meta-data and intends to be
   * used by Policy. NOTE: There is no ordering on tags.
   */
  8: optional set<string> tags;

  /** To interact with BGP, area prepending is needed. */
  9: optional list<string> area_stack;

  /** Describe the route of this prefix. **/
  10: optional string origination_policy;

  /**
   * If the number of nexthops for this prefix is below a certain threshold,
   * Decision will not program/announce the routes. If this parameter is not
   * set, Decision will not do extra check the number of nexthops.
   */
  11: optional i64 minNexthop;
} (cpp.minimize_padding)

struct LabelRange {
  1: i32 start_label;
  2: i32 end_label;
}

enum SegmentRoutingNodeLabelType {
  /**
   * Current way of allocation. Needs range parameter.
   */
  AUTO = 0,
  /**
   * Node Segment IDs are allocated statically.
   * The uniqueness of node label will be determined
   * by the application which generates static
   * node label.
   */
  STATIC = 1,
}

struct SegmentRoutingNodeLabel {
  /**
   * The way node segment label should be allocated.
   */
  1: SegmentRoutingNodeLabelType sr_node_label_type;

  /**
   * Statically allocated node label for this node.
   * Applicable if SegmentRoutingNodeLabelType is
   * SegmentRoutingNodeLabelType::STATIC
   */
  2: optional i32 node_segment_label;

  /**
   * Label range for node segment label to allocate from if
   * sr_node_label_type is AUTO.
   */
  3: optional LabelRange node_segment_label_range;
} (cpp.minimize_padding)

/**
 * The area config specifies the area name, interfaces to perform discovery
 * on, neighbor names to peer with, and interface addresses to redistribute
 *
 * 1) Config specifying patricular interface patterns and all neighbors. Will
 *    perform discovery on interfaces matching any include pattern and no
 *    exclude pattern and peer with any node in this area:
 * ```
 *  config = {
 *    area_id : "MyNetworkArea",
 *    neighbor_regexes : [".*"],
 *    include_interface_regexes : ["ethernet1", "port-channel.*"],
 *    exclude_interface_regexes : ["loopback1"],
 *    redistribute_interface_regexes: ["loopback10"],
 *  }
 * ```
 *
 * 2) Config specifying particular neighbor patterns and all interfaces. Will
 *    perform discovery on all available interfaces and peer with nodes in
 *    this area matching any neighbor pattern:
 * ```
 *  config = {
 *    area_id : "MyNetworkArea",
 *    neighbor_regexes : ["node123", "fsw.*"],
 *    include_interface_regexes : [".*"],
 *    exclude_interface_regexes : [],
 *    redistribute_interface_regexes: ["loopback10"],
 *  }
 * ```
 *
 * 3) Config specifying both. Will perform discovery on interfaces matching
 *    any include pattern and no exclude pattern and peer with nodes in this
 *    area matching any neighbor pattern:
 * ```
 *  config = {
 *    area_id : "MyNetworkArea",
 *    neighbor_regexes : ["node1.*"],
 *    include_interface_regexes : ["loopback0"],
 *    exclude_interface_regexes : ["loopback1"],
 *    redistribute_interface_regexes: ["loopback10"],
 *  }
 * ```
 *
 * 4) Config specifying neither. Will perform discovery on no interfaces and
 *    peer with no nodes:
 * ```
 *  config = {
 *    area_id : "MyNetworkArea",
 *    neighbor_regexes : [],
 *    include_interface_regexes : [],
 *    exclude_interface_regexes : [],
 *    redistribute_interface_regexes: ["loopback10"],
 *  }
 * ```
 *
 */

struct AreaConfig {
  1: string area_id;
  3: list<string> neighbor_regexes;
  4: list<string> include_interface_regexes;
  5: list<string> exclude_interface_regexes;

  /**
   * Comma-separated list of interface regexes whose addresses will be
   * advertised to this area
   */
  6: list<string> redistribute_interface_regexes;

  /**
   * Area import policy, applied when a route enters this area
   */
  7: optional string import_policy_name;

  /**
   * The way node segment label should be allocated
   * in this area for segment routing. Node segment
   * should be unique per device per area.
   */
  8: optional SegmentRoutingNodeLabel area_sr_node_label;
} (cpp.minimize_padding)

/**
 * Configuration to facilitate route translation between BGP <-> Open/R
 * ```
 * - BGP Communities <=> tags
 * - BGP AS Path <=> area_stack
 * - BGP Origin <=> metrics.path_preference (IGP=1000, EGP=900, INCOMPLETE=500)
 * - BGP Special Source AS => metrics.source_preference
 * - BGP AS Path Length => metrics.distance
 * - metrics.source_preference => BGP Local Preference
 * ```
 */
struct BgpRouteTranslationConfig {
  /**
   * Map that defines communities in '{asn}:{value}' format to their human
   * readable names. Open/R will use the community name as tag
   * If not available then string representation of community will be used as
   * tag
   */
  1: map<string, string> communities_to_name;

  /**
   * Map that defines ASN to Area name mapping. Mostly for readability. Open/R
   * will use this mapping to convert as-path to area_stack and vice versa.
   * If not mapping is found then string representation of ASN will be used.
   */
  2: map<i64, string> asn_to_area;

  /**
   * Source preference settings
   * The ASN if specified will add `5 * (3 - #count_asn)` to source preference
   * e.g. if `source_preference_asn = 65000` and AS_PATH contains the 65000 asn
   * twice, then `source_preference = 100 + 5 ( 3 - 2) = 105`
   */
  4: i64 default_source_preference = 100;
  5: optional i64 source_preference_asn;

  /**
   * ASNs to ignore for distance computation
   */
  6: set<i64> asns_to_ignore_for_distance;

  /**
   * Knob to enable BGP -> Open/R translation incrementally
   */
  8: bool enable_bgp_to_openr = 0;

  /**
   * Knob to enable Open/R -> BGP translation incrementally
   */
  9: bool enable_openr_to_bgp = 0;

  /**
   * Knob to disable legacy BGP route translation. e.g. `data`, `mv`
   * will no longer be populated. For this option to be enabled, both
   * of the above route translation options must be enabled.
   */
  10: bool disable_legacy_translation = 0;

  /**
   * Translate path preference to local_preference and vice versa.
   * This is the desired behavior with IBN policy.
   * IBN generates same preference value, assign it to local pref in
   * bgp context; to path preference in openr context.
   *
   * Current implementation is translating path preference to origin.
   * This is a migration knob for transition from legacy behavior to
   * new behavior.
   */
  11: bool enable_path_pref_to_local_pref_translation = 0;

  /**
   * Translate igp cost to community. Note since igp cost is
   * non transitive and there is no policy between openr<->bgp
   * Note this is only one way translation: openr -> bgp
   * When translating bgp->openr, these community will be stripped
   */
  12: map<i32, string> igp_cost_to_community;
} (cpp.minimize_padding)

struct OpenrConfig {
  1: string node_name;
  3: list<AreaConfig> areas = [];

  /** Thrift Server - Bind dddress */
  4: string listen_addr = "::";
  /** Thrift Server - Port */
  5: i32 openr_ctrl_port = 2018;

  /** If set to true, Open/R will not try to program routes. */
  6: optional bool dryrun;
  /**
   * Open/R supports v4 as well but it needs to be turned on explicitly. It is
   * expected that each interface will have v4 address configured for link local
   * transport and v4/v6 topologies are congruent.
   */
  7: optional bool enable_v4;
  /**
   * Knob to enable/disable default implementation of FibService that comes
   * along with Open/R for Linux platform. If you want to run your own FIB
   * service then disable this option.
   */
  8: optional bool enable_netlink_fib_handler;

  11: PrefixForwardingType prefix_forwarding_type = PrefixForwardingType.IP;
  12: PrefixForwardingAlgorithm prefix_forwarding_algorithm = PrefixForwardingAlgorithm.SP_ECMP;

  /**
   * Enables segment routing feature. Currently, it only elects node/adjacency labels.
   */
  13: optional bool enable_segment_routing;
  14: optional i32 prefix_min_nexthop;

  # Config for different modules
  15: KvstoreConfig kvstore_config;
  16: LinkMonitorConfig link_monitor_config;
  17: SparkConfig spark_config;

  /**
   * Enable watchdog thread to periodically check aliveness counters from each
   * Open/R thread, if unhealthy thread is detected, force crash Open/R.
   */
  18: optional bool enable_watchdog;
  19: optional WatchdogConfig watchdog_config;

  /** TCP port on which FibService will be listening. */
  23: i32 fib_port;

  /**
   * Enables `RibPolicy` for computed routes. This knob allows thrift APIs to
   * set/get `RibPolicy` in Decision module. For more information refer to
   * `struct RibPolicy` in OpenrCtrl.thrift.
   */
  24: bool enable_rib_policy = 0;

  /** Config for monitor module. */
  25: MonitorConfig monitor_config;

  /**
   * RFC5549 -- IPv4 reachability over IPv6 nexthop
   */
  28: optional bool v4_over_v6_nexthop;

  /**
   * Config for `decision` module
   */
  29: DecisionConfig decision_config;

  /**
   * Mark control plane traffic with specified IP-TOS value.
   * Valid range (0, 256) for making.
   * Set this to 0 if you don't want to mark packets.
   */
  30: optional i32 ip_tos;

  /**
   * V4/V6 prefixes to be originated.
   */
  31: optional list<OriginatedPrefix> originated_prefixes;

  /**
   * area policy definitions
   */
  32: optional routing_policy.PolicyConfig area_policies;

  /**
   * Config for thrift server.
   */
  33: ThriftServerConfig thrift_server;

  /**
   * Config for thrift client. If no config provided,
   * go with non-secure client connenction.
   */
  34: optional ThriftClientConfig thrift_client;

  /**
  * [Hard-Drain]
  *
  * If set, will assume node is drained if no drain state
  * is found in the persistent store.
  */
  35: bool assume_drained = true;

  /**
  * [Hard-Drain]
  *
  * Set the file path for undrained_flag. If the file undrained_flag
  * found, will set assume_drained to false.
  */
  36: optional string undrained_flag_path;

  /**
  * File path where to persist internal state across restart.
  */
  37: string persistent_config_store_path = "/tmp/openr_persistent_config_store.bin";

  /**
  * Config for periodically dumping the heap memory profile of Open/R process.
  */
  38: optional MemoryProfilingConfig memory_profiling_config;

  /**
  * [Soft-Drain]
  *
  * Set the file path for softdrained_flag. If the file flag is set. Open/R
  * will be set SOFT_DRAINED.
  */
  39: bool enable_soft_drain = true;

  /**
  * [Soft-Drain]
  *
  * Set the default value for soft-drained node increment. Can be adjusted
  * accordingly in case we choose different types of adj metric and different
  * size of the network.
  */
  40: i64 softdrained_node_increment = 100;

  /**
   * This knob is meant for migrating BGP config routes to Open/R-originated.
   * Currently there's no way to choose between two otherwise equivalent configs
   * (with equal metrics) in PrefixMgr (selectBestPrefixMetrics). The following
   * knob forces this selection in favor of Open/R originated routes
   */
  50: bool prefer_openr_originated_routes = 0;

  /**
   * Enable best route selection based on PrefixMetrics.
   * TODO: It is temporary & will go away once new prefix metrics is rolled out
   */
  51: bool enable_best_route_selection = 0;

  /**
   * Delay in seconds for MPLS route deletes. The delay would allow the remote
   * nodes to converge to new prepend-label associated with route advertisement.
   * This will avoid packet drops because of label route lookup.
   * NOTE: Label route add or update will happen immediately.
   */
  53: i32 mpls_route_delete_delay_s = 10;

  /**
   * [TO BE DEPRECATED]
   * Maximum hold time for synchronizing the adjacencies in KvStore after
   * service starts up. We expect all the adjacencies to be fully established
   * within hold time after Open/R starts LinkMonitor. LinkMonitor
   * can choose to synchronize adjacencies as soon as it receives all expected
   * established KvStore Peer Sync Events.
   */
  55: i32 adj_hold_time_s = 4;

  /**
   * Delay in milliseconds for route deletion. Route withdrawal would not get
   * impacted. This delay would provide remote nodes some time to process
   * route withdrawal and converge (e.g. next-hop group shrink), while still
   * keeping the forwarding state intact for specified duration. Value of 0ms
   * will disable route deletion.
   */
  60: i32 route_delete_delay_ms = 1000;

  # vip thrift injection service
  90: optional bool enable_vip_service;
  91: optional vip_service_config.VipServiceConfig vip_service_config;

  # bgp
  100: optional bool enable_bgp_peering;
  102: optional BgpConfig.BgpConfig bgp_config;

  /**
   * Configuration to facilitate Open/R <-> BGP route conversions.
   * NOTE: This must be specified if bgp_peering is enabled
   */
  104: optional BgpRouteTranslationConfig bgp_translation_config;

  /**
   * Enable FSDB subscription to detect a neighbor down event, in case
   * such event is undetectable through interface change
   * Current use case: detecting LAG down
   */
  105: bool enable_neighbor_monitor = false;

  /**
   * This flag indicates if we want to clear fib routes programmed by previous
   * incarnation. This is recommended to use in rollback scenario to clear all
   * routes previously programmed.
   */
  106: bool enable_clear_fib_state = false;
/**
 * ATTN: All of the temp config knobs serving for gradual rollout purpose use
 * id range of 200 - 300
 */
} (cpp.minimize_padding)
