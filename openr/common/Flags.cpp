/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/Flags.h"
#include <openr/if/gen-cpp2/KvStore_constants.h>

DEFINE_int32(
    openr_ctrl_port,
    openr::Constants::kOpenrCtrlPort,
    "Port for the OpenR ctrl thrift service");
DEFINE_int32(
    kvstore_rep_port,
    openr::Constants::kKvStoreRepPort,
    "The port KvStore replier listens on");
// Flag to enable or disable plugin module.
DEFINE_bool(enable_plugin, false, "Enable plugin module");
DEFINE_string(
    areas,
    openr::thrift::KvStore_constants::kDefaultArea(),
    "Comma separated list of areas name specified as string");
DEFINE_int32(
    monitor_pub_port,
    openr::Constants::kMonitorPubPort,
    "The port monitor publishes on");
DEFINE_int32(
    monitor_rep_port,
    openr::Constants::kMonitorRepPort,
    "The port monitor replies on");
DEFINE_int32(
    system_agent_port,
    openr::Constants::kSystemAgentPort,
    "Switch agent thrift service port for Platform programming.");
DEFINE_int32(
    fib_handler_port,
    openr::Constants::kFibAgentPort, // NOTE 100 is on purpose
    "Switch agent thrift service port for FIB programming.");
DEFINE_int32(
    spark_mcast_port,
    openr::Constants::kSparkMcastPort,
    "Spark UDP multicast port for sending spark-hello messages.");
DEFINE_string(
    platform_pub_url,
    "ipc:///tmp/platform-pub-url",
    "Publisher URL for interface/address notifications");
DEFINE_string(
    domain,
    "terragraph",
    "Domain name associated with this OpenR. No adjacencies will be formed "
    "to OpenR of other domains.");
DEFINE_string(listen_addr, "*", "The IP address to bind to");
DEFINE_string(
    config_store_filepath,
    "/tmp/aq_persistent_config_store.bin",
    "File name where to persist OpenR's internal state across restarts");
DEFINE_bool(
    assume_drained,
    false,
    "If set, will assume node is drained if no drain state is found in the "
    "persistent store");
DEFINE_string(
    node_name,
    "node1",
    "The name of current node (also serves as originator id");
DEFINE_bool(
    dryrun, true, "Run the process in dryrun mode. No FIB programming!");
DEFINE_string(loopback_iface, "lo", "The iface to configure with the prefix");
DEFINE_string(
    seed_prefix,
    "",
    "The seed prefix all subprefixes are to be allocated from. If empty, "
    "it will be injected later together with allocated prefix length");
DEFINE_bool(enable_prefix_alloc, false, "Enable automatic prefix allocation");
DEFINE_int32(alloc_prefix_len, 128, "Allocated prefix length");
DEFINE_bool(static_prefix_alloc, false, "Perform static prefix allocation");
DEFINE_bool(per_prefix_keys, false, "Create per IP prefix keys in Kvstore");
DEFINE_bool(
    set_loopback_address,
    false,
    "Set the IP addresses from supplied prefix param to loopback (/128)");
DEFINE_bool(
    override_loopback_addr,
    false,
    "If enabled then all global addresses assigned on loopback will be flushed "
    "whenever OpenR elects new prefix for node. Only effective when prefix "
    "allocator is turned on and set_loopback_address is also turned on");
DEFINE_string(
    iface_regex_include,
    "",
    "A comma separated list of extended POSIX regular expressions. Linux "
    "interface names containing a match (case insensitive) to at least one of "
    "these and not excluded by the flag iface_regex_exclude will be used for "
    "neighbor discovery");
DEFINE_string(
    iface_regex_exclude,
    "",
    "A comma separated list of extended POSIX regular expressions. Linux "
    "interface names containing a match (case insensitive) to at least one of "
    "these will not be used for neighbor discovery");
DEFINE_string(
    redistribute_ifaces,
    "",
    "The interface names or regex who's prefixes we want to advertise");
DEFINE_string(
    cert_file_path,
    "/tmp/cert_node_1.json",
    "my certificate file containing private & public key pair");
DEFINE_bool(enable_encryption, false, "Encrypt traffic between AQ instances");
DEFINE_bool(
    enable_fib_service_waiting,
    true,
    "Wait for Switch FIB agent to be ready before initialize OpenR");
DEFINE_bool(
    enable_rtt_metric,
    true,
    "Use dynamically learned RTT for interface metric values.");
DEFINE_bool(
    enable_v4,
    false,
    "Enable v4 in OpenR for exchanging and programming v4 routes. Works only "
    "when Switch FIB Agent is used for FIB programming. No NSS/Linux.");
DEFINE_bool(
    enable_lfa, false, "Enable LFA computation for quick reroute per RFC 5286");
DEFINE_bool(
    enable_ordered_fib_programming,
    false,
    "Enable ordered fib programming per RFC 6976");
DEFINE_bool(
    enable_bgp_route_programming,
    true,
    "Enable programming routes with prefix type BGP to the system FIB");
DEFINE_bool(
    bgp_use_igp_metric,
    false,
    "Use IGP metric from Open/R for BGP metric vector comparision");
DEFINE_int32(
    decision_graceful_restart_window_s,
    -1,
    "Duration (in seconds) to wait for convergence upon restart before "
    "calculating new routes. Set to negative value to disable.");
DEFINE_int32(
    spark_hold_time_s,
    18,
    "How long (in seconds) to keep neighbor adjacency without receiving any "
    "hello packets.");
DEFINE_int32(
    spark_keepalive_time_s,
    2,
    "Keep-alive message interval (in seconds) for spark hello message "
    "exchanges. At most 2 hello message exchanges are required for graceful "
    "restart.");
DEFINE_int32(
    spark_fastinit_keepalive_time_ms,
    100,
    "Fast initial keep alive time (in milliseconds)");
DEFINE_int32(
    spark2_hello_time_s,
    20,
    "Hello msg interval (in seconds) to do node advertisement");
DEFINE_int32(
    spark2_hello_fastinit_time_ms,
    500,
    "Fast init hello msg interval (in milliseconds) to do node advertisement");
DEFINE_int32(
    spark2_heartbeat_time_s,
    1,
    "Heartbeat msg interval (in seconds) to keep alive for this node");
DEFINE_int32(
    spark2_handshake_time_ms,
    500,
    "Handshake msg interval (in milliseconds) to negotiate param for "
    "adjacency establishment.");
DEFINE_int32(
    spark2_negotiate_hold_time_s,
    5,
    "How long (in seconds) to stay in negotiate state. Should form "
    "adjacency within this period of time.");
DEFINE_int32(
    spark2_heartbeat_hold_time_s,
    5,
    "How long (in seconds) to keep neighbor adjacency without receiving "
    "any heartbeat packet in stable state.");
DEFINE_uint64(
    step_detector_fast_window_size,
    10,
    "Number of samples in fast sliding window for StepDetector");
DEFINE_uint64(
    step_detector_slow_window_size,
    60,
    "Number of samples in slow sliding window for StepDetector");
DEFINE_uint32(
    step_detector_lower_threshold,
    2,
    "Lower threshold in percentage for StepDetector");
DEFINE_uint32(
    step_detector_upper_threshold,
    5,
    "Upper threshold in percentage for StepDetector");
DEFINE_uint64(
    step_detector_ads_threshold,
    500,
    "Absolute step threshold to detect gradual change for StepDetector");
DEFINE_bool(
    enable_netlink_fib_handler,
    false,
    "If set, netlink fib handler will be started for route programming.");
DEFINE_bool(
    enable_netlink_system_handler,
    true,
    "If set, netlink system handler will be started");
DEFINE_int32(
    ip_tos,
    openr::Constants::kIpTos,
    "Mark control plane traffic with specified IP-TOS value. Set this to 0 "
    "if you don't want to mark packets.");
DEFINE_int32(
    link_flap_initial_backoff_ms,
    1000,
    "Initial backoff to dampen link flaps (in milliseconds)");
DEFINE_int32(
    link_flap_max_backoff_ms,
    60000,
    "Max backoff to dampen link flaps (in millseconds)");
DEFINE_bool(
    enable_perf_measurement,
    true,
    "Enable performance measurement in network.");
DEFINE_bool(enable_rib_policy, false, "Enable RibPolicy in Decision module");
DEFINE_int32(
    decision_debounce_min_ms,
    10,
    "Fast reaction time to update decision spf upon receiving adj db update "
    "(in milliseconds)");
DEFINE_int32(
    decision_debounce_max_ms,
    250,
    "Decision debounce time to update spf in frequent adj db update "
    "(in milliseconds)");
DEFINE_bool(
    enable_watchdog,
    true,
    "Enable watchdog thread to periodically check aliveness counters from each "
    "openr thread, if unhealthy thread is detected, force crash openr");
DEFINE_int32(watchdog_interval_s, 20, "Watchdog thread healthcheck interval");
DEFINE_int32(watchdog_threshold_s, 300, "Watchdog thread aliveness threshold");
DEFINE_bool(
    enable_segment_routing, false, "Flag to disable/enable segment routing");
DEFINE_bool(set_leaf_node, false, "Flag to enable/disable node as a leaf node");
DEFINE_string(
    key_prefix_filters,
    "",
    "Only keys matching any of the prefixes in the list "
    "will be added to kvstore");
DEFINE_string(
    key_originator_id_filters,
    "",
    "Only keys with originator ID matching any of the originator ID will "
    "be added to kvstore.");
DEFINE_int32(memory_limit_mb, 300, "Memory limit in MB");
DEFINE_bool(
    enable_kvstore_thrift,
    false,
    "Flag to enable KvStore external msg exchanging over thrift");
DEFINE_bool(
    enable_periodic_sync,
    true,
    "Flag to enable KvStore periodic sync over ZMQ");
DEFINE_int32(
    kvstore_zmq_hwm,
    openr::Constants::kHighWaterMark,
    "Max number of packets to hold in kvstore ZMQ socket queue per peer");
DEFINE_int32(
    kvstore_flood_msg_per_sec,
    0,
    "Rate of Kvstore flooding in number of messages per second");
DEFINE_int32(
    kvstore_flood_msg_burst_size,
    0,
    "Burst size of Kvstore flooding in number of messages");
DEFINE_int32(
    kvstore_key_ttl_ms,
    openr::Constants::kKvStoreDbTtl.count(), // 5 min
    "TTL of a key (in ms) in the Kvstore");
DEFINE_int32(
    kvstore_sync_interval_s,
    openr::Constants::kStoreSyncInterval.count(),
    "Kvstore periodic random node sync interval in seconds");
DEFINE_int32(
    kvstore_ttl_decrement_ms,
    openr::Constants::kTtlDecrement.count(),
    "Amount of time to decrement TTL when flooding updates");
DEFINE_bool(
    enable_secure_thrift_server,
    false,
    "Flag to enable TLS for our thrift server");
DEFINE_string(
    x509_cert_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "certificate path for the associated wangle::SSLContextConfig");
DEFINE_string(
    x509_key_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "key path for the associated wangle::SSLContextConfig. If unspecified, "
    "will use x509_cert_path");
DEFINE_string(
    x509_ca_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "certificate authority path for verifying peers");
DEFINE_string(
    tls_ticket_seed_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "TLS ticket seed file path to use for client session resumption");
DEFINE_string(
    tls_ecc_curve_name,
    "prime256v1",
    "If we are running an SSL thrift server, this option specifies the "
    "eccCurveName for the associated wangle::SSLContextConfig");
DEFINE_string(
    tls_acceptable_peers,
    "",
    "A comma separated list of strings. Strings are x509 common names to "
    "accept SSL connections from. If an empty string is provided, the server "
    "will accept connections from any authenticated peer.");
DEFINE_bool(enable_flood_optimization, false, "Enable flooding optimization");
DEFINE_bool(is_flood_root, false, "set myself as flooding root or not");
// TODO this option will be deprecated in near future, this is just for safely
// rollout purpose
DEFINE_bool(
    use_flood_optimization,
    false,
    "Enable this to use formed flooding topology to flood updates");
DEFINE_bool(enable_spark2, false, "Enable Spark2 support");
DEFINE_bool(
    spark2_increase_hello_interval,
    false,
    "Increase Spark2 hello msg interval");
DEFINE_bool(
    prefix_fwd_type_mpls,
    false,
    "Advertise prefix forwarding type as SR MPLS to use label forwarding");
DEFINE_bool(
    prefix_algo_type_ksp2_ed_ecmp,
    false,
    "Advertise prefix algorithm type as 2-Shortest paths Edge Disjoint ECMP");

// Local AS number
// TODO: Exact config values needs to be decided
DEFINE_int32(bgp_local_as, 61234, "Bgp local AS number");

// Router id
DEFINE_string(bgp_router_id, "169.0.0.1", "Bgp router id");

// Hold time
DEFINE_int32(bgp_hold_time_s, 30, "Bgp hold time in seconds");

// GR time
DEFINE_int32(bgp_gr_time_s, 120, "Bgp graceful restart time in seconds");

// Peer address
DEFINE_string(bgp_peer_addr, "::1", "Bgp peer IPv4 address");

// Confed AS
DEFINE_int32(bgp_confed_as, 6001, "Bgp confed AS number");

// Peer As
DEFINE_int32(bgp_remote_as, 2028, "Bgp peer AS number");

// is confed peer
DEFINE_bool(bgp_is_confed, false, "Is Bgp peer confed");

// is rr client
DEFINE_bool(bgp_is_rr_client, false, "Is Bgp peer rr client");

// Thrift service port
DEFINE_int32(bgp_thrift_port, 2029, "port for thrift service");

// nexthop ipv4
DEFINE_string(bgp_nexthop4, "0.0.0.0", "Bgp nexthop ipv4");

// nexthop ipv4
DEFINE_string(bgp_nexthop6, "::", "Bgp nexthop ipv6");

// set nexthop self
DEFINE_bool(bgp_nexthop_self, false, "Bgp nexthop self");

// Override bgp auto config with arguments passed
DEFINE_bool(bgp_override_auto_config, false, "Override BGP auto config");

DEFINE_string(
    spr_ha_state_file,
    "/dev/shm/spr_ha_state.txt",
    "File in which HA stateful information is stored across bgp restarts");

// bgp stateful ha
DEFINE_bool(bgp_enable_stateful_ha, true, "Is Bgp peer stateful HA required");

DEFINE_uint32(
    bgp_min_nexthop,
    0,
    "Min nexthops for a route to be considered to program/announce."
    " This would only work for prefix announcing with ksp2 "
    "algorithm. Right now, only used in turbo-fabric setup");

DEFINE_int32(add_path, 0, "add path value");

DEFINE_uint32(
    monitor_max_event_log, 100, "Max number for storing recent event logs");

DEFINE_string(config, "", "OpenR config file path");
