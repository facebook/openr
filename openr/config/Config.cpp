// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/FileUtil.h>
#include <folly/gen/Base.h>
#include <folly/gen/String.h>
#include <glog/logging.h>

#include <openr/common/Flags.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "Config.h"

namespace openr {

Config::Config(const std::string& configFile) {
  std::string contents;
  if (not folly::readFile(configFile.c_str(), contents)) {
    LOG(FATAL) << folly::sformat("Could not read config file: {}", configFile);
  }

  auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
  try {
    jsonSerializer.deserialize(contents, config_);
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Could not parse OpenrConfig struct: "
               << folly::exceptionStr(ex);
    throw;
  }
}

std::shared_ptr<Config>
Config::createConfigFromGflag() {
  thrift::OpenrConfig config;
  config.node_name = FLAGS_node_name;
  config.domain = FLAGS_domain;

  std::vector<std::string> areas;
  folly::split(",", FLAGS_areas, areas, true);
  if (areas.empty()) {
    areas.emplace_back(openr::thrift::KvStore_constants::kDefaultArea());
  }
  for (const auto& area : areas) {
    config.areas.emplace_back(apache::thrift::FRAGILE, area);
  }

  config.listen_addr = FLAGS_listen_addr;
  config.openr_ctrl_port = FLAGS_openr_ctrl_port;

  if (auto v = FLAGS_dryrun) {
    config.dryrun_ref() = v;
  }
  if (auto v = FLAGS_enable_v4) {
    config.enable_v4_ref() = v;
  }
  if (auto v = FLAGS_enable_netlink_fib_handler) {
    config.enable_netlink_fib_handler_ref() = v;
  }
  if (auto v = FLAGS_enable_netlink_system_handler) {
    config.enable_netlink_system_handler_ref() = v;
  }

  config.eor_time_s = FLAGS_decision_graceful_restart_window_s;

  config.prefix_forwarding_type = FLAGS_prefix_fwd_type_mpls
      ? thrift::PrefixForwardingType::SR_MPLS
      : thrift::PrefixForwardingType::IP;
  config.prefix_forwarding_algorithm = FLAGS_prefix_algo_type_ksp2_ed_ecmp
      ? thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP
      : thrift::PrefixForwardingAlgorithm::SP_ECMP;
  if (auto v = FLAGS_enable_segment_routing) {
    config.enable_segment_routing_ref() = v;
  }
  if (FLAGS_bgp_min_nexthop > 0) {
    config.prefix_min_nexthop_ref() = FLAGS_bgp_min_nexthop;
  }

  // KvStore
  auto& kvstoreConf = config.kvstore_config;
  kvstoreConf.flood_msg_per_sec = FLAGS_kvstore_flood_msg_per_sec;
  kvstoreConf.flood_msg_burst_size = FLAGS_kvstore_flood_msg_burst_size;
  kvstoreConf.key_ttl_ms = FLAGS_kvstore_key_ttl_ms;
  kvstoreConf.sync_interval_s = FLAGS_kvstore_sync_interval_s;
  kvstoreConf.ttl_decrement_ms = FLAGS_kvstore_ttl_decrement_ms;
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
    kvstoreConf.key_originator_id_filters_ref() = std::move(orignatorIdFilters);
  }
  // flood optiomization
  if (auto v = FLAGS_enable_flood_optimization) {
    kvstoreConf.enable_flood_optimization_ref() = v;
  }
  if (auto v = FLAGS_is_flood_root) {
    kvstoreConf.is_flood_root_ref() = v;
  }
  if (auto v = FLAGS_use_flood_optimization) {
    kvstoreConf.use_flood_optimization_ref() = v;
  }

  // LinkMonitor
  auto& lmConf = config.link_monitor_config;
  lmConf.linkflap_initial_backoff_ms = FLAGS_link_flap_initial_backoff_ms;
  lmConf.linkflap_max_backoff_ms = FLAGS_link_flap_max_backoff_ms;
  lmConf.use_rtt_metric = FLAGS_enable_rtt_metric;
  folly::split(
      ",", FLAGS_iface_regex_include, lmConf.include_interface_regexes, true);
  folly::split(
      ",", FLAGS_iface_regex_exclude, lmConf.exclude_interface_regexes, true);
  folly::split(
      ",",
      FLAGS_redistribute_ifaces,
      lmConf.redistribute_interface_regexes,
      true);

  // Spark
  auto& sparkConf = config.spark_config;
  sparkConf.neighbor_discovery_port = FLAGS_spark_mcast_port;
  sparkConf.hello_time_s = FLAGS_spark2_hello_time_s;
  sparkConf.fastinit_hello_time_ms = FLAGS_spark2_hello_fastinit_time_ms;
  sparkConf.keepalive_time_s = FLAGS_spark2_heartbeat_time_s;
  sparkConf.hold_time_s = FLAGS_spark2_heartbeat_hold_time_s;
  sparkConf.graceful_restart_time_s = FLAGS_spark_hold_time_s;

  // Watchdog
  if (FLAGS_enable_watchdog) {
    config.enable_watchdog_ref() = FLAGS_enable_watchdog;

    thrift::WatchdogConfig watchdogConfig;
    watchdogConfig.interval_s = FLAGS_watchdog_interval_s;
    watchdogConfig.threshold_s = FLAGS_watchdog_threshold_s;

    config.watchdog_config_ref() = std::move(watchdogConfig);
  }

  // Prefix Allocation
  if (FLAGS_enable_prefix_alloc) {
    config.enable_prefix_allocation_ref() = FLAGS_enable_prefix_alloc;

    thrift::PrefixAllocationConfig pfxAllocConf;
    pfxAllocConf.loopback_interface = FLAGS_loopback_iface;
    pfxAllocConf.seed_prefix = FLAGS_seed_prefix;
    pfxAllocConf.allocate_prefix_len = FLAGS_alloc_prefix_len;
    pfxAllocConf.static_prefix_allocation = FLAGS_static_prefix_alloc;
    pfxAllocConf.set_loopback_addr = FLAGS_set_loopback_address;
    pfxAllocConf.override_loopback_addr = FLAGS_override_loopback_addr;

    config.prefix_allocation_config_ref() = std::move(pfxAllocConf);
  }

  // SPR
  if (FLAGS_enable_plugin) {
    config.enable_spr_ref() = FLAGS_enable_plugin;
    // TODO - add bgp config
    if (auto v = FLAGS_bgp_use_igp_metric) {
      config.bgp_use_igp_metric_ref() = v;
    }
  }
  return std::make_shared<Config>(config);
}

} // namespace openr
