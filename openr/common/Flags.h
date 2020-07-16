/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gflags/gflags.h>

#include <openr/common/Constants.h>

DECLARE_int32(openr_ctrl_port);
DECLARE_int32(kvstore_rep_port);

DECLARE_bool(enable_plugin);

DECLARE_string(areas);

DECLARE_int32(monitor_pub_port);
DECLARE_int32(monitor_rep_port);

DECLARE_int32(system_agent_port);
DECLARE_int32(fib_handler_port);
DECLARE_int32(spark_mcast_port);
DECLARE_string(platform_pub_url);
DECLARE_string(domain);
DECLARE_string(listen_addr);
DECLARE_string(config_store_filepath);
DECLARE_bool(assume_drained);
DECLARE_string(node_name);
DECLARE_bool(dryrun);
DECLARE_string(loopback_iface);
DECLARE_string(seed_prefix);

DECLARE_bool(enable_prefix_alloc);
DECLARE_int32(alloc_prefix_len);
DECLARE_bool(static_prefix_alloc);
DECLARE_bool(per_prefix_keys);

DECLARE_bool(set_loopback_address);
DECLARE_bool(override_loopback_addr);

DECLARE_string(iface_regex_include);
DECLARE_string(iface_regex_exclude);
DECLARE_string(redistribute_ifaces);

DECLARE_string(cert_file_path);

DECLARE_bool(enable_encryption);
DECLARE_bool(enable_fib_service_waiting);
DECLARE_bool(enable_rtt_metric);
DECLARE_bool(enable_v4);
DECLARE_bool(enable_lfa);
DECLARE_bool(enable_ordered_fib_programming);
DECLARE_bool(enable_bgp_route_programming);
DECLARE_bool(bgp_use_igp_metric);

DECLARE_int32(decision_graceful_restart_window_s);

DECLARE_int32(spark_hold_time_s);
DECLARE_int32(spark_keepalive_time_s);
DECLARE_int32(spark_fastinit_keepalive_time_ms);

DECLARE_uint64(step_detector_fast_window_size);
DECLARE_uint64(step_detector_slow_window_size);
DECLARE_uint32(step_detector_lower_threshold);
DECLARE_uint32(step_detector_upper_threshold);
DECLARE_uint64(step_detector_ads_threshold);

DECLARE_bool(enable_netlink_fib_handler);
DECLARE_bool(enable_netlink_system_handler);

DECLARE_int32(ip_tos);

DECLARE_int32(link_flap_initial_backoff_ms);
DECLARE_int32(link_flap_max_backoff_ms);

DECLARE_bool(enable_perf_measurement);

DECLARE_bool(enable_rib_policy);

DECLARE_int32(decision_debounce_min_ms);
DECLARE_int32(decision_debounce_max_ms);

DECLARE_bool(enable_watchdog);
DECLARE_int32(watchdog_interval_s);
DECLARE_int32(watchdog_threshold_s);

DECLARE_bool(enable_segment_routing);
DECLARE_bool(set_leaf_node);

DECLARE_string(key_prefix_filters);
DECLARE_string(key_originator_id_filters);

DECLARE_int32(memory_limit_mb);

DECLARE_int32(kvstore_zmq_hwm);
DECLARE_int32(kvstore_flood_msg_per_sec);
DECLARE_int32(kvstore_flood_msg_burst_size);
DECLARE_int32(kvstore_key_ttl_ms);
DECLARE_int32(kvstore_sync_interval_s);
DECLARE_int32(kvstore_ttl_decrement_ms);

DECLARE_bool(enable_secure_thrift_server);
DECLARE_string(x509_cert_path);
DECLARE_string(x509_key_path);
DECLARE_string(x509_ca_path);
DECLARE_string(tls_ticket_seed_path);
DECLARE_string(tls_ecc_curve_name);
DECLARE_string(tls_acceptable_peers);

DECLARE_bool(enable_flood_optimization);
DECLARE_bool(is_flood_root);
DECLARE_bool(use_flood_optimization);

DECLARE_bool(enable_spark2);
DECLARE_bool(spark2_increase_hello_interval);
DECLARE_int32(spark2_hello_time_s);
DECLARE_int32(spark2_hello_fastinit_time_ms);
DECLARE_int32(spark2_heartbeat_time_s);
DECLARE_int32(spark2_handshake_time_ms);
DECLARE_int32(spark2_negotiate_hold_time_s);
DECLARE_int32(spark2_heartbeat_hold_time_s);

DECLARE_bool(enable_kvstore_thrift);

DECLARE_bool(prefix_fwd_type_mpls);
DECLARE_bool(prefix_algo_type_ksp2_ed_ecmp);

DECLARE_int32(bgp_local_as);
DECLARE_string(bgp_router_id);
DECLARE_int32(bgp_hold_time_s);
DECLARE_int32(bgp_gr_time_s);
DECLARE_string(bgp_peer_addr);
DECLARE_int32(bgp_confed_as);
DECLARE_int32(bgp_remote_as);
DECLARE_bool(bgp_is_confed);
DECLARE_bool(bgp_is_rr_client);
DECLARE_int32(bgp_thrift_port);
DECLARE_string(bgp_nexthop4);
DECLARE_string(bgp_nexthop6);
DECLARE_bool(bgp_nexthop_self);
DECLARE_bool(bgp_override_auto_config);
DECLARE_string(spr_ha_state_file);
DECLARE_bool(bgp_enable_stateful_ha);
DECLARE_uint32(bgp_min_nexthop);
DECLARE_int32(add_path);

DECLARE_uint32(monitor_max_event_log);

DECLARE_string(config);
