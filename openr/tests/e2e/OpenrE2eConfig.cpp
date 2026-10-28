/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrE2eConfig.h>

#include <stdexcept>

#include <openr/tests/utils/Utils.h>

namespace openr::e2e {

thrift::OpenrConfig
makeOpenrE2eConfig(
    const std::string& nodeName,
    uint16_t fibPort,
    uint16_t kvStorePort,
    const std::string& persistentStorePath) {
  if (nodeName.empty() || fibPort == 0 || kvStorePort == 0 ||
      persistentStorePath.empty()) {
    throw std::invalid_argument(
        "Open/R E2E requires a node, ports, and state path");
  }

  auto config = getBasicOpenrConfig(
      nodeName,
      {createAreaConfig("0", {".*"}, {".*"})},
      /*enableV4=*/true,
      /*dryrun=*/false);
  config.fib_port() = fibPort;
  config.thrift_server()->openr_ctrl_port() = kvStorePort;
  config.thrift_server()->kvstore_peer_port() = kvStorePort;
  config.persistent_config_store_path() = persistentStorePath;
  config.enable_watchdog() = false;
  config.enable_neighbor_monitor() = false;
  config.route_delete_delay_ms() = 0;

  auto& linkMonitor = *config.link_monitor_config();
  linkMonitor.linkflap_initial_backoff_ms() = 1;
  linkMonitor.linkflap_max_backoff_ms() = 8;
  linkMonitor.use_rtt_metric() = false;

  auto& decision = *config.decision_config();
  decision.debounce_min_ms() = 10;
  decision.debounce_max_ms() = 50;

  auto& spark = *config.spark_config();
  spark.hello_time_s() = 1;
  spark.fastinit_hello_time_ms() = 20;
  spark.keepalive_time_s() = 1;
  spark.hold_time_s() = 2;
  spark.graceful_restart_time_s() = 6;
  return config;
}

} // namespace openr::e2e
