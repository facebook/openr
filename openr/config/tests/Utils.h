#pragma once

#include <openr/config/Config.h>

namespace {
openr::thrift::OpenrConfig
getBasicOpenrConfig() {
  openr::thrift::LinkMonitorConfig linkMonitorConfig;
  linkMonitorConfig.include_interface_regexes =
      std::vector<std::string>{"et[0-9].*"};
  linkMonitorConfig.exclude_interface_regexes =
      std::vector<std::string>{"eth0"};
  linkMonitorConfig.redistribute_interface_regexes =
      std::vector<std::string>{"lo1"};

  openr::thrift::KvstoreConfig kvstoreConfig;
  kvstoreConfig.enable_flood_optimization = true;
  kvstoreConfig.use_flood_optimization = true;
  kvstoreConfig.is_flood_root = true;

  openr::thrift::SparkConfig sparkConfig;
  sparkConfig.graceful_restart_time_s = 60;

  openr::thrift::WatchdogConfig watchdogConfig;

  openr::thrift::OpenrConfig config;

  config.node_name = "";
  config.domain = "domain";
  config.enable_v4 = true;
  config.enable_netlink_system_handler = true;
  config.kvstore_config = kvstoreConfig;
  config.link_monitor_config = linkMonitorConfig;
  config.spark_config = sparkConfig;
  config.watchdog_config = watchdogConfig;

  return config;
}
} // namespace
