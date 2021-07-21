/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/Constants.h>
#include <openr/common/MplsUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <optional>

#include <openr/if/gen-cpp2/BgpConfig_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace {

// utility function to construct thrift::AreaConfig
openr::thrift::AreaConfig
createAreaConfig(
    const std::string& areaId,
    const std::vector<std::string>& neighborRegexes,
    const std::vector<std::string>& interfaceRegexes,
    const std::optional<std::string>& policy = std::nullopt,
    const bool enableAdjLabels = false) {
  openr::thrift::AreaConfig areaConfig;
  areaConfig.area_id_ref() = areaId;
  areaConfig.neighbor_regexes_ref() = neighborRegexes;
  areaConfig.include_interface_regexes_ref() = interfaceRegexes;
  if (policy) {
    areaConfig.set_import_policy_name(*policy);
  }

  if (enableAdjLabels) {
    openr::thrift::SegmentRoutingAdjLabelType sr_adj_label_type;
    openr::thrift::SegmentRoutingAdjLabel sr_adj_label;
    openr::thrift::LabelRange lr;

    lr.start_label_ref() = openr::MplsConstants::kSrLocalRange.first;
    lr.end_label_ref() = openr::MplsConstants::kSrLocalRange.second;
    sr_adj_label_type = openr::thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX;
    sr_adj_label.sr_adj_label_type_ref() = sr_adj_label_type;
    sr_adj_label.adj_label_range_ref() = lr;
    areaConfig.sr_adj_label_ref() = sr_adj_label;
  }
  return areaConfig;
}

openr::thrift::OpenrConfig
getBasicOpenrConfig(
    const std::string nodeName = "",
    const std::string domainName = "domain",
    const std::vector<openr::thrift::AreaConfig>& areaCfg = {},
    bool enableV4 = true,
    bool enableSegmentRouting = false,
    bool dryrun = true,
    bool enableV4OverV6Nexthop = false,
    bool enableAdjLabels = false,
    bool enablePrependLabels = false) {
  openr::thrift::LinkMonitorConfig linkMonitorConfig;
  linkMonitorConfig.include_interface_regexes_ref() =
      std::vector<std::string>{".*"};
  linkMonitorConfig.redistribute_interface_regexes_ref() =
      std::vector<std::string>{"lo1"};

  openr::thrift::KvstoreConfig kvstoreConfig;

  openr::thrift::SparkConfig sparkConfig;
  sparkConfig.hello_time_s_ref() = 2;
  sparkConfig.keepalive_time_s_ref() = 1;
  sparkConfig.fastinit_hello_time_ms_ref() = 100;
  sparkConfig.hold_time_s_ref() = 2;
  sparkConfig.graceful_restart_time_s_ref() = 6;

  openr::thrift::DecisionConfig decisionConfig;
  decisionConfig.enable_bgp_route_programming_ref() = true;

  openr::thrift::OpenrConfig config;

  config.node_name_ref() = nodeName;
  config.domain_ref() = domainName;
  config.enable_v4_ref() = enableV4;
  config.v4_over_v6_nexthop_ref() = enableV4OverV6Nexthop;
  config.enable_segment_routing_ref() = enableSegmentRouting;
  config.dryrun_ref() = dryrun;
  config.ip_tos_ref() = 192;

  config.kvstore_config_ref() = kvstoreConfig;
  config.link_monitor_config_ref() = linkMonitorConfig;
  config.spark_config_ref() = sparkConfig;
  config.decision_config_ref() = decisionConfig;
  config.enable_rib_policy_ref() = true;
  config.assume_drained_ref() = false;
  config.enable_fib_ack_ref() = true;
  config.enable_kvstore_request_queue_ref() = false;
  config.prefix_hold_time_s_ref() = 0;

  if (areaCfg.empty()) {
    config.areas_ref() = {createAreaConfig(
        kTestingAreaName, {".*"}, {".*"}, std::nullopt, enableAdjLabels)};
  } else {
    config.areas_ref() = areaCfg;
  }

  openr::thrift::SegmentRoutingConfig srConfig;
  if (enablePrependLabels) {
    openr::thrift::MplsLabelRanges prepend_label_ranges;
    openr::thrift::LabelRange lr4;
    openr::thrift::LabelRange lr6;
    lr4.start_label_ref() =
        openr::MplsConstants::kSrV4StaticMplsRouteRange.first;
    lr4.end_label_ref() =
        openr::MplsConstants::kSrV4StaticMplsRouteRange.second;
    lr6.start_label_ref() =
        openr::MplsConstants::kSrV6StaticMplsRouteRange.first;
    lr6.end_label_ref() =
        openr::MplsConstants::kSrV6StaticMplsRouteRange.second;
    prepend_label_ranges.v4_ref() = lr4;
    prepend_label_ranges.v6_ref() = lr6;
    srConfig.prepend_label_ranges_ref() = prepend_label_ranges;
  }
  config.segment_routing_config_ref() = srConfig;

  return config;
}

} // namespace
