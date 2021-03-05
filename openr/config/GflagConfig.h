/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/gen/Base.h>
#include <folly/gen/String.h>

#include <openr/common/Flags.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/BgpConfig_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_constants.h>

// RSW Confed AS base
constexpr int32_t kRswConfedBase = 2000;

// FSW Confed AS base
constexpr int32_t kFswConfedBase = 6000;

// RSW Local Confed AS number offset relative to device number
constexpr int32_t kRswLocalConfedOffset = 100;

// FSW Local Confed AS number offset relative to device number
constexpr int32_t kFswLocalConfedOffset = 200;

namespace openr {

//
// Class of static functions to generate Config from existing gflags.
// This is for migration use. Separate out to avoid <openr/common/Flags.h>
// got imported unintentionally by importing <openr/config/Config.h>
//
class GflagConfig final {
 public:
  //
  // Migration function to create config from gflag values.
  //
  // This populate thrift::OpenrConfig structure from flag for 2 purposes:
  //   1. internal module could be migrated to use thrift::OpenrConfig
  //   2. Config loaded with '--config' could be compared to config here
  static std::shared_ptr<Config>
  createConfigFromGflag() {
    thrift::OpenrConfig config;
    *config.node_name_ref() = FLAGS_node_name;
    *config.domain_ref() = FLAGS_domain;

    *config.listen_addr_ref() = FLAGS_listen_addr;
    *config.openr_ctrl_port_ref() = FLAGS_openr_ctrl_port;

    if (auto v = FLAGS_dryrun) {
      config.dryrun_ref() = v;
    }
    if (auto v = FLAGS_enable_v4) {
      config.enable_v4_ref() = v;
    }
    if (auto v = FLAGS_enable_netlink_fib_handler) {
      config.enable_netlink_fib_handler_ref() = v;
    }
    if (auto v = FLAGS_decision_graceful_restart_window_s; v >= 0) {
      config.eor_time_s_ref() = v;
    }

    *config.prefix_forwarding_type_ref() = FLAGS_prefix_fwd_type_mpls
        ? thrift::PrefixForwardingType::SR_MPLS
        : thrift::PrefixForwardingType::IP;
    *config.prefix_forwarding_algorithm_ref() =
        FLAGS_prefix_algo_type_ksp2_ed_ecmp
        ? thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP
        : thrift::PrefixForwardingAlgorithm::SP_ECMP;
    if (auto v = FLAGS_enable_segment_routing) {
      config.enable_segment_routing_ref() = v;
    }
    if (FLAGS_bgp_min_nexthop > 0) {
      config.prefix_min_nexthop_ref() = FLAGS_bgp_min_nexthop;
    }

    // KvStore
    auto& kvstoreConf = *config.kvstore_config_ref();
    if (FLAGS_kvstore_flood_msg_per_sec > 0 and
        FLAGS_kvstore_flood_msg_burst_size > 0) {
      thrift::KvstoreFloodRate rate;
      *rate.flood_msg_per_sec_ref() = FLAGS_kvstore_flood_msg_per_sec;
      *rate.flood_msg_burst_size_ref() = FLAGS_kvstore_flood_msg_burst_size;
      kvstoreConf.flood_rate_ref() = rate;
    }

    *kvstoreConf.key_ttl_ms_ref() = FLAGS_kvstore_key_ttl_ms;
    *kvstoreConf.sync_interval_s_ref() = FLAGS_kvstore_sync_interval_s;
    *kvstoreConf.ttl_decrement_ms_ref() = FLAGS_kvstore_ttl_decrement_ms;
    if (FLAGS_set_leaf_node) {
      kvstoreConf.set_leaf_node_ref() = FLAGS_set_leaf_node;
      // prefix filters
      std::vector<std::string> pfxFilters;
      folly::split(",", FLAGS_key_prefix_filters, pfxFilters, true);
      kvstoreConf.key_prefix_filters_ref() = std::move(pfxFilters);
      // originator id filters
      std::vector<std::string> orignatorIdFilters;
      folly::split(
          ",", FLAGS_key_originator_id_filters, orignatorIdFilters, true);
      kvstoreConf.key_originator_id_filters_ref() =
          std::move(orignatorIdFilters);
    }
    // flood optiomization
    if (auto v = FLAGS_enable_flood_optimization) {
      kvstoreConf.enable_flood_optimization_ref() = v;
    }
    if (auto v = FLAGS_is_flood_root) {
      kvstoreConf.is_flood_root_ref() = v;
    }

    // LinkMonitor
    auto& lmConf = *config.link_monitor_config_ref();
    *lmConf.linkflap_initial_backoff_ms_ref() =
        FLAGS_link_flap_initial_backoff_ms;
    *lmConf.linkflap_max_backoff_ms_ref() = FLAGS_link_flap_max_backoff_ms;
    *lmConf.use_rtt_metric_ref() = FLAGS_enable_rtt_metric;
    folly::split(
        ",",
        FLAGS_iface_regex_include,
        *lmConf.include_interface_regexes_ref(),
        true);
    folly::split(
        ",",
        FLAGS_iface_regex_exclude,
        *lmConf.exclude_interface_regexes_ref(),
        true);
    folly::split(
        ",",
        FLAGS_redistribute_ifaces,
        *lmConf.redistribute_interface_regexes_ref(),
        true);

    // Spark
    auto& sparkConf = *config.spark_config_ref();
    *sparkConf.neighbor_discovery_port_ref() = FLAGS_spark_mcast_port;
    *sparkConf.hello_time_s_ref() = FLAGS_spark2_hello_time_s;
    *sparkConf.fastinit_hello_time_ms_ref() =
        FLAGS_spark2_hello_fastinit_time_ms;
    *sparkConf.keepalive_time_s_ref() = FLAGS_spark2_heartbeat_time_s;
    *sparkConf.hold_time_s_ref() = FLAGS_spark2_heartbeat_hold_time_s;
    *sparkConf.graceful_restart_time_s_ref() = FLAGS_spark_hold_time_s;

    // StepDetector
    auto& stepDetectorConf = *sparkConf.step_detector_conf_ref();
    *stepDetectorConf.fast_window_size_ref() =
        FLAGS_step_detector_fast_window_size;
    *stepDetectorConf.slow_window_size_ref() =
        FLAGS_step_detector_slow_window_size;
    *stepDetectorConf.lower_threshold_ref() =
        FLAGS_step_detector_lower_threshold;
    *stepDetectorConf.upper_threshold_ref() =
        FLAGS_step_detector_upper_threshold;
    *stepDetectorConf.ads_threshold_ref() = FLAGS_step_detector_ads_threshold;

    // Monitor
    auto& monitorConf = *config.monitor_config_ref();
    *monitorConf.max_event_log_ref() = FLAGS_monitor_max_event_log;
    *monitorConf.enable_event_log_submission_ref() =
        FLAGS_enable_event_log_submission;

    // Prefix Allocation
    if (FLAGS_enable_prefix_alloc) {
      config.enable_prefix_allocation_ref() = FLAGS_enable_prefix_alloc;

      thrift::PrefixAllocationConfig pfxAllocConf;
      *pfxAllocConf.loopback_interface_ref() = FLAGS_loopback_iface;
      *pfxAllocConf.set_loopback_addr_ref() = FLAGS_set_loopback_address;
      *pfxAllocConf.override_loopback_addr_ref() = FLAGS_override_loopback_addr;
      if (FLAGS_static_prefix_alloc) {
        *pfxAllocConf.prefix_allocation_mode_ref() =
            thrift::PrefixAllocationMode::STATIC;
      } else if (not FLAGS_seed_prefix.empty()) {
        *pfxAllocConf.prefix_allocation_mode_ref() =
            thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE;
        pfxAllocConf.seed_prefix_ref() = FLAGS_seed_prefix;
        pfxAllocConf.allocate_prefix_len_ref() = FLAGS_alloc_prefix_len;
      } else {
        *pfxAllocConf.prefix_allocation_mode_ref() =
            thrift::PrefixAllocationMode::DYNAMIC_LEAF_NODE;
      }

      config.prefix_allocation_config_ref() = std::move(pfxAllocConf);
    }

    // Fib
    if (auto v = FLAGS_enable_ordered_fib_programming) {
      config.enable_ordered_fib_programming_ref() = v;
    }

    // RibPolicy
    *config.enable_rib_policy_ref() = FLAGS_enable_rib_policy;

    // KvStore thrift migration knobs
    if (auto v = FLAGS_enable_kvstore_thrift) {
      *config.enable_kvstore_thrift_ref() = v;
    }
    if (auto v = FLAGS_enable_periodic_sync) {
      *config.enable_periodic_sync_ref() = v;
    }

    return std::make_shared<Config>(config);
  }

  // Generate Bgp configuration based on input arguments
  static thrift::BgpConfig
  getBgpArgConfig() {
    thrift::BgpConfig config;
    thrift::BgpPeer staticPeer;

    // create new config
    *config.router_id_ref() = FLAGS_bgp_router_id;
    *config.local_as_ref() = FLAGS_bgp_local_as;
    if (FLAGS_bgp_is_confed) {
      config.local_confed_as_ref() = FLAGS_bgp_confed_as;
    }
    *config.hold_time_ref() = FLAGS_bgp_hold_time_s;
    config.graceful_restart_convergence_seconds_ref() = FLAGS_bgp_gr_time_s;
    // Bind to ephemeral port. Should not bind to default 179 port to avoid
    // clashing with other bgp instance running.
    // TODO: In long run bgp should support not listening on any port
    *config.listen_port_ref() = 0;

    // static peer (no peer groups)
    *staticPeer.remote_as_ref() = FLAGS_bgp_remote_as;
    *staticPeer.peer_addr_ref() = FLAGS_bgp_peer_addr;
    *staticPeer.local_addr_ref() = FLAGS_bgp_peer_addr;
    *staticPeer.next_hop4_ref() = FLAGS_bgp_nexthop4;
    *staticPeer.next_hop6_ref() = FLAGS_bgp_nexthop6;
    staticPeer.is_rr_client_ref() = FLAGS_bgp_is_rr_client;
    staticPeer.is_confed_peer_ref() = FLAGS_bgp_is_confed;
    staticPeer.next_hop_self_ref() = FLAGS_bgp_nexthop_self;
    staticPeer.enable_stateful_ha_ref() = FLAGS_bgp_enable_stateful_ha;

    *config.peers_ref() = {staticPeer};
    return config;
  }

  // Generate Bgp configuration automatically
  static thrift::BgpConfig
  getBgpAutoConfig() {
    thrift::BgpConfig config;
    thrift::BgpPeer staticPeer;
    auto deviceName = FLAGS_node_name;

    *config.router_id_ref() = FLAGS_bgp_router_id;
    // Local confed as is sufficient, local as is unused anyways
    *config.local_as_ref() = FLAGS_bgp_local_as;

    if (deviceName.substr(0, 3) == "rsw") {
      // rsw001
      auto rswNum = folly::to<uint32_t>(deviceName.substr(3, 3));
      config.local_confed_as_ref() =
          kRswConfedBase + kRswLocalConfedOffset + rswNum;
      *staticPeer.remote_as_ref() = kRswConfedBase + rswNum;
    } else if (deviceName.substr(0, 3) == "fsw") {
      // fsw001
      auto fswNum = folly::to<uint32_t>(deviceName.substr(3, 3));
      config.local_confed_as_ref() =
          kRswConfedBase + kFswLocalConfedOffset + fswNum;
      *staticPeer.remote_as_ref() = kFswConfedBase + fswNum;
    } else {
      LOG(FATAL) << "Unsupported device to enable spr " << deviceName;
    }
    *config.hold_time_ref() = FLAGS_bgp_hold_time_s;
    config.graceful_restart_convergence_seconds_ref() = FLAGS_bgp_gr_time_s;
    // Bind to ephemeral port. Should not bind to default 179 port to avoid
    // clashing with other bgp instance running.
    *config.listen_port_ref() = 0;

    // static peer (no peer groups)
    *staticPeer.peer_addr_ref() = FLAGS_bgp_peer_addr;
    *staticPeer.local_addr_ref() = FLAGS_bgp_peer_addr;
    *staticPeer.next_hop4_ref() = FLAGS_bgp_nexthop4;
    *staticPeer.next_hop6_ref() = FLAGS_bgp_nexthop6;
    staticPeer.next_hop_self_ref() = false;
    staticPeer.is_rr_client_ref() = false;
    staticPeer.is_confed_peer_ref() = true;
    staticPeer.enable_stateful_ha_ref() = true;

    *config.peers_ref() = {staticPeer};
    return config;
  }

  // Generate Bgp configuration
  static thrift::BgpConfig
  getBgpConfig() {
    // Validate input arguments
    if (!folly::IPAddress::validate(FLAGS_bgp_router_id)) {
      LOG(FATAL) << "Invalid bgp_router_id " << FLAGS_bgp_router_id;
    }
    if (!folly::IPAddress::validate(FLAGS_bgp_peer_addr)) {
      LOG(FATAL) << "Invalid bgp_peer_addr " << FLAGS_bgp_peer_addr;
    }
    if (!folly::IPAddress::validate(FLAGS_bgp_nexthop4)) {
      LOG(FATAL) << "Invalid bgp_nexthop4 " << FLAGS_bgp_nexthop4;
    }
    if (!folly::IPAddress::validate(FLAGS_bgp_nexthop6)) {
      LOG(FATAL) << "Invalid bgp_nexthop6 " << FLAGS_bgp_nexthop6;
    }

    return FLAGS_bgp_override_auto_config ? getBgpArgConfig()
                                          : getBgpAutoConfig();
  }
};

} // namespace openr
