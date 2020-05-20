#pragma once

#include <openr/config/Config.h>

namespace {
openr::thrift::OpenrConfig
getBasicOpenrConfig(
    const std::string nodeName = "",
    bool enableV4 = true,
    bool enableSegmentRouting = false,
    bool orderedFibProgramming = false,
    bool dryrun = true) {
  openr::thrift::LinkMonitorConfig linkMonitorConfig;
  linkMonitorConfig.include_interface_regexes =
      std::vector<std::string>{"et[0-9].*"};
  linkMonitorConfig.exclude_interface_regexes =
      std::vector<std::string>{"eth0"};
  linkMonitorConfig.redistribute_interface_regexes =
      std::vector<std::string>{"lo1"};

  openr::thrift::KvstoreConfig kvstoreConfig;

  openr::thrift::SparkConfig sparkConfig;
  sparkConfig.graceful_restart_time_s = 60;

  openr::thrift::OpenrConfig config;

  config.node_name = nodeName;
  config.domain = "domain";
  config.enable_v4_ref() = enableV4;
  config.enable_segment_routing_ref() = enableSegmentRouting;
  config.enable_ordered_fib_programming_ref() = orderedFibProgramming;
  config.dryrun_ref() = dryrun;
  config.enable_netlink_system_handler_ref() = true;

  config.kvstore_config = kvstoreConfig;
  config.link_monitor_config = linkMonitorConfig;
  config.spark_config = sparkConfig;

  return config;
}
} // namespace
