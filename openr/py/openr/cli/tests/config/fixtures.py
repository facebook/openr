#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

OPENR_CONFIG_STR = r"""
{
  "adj_hold_time_s": 15,
  "areas": [
    {
      "area_id": "0",
      "exclude_interface_regexes": [
        "eth0",
        "fboss10",
        "fboss2000",
        "fboss11"
      ],
      "include_interface_regexes": [
        "et[0-9].*",
        "po[0-9]+",
        "fboss[0-9]+",
        "if.*"
      ],
      "neighbor_regexes": [
        "rsw.*"
      ],
      "redistribute_interface_regexes": []
    }
  ],
  "assume_drained": true,
  "bgp_config": {},
  "bgp_translation_config": {
    "asn_to_area": {},
    "asns_to_ignore_for_distance": [
      65000
    ],
    "communities_to_name": {},
    "default_source_preference": 100,
    "disable_legacy_translation": true,
    "enable_bgp_to_openr": true,
    "enable_openr_to_bgp": true,
    "is_enabled": false,
    "source_preference_asn": 65000
  },
  "decision_config": {
    "debounce_max_ms": 250,
    "debounce_min_ms": 10,
    "enable_bgp_route_programming": true
  },
  "domain": "p002_f01_vll2",
  "enable_best_route_selection": true,
  "enable_bgp_peering": true,
  "enable_fib_ack": false,
  "enable_fib_service_waiting": true,
  "enable_kvstore_request_queue": true,
  "enable_new_gr_behavior": true,
  "enable_new_prefix_format": true,
  "enable_rib_policy": false,
  "enable_v4": true,
  "enable_watchdog": true,
  "eor_time_s": 90,
  "fib_port": 5909,
  "ip_tos": 192,
  "kvstore_config": {
    "enable_flood_optimization": true,
    "enable_thrift_dual_msg": false,
    "is_flood_root": true,
    "key_ttl_ms": 3600000,
    "ttl_decrement_ms": 1,
    "zmq_hwm": 65536
  },
  "link_monitor_config": {
    "enable_perf_measurement": true,
    "exclude_interface_regexes": [],
    "include_interface_regexes": [],
    "linkflap_initial_backoff_ms": 60000,
    "linkflap_max_backoff_ms": 300000,
    "redistribute_interface_regexes": [],
    "use_rtt_metric": false
  },
  "listen_addr": "::",
  "monitor_config": {
    "enable_event_log_submission": true,
    "max_event_log": 100
  },
  "mpls_route_delete_delay_s": 10,
  "node_name": "fsw008.p002.f01.vll2.tfbnw.net",
  "openr_ctrl_port": 2018,
  "persistent_config_store_path": "/var/facebook/fboss/openr/openr_persistent_config.bin",
  "prefer_openr_originated_routes": false,
  "prefix_forwarding_algorithm": 0,
  "prefix_forwarding_type": 0,
  "prefix_hold_time_s": 20,
  "route_delete_delay_ms": 1000,
  "spark_config": {
    "fastinit_hello_time_ms": 500,
    "graceful_restart_time_s": 120,
    "hello_time_s": 20,
    "hold_time_s": 10,
    "keepalive_time_s": 2,
    "neighbor_discovery_port": 6666,
    "step_detector_conf": {
      "ads_threshold": 500,
      "fast_window_size": 10,
      "lower_threshold": 2,
      "slow_window_size": 60,
      "upper_threshold": 5
    }
  },
  "thrift_server": {
    "acceptable_peers": "svc:openr",
    "ecc_curve_name": "prime256v1",
    "enable_secure_thrift_server": true,
    "listen_addr": "::",
    "openr_ctrl_port": 2018,
    "ticket_seed_path": "/var/facebook/x509_svc/openr_server.pem.seeds",
    "verify_client_type": 1,
    "x509_ca_path": "/var/facebook/rootcanal/ca.pem",
    "x509_cert_path": "/var/facebook/x509_svc/openr_server.pem",
    "x509_key_path": "/var/facebook/x509_svc/openr_server.pem"
  },
  "undrained_flag_path": "/dev/shm/fboss/UNDRAINED",
  "watchdog_config": {
    "interval_s": 20,
    "max_memory_mb": 800,
    "thread_timeout_s": 300
  }
}
"""
