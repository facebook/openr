/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.OpenrConfig
namespace py3 openr.thrift

include "BgpConfig.thrift"

struct KvstoreFloodRate {
  1: i32 flood_msg_per_sec
  2: i32 flood_msg_burst_size
}

struct KvstoreConfig {
  # kvstore
  1: i32 key_ttl_ms = 300000 # 5min 300*1000
  2: i32 sync_interval_s = 60
  3: i32 ttl_decrement_ms = 1
  4: optional KvstoreFloodRate flood_rate

  5: optional bool set_leaf_node
  6: optional list<string> key_prefix_filters
  7: optional list<string> key_originator_id_filters

  # flood optimization
  8: optional bool enable_flood_optimization
  9: optional bool is_flood_root
}

struct LinkMonitorConfig {
  1: i32 linkflap_initial_backoff_ms = 60000 # 60s
  2: i32 linkflap_max_backoff_ms = 300000 # 5min
  3: bool use_rtt_metric = true
  4: list<string> include_interface_regexes = []
  5: list<string> exclude_interface_regexes = []
  6: list<string> redistribute_interface_regexes = []
}

struct SparkConfig {
  1: i32 neighbor_discovery_port = 6666

  2: i32 hello_time_s = 20
  3: i32 fastinit_hello_time_ms = 500

  4: i32 keepalive_time_s = 2
  5: i32 hold_time_s = 10
  6: i32 graceful_restart_time_s = 30
}

struct WatchdogConfig {
  1: i32 interval_s = 20
  2: i32 thread_timeout_s = 300
  3: i32 max_memory_mb = 800
}

enum PrefixForwardingType {
  IP = 0
  SR_MPLS = 1
}

enum PrefixForwardingAlgorithm {
  SP_ECMP = 0
  KSP2_ED_ECMP = 1
}

/*
 * DYNAMIC_LEAF_NODE
 *   => looks for seed_prefix in kvstore and elects a subprefix
 * DYNAMIC_ROOT_NODE
 *   => elects subprefix from configured seed_prefix
 * STATIC
 *   => looks for static allocation key in kvstore and use the prefix
 */
enum PrefixAllocationMode {
  DYNAMIC_LEAF_NODE = 0
  DYNAMIC_ROOT_NODE = 1
  STATIC = 2
}

struct PrefixAllocationConfig {
  1: string loopback_interface = "lo"
  2: bool set_loopback_addr = false
  3: bool override_loopback_addr = false

  // If prefixAllocationMode == DYNAMIC_ROOT_NODE
  // seed_prefix and allocate_prefix_len needs to be filled.
  4: PrefixAllocationMode prefix_allocation_mode
  5: optional string seed_prefix
  6: optional i32 allocate_prefix_len
}

/**
 * NOTE: interfaces and nodes can be explicit or unix regex
 * 1) Config specifying interfaces:
 *  config = {
 *    area_id : "1",
 *    interface_regexes : ["ethernet1, port-channel.*"],
 *    neighbor_regexes : []
 *  }
 *
 * 2) Config specifying nodes:
 *  config = {
 *    area_id : "2",
 *    interface_regexes : [],
 *    neighbor_regexes : ["node123", "fsw.*"]
 *  }
 *
 * 3) Config specifying both. Tie breaker will be implementated
 *    from config reader side:
 *  config = {
 *    area_id : "3",
 *    interface_regexes : [loopback0],
 *    neighbor_regexes : ["node1.*"],
 *  }
 */
struct AreaConfig {
  1: string area_id
  2: list<string> interface_regexes
  3: list<string> neighbor_regexes
}

struct OpenrConfig {
  1: string node_name
  2: string domain
  3: list<AreaConfig> areas = []
  # The IP address to bind to
  4: string listen_addr = "::"
  # Port for the OpenR ctrl thrift service
  5: i32 openr_ctrl_port = 2018

  6: optional bool dryrun
  7: optional bool enable_v4
  # do route programming through netlink
  8: optional bool enable_netlink_fib_handler
  # listen for interface information (link status|addresses) through netlink
  # TODO: will be deprecated soon T66361115
  9: optional bool enable_netlink_system_handler

  # time before decision start compute routes
  10: optional i32 eor_time_s

  11: PrefixForwardingType prefix_forwarding_type = PrefixForwardingType.IP
  12: PrefixForwardingAlgorithm prefix_forwarding_algorithm = PrefixForwardingAlgorithm.SP_ECMP
  13: optional bool enable_segment_routing
  14: optional i32 prefix_min_nexthop

  # Config for different modules
  15: KvstoreConfig kvstore_config
  16: LinkMonitorConfig link_monitor_config
  17: SparkConfig spark_config
  # Watchdog
  18: optional bool enable_watchdog
  19: optional WatchdogConfig watchdog_config
  # prefix allocation
  20: optional bool enable_prefix_allocation
  21: optional PrefixAllocationConfig prefix_allocation_config
  # fib
  22: optional bool enable_ordered_fib_programming
  23: i32 fib_port

  # bgp
  100: optional bool enable_bgp_peering
  102: optional BgpConfig.BgpConfig bgp_config
  103: optional bool bgp_use_igp_metric
}
