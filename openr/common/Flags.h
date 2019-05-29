/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gflags/gflags.h>

#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/HealthChecker_types.h>

DECLARE_int32(openr_ctrl_port);

DECLARE_int32(kvstore_pub_port);
DECLARE_int32(kvstore_rep_port);

DECLARE_int32(decision_pub_port);
DECLARE_int32(decision_rep_port);

DECLARE_bool(enable_plugin);

DECLARE_int32(link_monitor_pub_port);
DECLARE_int32(link_monitor_cmd_port);

DECLARE_int32(monitor_pub_port);
DECLARE_int32(monitor_rep_port);

DECLARE_int32(fib_rep_port);
DECLARE_int32(health_checker_port);
DECLARE_int32(prefix_manager_cmd_port);
DECLARE_int32(health_checker_rep_port);
DECLARE_int32(system_agent_port);
DECLARE_int32(fib_handler_port);
DECLARE_int32(spark_mcast_port);
DECLARE_string(platform_pub_url);
DECLARE_string(domain);
DECLARE_string(chdir);
DECLARE_string(listen_addr);
DECLARE_string(config_store_filepath);
DECLARE_bool(assume_drained);
DECLARE_string(node_name);
DECLARE_bool(dryrun);
DECLARE_string(loopback_iface);
DECLARE_string(prefixes);
DECLARE_string(seed_prefix);

DECLARE_bool(enable_prefix_alloc);
DECLARE_int32(alloc_prefix_len);
DECLARE_bool(static_prefix_alloc);

DECLARE_bool(set_loopback_address);
DECLARE_bool(override_loopback_addr);

DECLARE_string(ifname_prefix);
DECLARE_string(iface_regex_include);
DECLARE_string(iface_regex_exclude);
DECLARE_string(redistribute_ifaces);

DECLARE_string(cert_file_path);

DECLARE_bool(enable_encryption);
DECLARE_bool(enable_rtt_metric);
DECLARE_bool(enable_v4);
DECLARE_bool(enable_subnet_validation);
DECLARE_bool(enable_lfa);
DECLARE_bool(enable_ordered_fib_programming);
DECLARE_bool(enable_bgp_route_programming);

DECLARE_bool(enable_spark);

DECLARE_int32(spark_hold_time_s);
DECLARE_int32(spark_keepalive_time_s);
DECLARE_int32(spark_fastinit_keepalive_time_ms);

DECLARE_string(spark_report_url);
DECLARE_string(spark_cmd_url);

DECLARE_int32(health_checker_ping_interval_s);
DECLARE_bool(enable_health_checker);
DECLARE_bool(enable_fib_sync);
DECLARE_int32(health_check_option);
DECLARE_int32(health_check_pct);

DECLARE_bool(enable_netlink_fib_handler);
DECLARE_bool(enable_netlink_system_handler);

DECLARE_int32(ip_tos);
DECLARE_int32(zmq_context_threads);

DECLARE_int32(link_flap_initial_backoff_ms);
DECLARE_int32(link_flap_max_backoff_ms);

DECLARE_bool(enable_perf_measurement);

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
DECLARE_bool(enable_legacy_flooding);

DECLARE_int32(kvstore_zmq_hwm);
DECLARE_int32(kvstore_flood_msg_per_sec);
DECLARE_int32(kvstore_flood_msg_burst_size);
DECLARE_int32(kvstore_key_ttl_ms);
DECLARE_int32(kvstore_ttl_decrement_ms);

DECLARE_bool(enable_secure_thrift_server);
DECLARE_string(x509_cert_path);
DECLARE_string(x509_key_path);
DECLARE_string(x509_ca_path);
DECLARE_string(tls_ticket_seed_path);
DECLARE_string(tls_ecc_curve_name);
DECLARE_string(tls_acceptable_peers);

DECLARE_int32(persistent_store_initial_backoff_ms);
DECLARE_int32(persistent_store_max_backoff_ms);

DECLARE_bool(enable_flood_optimization);
DECLARE_bool(is_flood_root);
DECLARE_bool(use_flood_optimization);

DECLARE_bool(use_netlink_message);
DECLARE_bool(prefix_fwd_type_mpls);
