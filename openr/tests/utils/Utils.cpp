/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/utils/Utils.h>

namespace detail {
// Prefix length of a subnet
const uint8_t kBitMaskLen = 128;

} // namespace detail

namespace openr {

/*
 * Util function to generate random string of given length
 */
std::string
genRandomStr(const int64_t len) {
  std::string s;
  s.resize(len);

  static const std::string alphanum =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  for (int64_t i = 0; i < len; ++i) {
    s[i] = alphanum[folly::Random::rand32() % alphanum.size()];
  }
  return s;
}

/*
 * Util function to construct thrift::AreaConfig
 */
openr::thrift::AreaConfig
createAreaConfig(
    const std::string& areaId,
    const std::vector<std::string>& neighborRegexes,
    const std::vector<std::string>& interfaceRegexes,
    const std::optional<std::string>& policy,
    const bool enableAdjLabels) {
  openr::thrift::AreaConfig areaConfig;
  areaConfig.area_id_ref() = areaId;
  areaConfig.neighbor_regexes_ref() = neighborRegexes;
  areaConfig.include_interface_regexes_ref() = interfaceRegexes;
  if (policy) {
    areaConfig.import_policy_name_ref() = policy.value();
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

/*
 * Util function to genearate basic Open/R config in test environment:
 *  1) Unit Test;
 *  2) Benchmark Test;
 *  3) TBD;
 */
openr::thrift::OpenrConfig
getBasicOpenrConfig(
    const std::string& nodeName,
    const std::string& domainName,
    const std::vector<openr::thrift::AreaConfig>& areaCfg,
    bool enableV4,
    bool enableSegmentRouting,
    bool dryrun,
    bool enableV4OverV6Nexthop,
    bool enableAdjLabels,
    bool enablePrependLabels) {
  /*
   * [DEFAULT] thrift::OpenrConfig
   */
  openr::thrift::OpenrConfig config;

  /*
   * [OVERRIDE] config knob toggling
   */
  config.node_name_ref() = nodeName;
  config.domain_ref() = domainName;
  config.enable_v4_ref() = enableV4;
  config.v4_over_v6_nexthop_ref() = enableV4OverV6Nexthop;
  config.enable_segment_routing_ref() = enableSegmentRouting;
  config.dryrun_ref() = dryrun;
  config.ip_tos_ref() = 192;

  config.enable_rib_policy_ref() = true;
  config.assume_drained_ref() = false;
  config.enable_fib_ack_ref() = true;
  config.enable_kvstore_request_queue_ref() = true;
  config.prefix_hold_time_s_ref() = 0;
  config.enable_new_gr_behavior_ref() = true;
  config.enable_new_prefix_format_ref() = true;

  /*
   * [OVERRIDE] thrift::LinkMonitorConfig
   */
  openr::thrift::LinkMonitorConfig linkMonitorConfig;
  linkMonitorConfig.include_interface_regexes_ref() =
      std::vector<std::string>{".*"};
  linkMonitorConfig.redistribute_interface_regexes_ref() =
      std::vector<std::string>{"lo1"};
  config.link_monitor_config_ref() = linkMonitorConfig;

  /*
   * [OVERRIDE] thrift::KvStoreConfig
   */
  openr::thrift::KvstoreConfig kvstoreConfig;
  config.kvstore_config_ref() = kvstoreConfig;

  /*
   * [OVERRIDE] thrift::SparkConfig
   */
  openr::thrift::SparkConfig sparkConfig;
  sparkConfig.hello_time_s_ref() = 2;
  sparkConfig.keepalive_time_s_ref() = 1;
  sparkConfig.fastinit_hello_time_ms_ref() = 100;
  sparkConfig.hold_time_s_ref() = 2;
  sparkConfig.graceful_restart_time_s_ref() = 6;
  config.spark_config_ref() = sparkConfig;

  /*
   * [OVERRIDE] thrift::DecisionConfig
   */
  openr::thrift::DecisionConfig decisionConfig;
  decisionConfig.enable_bgp_route_programming_ref() = true;
  config.decision_config_ref() = decisionConfig;

  /*
   * [OVERRIDE] thrift::AreaConfig
   */
  if (areaCfg.empty()) {
    config.areas_ref() = {createAreaConfig(
        kTestingAreaName, {".*"}, {".*"}, std::nullopt, enableAdjLabels)};
  } else {
    config.areas_ref() = areaCfg;
  }

  /*
   * [OVERRIDE] (SR) thrift::SegmentRoutingConfig
   */
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

std::vector<thrift::PrefixEntry>
generatePrefixEntries(const PrefixGenerator& prefixGenerator, uint32_t num) {
  // generate `num` of random prefixes
  std::vector<thrift::IpPrefix> prefixes =
      prefixGenerator.ipv6PrefixGenerator(num, ::detail::kBitMaskLen);
  auto tPrefixEntries =
      folly::gen::from(prefixes) |
      folly::gen::mapped([](const thrift::IpPrefix& prefix) {
        return createPrefixEntry(prefix, thrift::PrefixType::DEFAULT);
      }) |
      folly::gen::as<std::vector<thrift::PrefixEntry>>();
  return tPrefixEntries;
}

DecisionRouteUpdate
generateDecisionRouteUpdateFromPrefixEntries(
    std::vector<thrift::PrefixEntry> prefixEntries, uint32_t areaId) {
  // Borrow the settings for prefixEntries from PrefixManagerTest
  auto path1 =
      createNextHop(toBinaryAddress(folly::IPAddress("fe80::2")), "iface", 1);
  path1.area_ref() = std::to_string(areaId);
  DecisionRouteUpdate routeUpdate;

  for (auto& prefixEntry : prefixEntries) {
    prefixEntry.area_stack_ref() = {"65000"};
    prefixEntry.metrics_ref()->distance_ref() = 1;
    prefixEntry.type_ref() = thrift::PrefixType::DEFAULT;
    prefixEntry.forwardingAlgorithm_ref() =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    prefixEntry.forwardingType_ref() = thrift::PrefixForwardingType::SR_MPLS;
    prefixEntry.minNexthop_ref() = 10;
    prefixEntry.prependLabel_ref() = 70000;

    auto unicastRoute = RibUnicastEntry(
        toIPNetwork(prefixEntry.get_prefix()),
        {path1},
        prefixEntry,
        std::to_string(areaId),
        false);
    routeUpdate.addRouteToUpdate(unicastRoute);
  }
  return routeUpdate;
}

DecisionRouteUpdate
generateDecisionRouteUpdate(
    const PrefixGenerator& prefixGenerator, uint32_t num, uint32_t areaId) {
  std::vector<thrift::PrefixEntry> prefixEntries =
      generatePrefixEntries(prefixGenerator, num);
  return generateDecisionRouteUpdateFromPrefixEntries(prefixEntries, areaId);
}
} // namespace openr
