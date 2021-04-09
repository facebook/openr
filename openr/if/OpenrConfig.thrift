/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.OpenrConfig
namespace py openr.OpenrConfig
namespace py3 openr.thrift
namespace wiki Open_Routing.Thrift_APIs.OpenrConfig

include "BgpConfig.thrift"
include "configerator/structs/neteng/config/routing_policy.thrift"

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
  2: i32 sync_interval_s = 60;
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

  /**
   * Set this true to enable flooding-optimization, Open/R will start forming
   * spanning tree and flood updates on formed SPT instead of physical topology.
   * This will greatly reduce kvstore updates traffic, however, based on which
   * node is picked as flood-root, control-plane propagation might increase.
   * Before, propagation is determined by shortest path between two nodes. Now,
   * it will be the path between two nodes in the formed SPT, which is not
   * necessary to be the shortest path. (worst case: 2 x SPT-depth between two
   * leaf nodes). data-plane traffic stays the same.
   */
  8: optional bool enable_flood_optimization;
  /**
   * Set this true to let this node declare itself as a flood-root. You can set
   * multiple nodes as flood-roots in a network, in steady state, Open/R will
   * pick optimal (smallest node-name) one as the SPT for flooding. If optimal
   * root went away, Open/R will pick 2nd optimal one as SPT-root and so on so
   * forth. If all root nodes went away, Open/R will fall back to naive flooding.
   */
  9: optional bool is_flood_root;

  /**
  * Set buffering size for KvStore socket communication. Updates to neighbor node during
  * flooding can be buffered upto this number. For larger networks where burst of updates
  * can be high having high value makes sense. For smaller networks where burst of updates
  * are low, having low value makes more sense. Defaults to 65536.
  */
  101: i32 zmq_hwm = 65536;
} (cpp.minimize_padding)

struct DecisionConfig {
  /** Fast reaction time to update decision SPF upon receiving adj db update
  (in milliseconds). */
  1: i32 debounce_min_ms = 10;
  /** Decision debounce time to update SPF in frequent adj db update
    (in milliseconds). */
  2: i32 debounce_max_ms = 250;

  /** Knob to enable/disable BGP route programming. */
  101: bool enable_bgp_dryrun = false;
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

  /** Deprecated. Use area config. */
  4: list<string> include_interface_regexes = [] (deprecated);
  /** Deprecated. Use area config. */
  5: list<string> exclude_interface_regexes = [] (deprecated);
  /** Deprecated. Use area config. */
  6: list<string> redistribute_interface_regexes = [] (deprecated);

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
  4: i32 keepalive_time_s = 2;
  /**
   * Expiration time if node does NOT receive ‘keepAlive’ info from neighbor node.
   */
  5: i32 hold_time_s = 10;
  /**
   * Max time in seconds to hold neighbor's adjacency after receiving neighbor
   * restarting message
   */
  6: i32 graceful_restart_time_s = 30;

  7: StepDetectorConfig step_detector_conf;
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
  /** Bind address of thrift server. */
  1: string listen_addr = "::";
  /** Port of thrift server. */
  2: i32 openr_ctrl_port = 2018;
  /** Knob to enable/disable TLS thrift server. */
  3: bool enable_secure_thrift_server = false;
  /** Certificate authority path for verifying peers. */
  4: optional string x509_ca_path;
  /** Certificate path for the associated wangle::SSLContextConfig. */
  5: optional string x509_cert_path;
  /** The key path for the associated wangle::SSLContextConfig.
  If unspecified, will use x509_cert_path */
  6: optional string x509_key_path;
  /** eccCurveName for the associated wangle::SSLContextConfig */
  7: optional string ecc_curve_name;
  /** A comma separated list of strings. Strings are x509 common names to
  accept SSL connections from. If an empty string is provided, the server
  will accept connections from any authenticated peer. */
  8: optional string acceptable_peers;
  /** TLS ticket seed file path to use for client session resumption. */
  9: optional string ticket_seed_path;
  /** Verify type for client when enabling secure server. */
  10: optional VerifyClientType verify_client_type;
}

struct ThriftClientConfig {
  /** Knob to enable/disable TLS thrift client. */
  1: bool enable_secure_thrift_client = false;
  /** Verify type for server when enabling secure client. */
  2: VerifyServerType verify_server_type;
}

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

enum PrefixAllocationMode {
  /** Looks for seed_prefix in kvstore and elects a subprefix */
  DYNAMIC_LEAF_NODE = 0,
  /** Elects subprefix from configured seed_prefix */
  DYNAMIC_ROOT_NODE = 1,
  /** Looks for static allocation key in kvstore and use the prefix */
  STATIC = 2,
}

struct PrefixAllocationConfig {
  /**
   * Loopback address to which auto elected prefix will be assigned if enabled
   */
  1: string loopback_interface = "lo";
  /**
   * If set to true along with enable_prefix_allocation, second valid IP address
   * of the block will be assigned onto loopback_interface.
   */
  2: bool set_loopback_addr = false;
  /**
   * Whenever new address is elected for a node, before assigning it to
   * interface all previously allocated prefixes or other global prefixes will
   * be overridden with the new one. Use it with care!
   */
  3: bool override_loopback_addr = false;

  /**
   * If PrefixAllocationMode is DYNAMIC_ROOT_NODE, seed_prefix and
   * allocate_prefix_len needs to be filled.
   */
  4: PrefixAllocationMode prefix_allocation_mode;
  /**
   * In order to elect a prefix for the node a super prefix to elect from is
   * required. This is only applicable when enable_prefix_allocation is set to
   * true. e.g. "face:b00c::/64"
   */
  5: optional string seed_prefix;
  /**
   * Block size of allocated prefix in terms of it’s prefix length. If this is
   * set as 80, /80 prefix will be elected for a node. e.g. face:b00c:0:0:1234::/80
   */
  6: optional i32 allocate_prefix_len;
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

  /**
   * If the number of nexthops for this prefix is below a certain threshold,
   * Decision will not program/announce the routes. If this parameter is not
   * set, Decision will not do extra check the number of nexthops.
   */
  11: optional i64 minNexthop;

  /**
   * Next hop to specify.
   * It's used for Thrift-based VIP injection
   */
  12: optional string nexthop;
}

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
   * Area import policy, applied when a route enter this area
   */
  7: optional string import_policy_name;
}

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
   * Deprecated. Use enabled_bgp_to_openr instead.
   * Knob to enable BGP -> Open/R translation incrementally.
   */
  7: bool is_enabled = 0 (deprecated);

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
}

struct OpenrConfig {
  1: string node_name;
  /** Deprecated. Use area config. */
  2: string domain (deprecated);
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
  /**
   * Knob to enable/disable waiting for the FIB service to be ready
   * before initialize Open/R.
   */
  9: bool enable_fib_service_waiting = true;

  /**
   * Set time interval to wait for convergence before Decision starts to
   * compute routes. If not set, first neighbor update will trigger route
   * computation.
   */
  10: optional i32 eor_time_s;

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

  # Watchdog
  /** Enable watchdog thread to periodically check aliveness counters from each
  Open/R thread, if unhealthy thread is detected, force crash Open/R. */
  18: optional bool enable_watchdog;
  19: optional WatchdogConfig watchdog_config;

  /**
   * Enable prefix allocator to elect and assign a unique prefix for the node.
   * You will need to specify other configuration parameters in
   * prefix_allocation_config.
   */
  20: optional bool enable_prefix_allocation;
  21: optional PrefixAllocationConfig prefix_allocation_config;

  /** Deprecated. The feature is deprecated. */
  22: optional bool enable_ordered_fib_programming (deprecated);
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
   * KvStore thrift migration flags.
   * TODO: It is temporary & will go away once migration is done.
   */
  26: bool enable_kvstore_thrift = 1;
  27: bool enable_periodic_sync = 1;

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
   * Valid range (0, 256)
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
  * If set, will assume node is drained if no drain state
  * is found in the persistent store.
  */
  35: bool assume_drained = true;

  /**
  * Set the file path for undrained_flag. If the file undrained_flag
  * found, will set assume_drained to false.
  */
  36: optional string undrained_flag_path;

  /**
   * Enable best route selection based on PrefixMetrics.
   * TODO: It is temporary & will go away once new prefix metrics is rolled out
   */
  51: bool enable_best_route_selection = 0;

  /**
   * Maximum hold time for synchronizing the prefixes in KvStore after service
   * starts up. It is expected that all the sources inform PrefixManager about
   * the routes to be advertised within the hold time window. PrefixManager
   * can choose to synchronize routes as soon as it receives END marker from
   * all the expected sources.
   */
  52: i32 prefix_hold_time_s = 15;

  /**
   * Delay in seconds for MPLS route deletes. The delay would allow the remote
   * nodes to converge to new prepend-label associated with route advertisement.
   * This will avoid packet drops because of label route lookup.
   * NOTE: Label route add or update will happen immediately.
   */
  53: i32 mpls_route_delete_delay_s = 10;

  /**
   * Feature gate for new graceful restart behavior.
   * New workflow is to promote adj up after kvstore reaches initial sync state
   * This is a series of new changes in turn to address traffic loss during
   * agent cold boot.
   */
  54: bool enable_new_gr_behavior = 0;

  /**
   * Maximum hold time for synchronizing the adjacencies in KvStore after
   * service starts up. We expect all the adjacencies to be fully established
   * within hold time after Open/R starts LinkMonitor. LinkMonitor
   * can choose to synchronize adjacencies as soon as it receives all expected
   * established KvStore Peer Sync Events.
   */
  55: i32 adj_hold_time_s = 4;

  # vip thrift injection service
  90: optional bool enable_vip_service;

  # bgp
  100: optional bool enable_bgp_peering;
  102: optional BgpConfig.BgpConfig bgp_config;

  /**
   * Configuration to facilitate Open/R <-> BGP route conversions.
   * NOTE: This must be specified if bgp_peering is enabled
   */
  104: optional BgpRouteTranslationConfig bgp_translation_config;
}
