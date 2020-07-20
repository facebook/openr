#pragma once

#include <openr/config/Config.h>

namespace {
openr::thrift::OpenrConfig
getBasicOpenrConfig(
    const std::string nodeName = "",
    const std::string domainName = "domain",
    const std::vector<openr::thrift::AreaConfig>& areaCfg = {},
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
  sparkConfig.hello_time_s = 2;
  sparkConfig.keepalive_time_s = 1;
  sparkConfig.fastinit_hello_time_ms = 50;
  sparkConfig.hold_time_s = 2;
  sparkConfig.graceful_restart_time_s = 6;

  openr::thrift::OpenrConfig config;

  config.node_name = nodeName;
  config.domain = domainName;
  config.enable_v4_ref() = enableV4;
  config.enable_segment_routing_ref() = enableSegmentRouting;
  config.enable_ordered_fib_programming_ref() = orderedFibProgramming;
  config.dryrun_ref() = dryrun;

  config.kvstore_config = kvstoreConfig;
  config.link_monitor_config = linkMonitorConfig;
  config.spark_config = sparkConfig;

  config.enable_rib_policy = true;

  if (areaCfg.empty()) {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id = "0";
    areaConfig.neighbor_regexes = {".*"};
    areaConfig.interface_regexes = {".*"};
    config.areas.emplace_back(areaConfig);
  } else {
    for (const auto& areaCfg : areaCfg) {
      config.areas.emplace_back(areaCfg);
    }
  }

  return config;
}
} // namespace
