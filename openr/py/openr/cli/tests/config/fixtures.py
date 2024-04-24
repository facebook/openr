#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
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
  "decision_config": {
    "debounce_max_ms": 250,
    "debounce_min_ms": 10
  },
  "domain": "p002_f01_vll2",
  "enable_best_route_selection": true,
  "enable_bgp_peering": true,
  "enable_rib_policy": false,
  "enable_v4": true,
  "enable_watchdog": true,
  "fib_port": 5909,
  "ip_tos": 192,
  "kvstore_config": {
    "key_ttl_ms": 3600000,
    "ttl_decrement_ms": 1
  },
  "link_monitor_config": {
    "enable_perf_measurement": true,
    "linkflap_initial_backoff_ms": 60000,
    "linkflap_max_backoff_ms": 300000,
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
