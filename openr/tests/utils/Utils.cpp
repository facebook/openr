/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
  areaConfig.area_id() = areaId;
  areaConfig.neighbor_regexes() = neighborRegexes;
  areaConfig.include_interface_regexes() = interfaceRegexes;
  if (policy) {
    areaConfig.import_policy_name() = policy.value();
  }

  if (enableAdjLabels) {
    openr::thrift::SegmentRoutingAdjLabelType sr_adj_label_type;
    openr::thrift::SegmentRoutingAdjLabel sr_adj_label;
    openr::thrift::LabelRange lr;

    lr.start_label() = openr::MplsConstants::kSrLocalRange.first;
    lr.end_label() = openr::MplsConstants::kSrLocalRange.second;
    sr_adj_label_type = openr::thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX;
    sr_adj_label.sr_adj_label_type() = sr_adj_label_type;
    sr_adj_label.adj_label_range() = lr;
    areaConfig.sr_adj_label() = sr_adj_label;
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
  config.node_name() = nodeName;
  config.enable_v4() = enableV4;
  config.v4_over_v6_nexthop() = enableV4OverV6Nexthop;
  config.enable_segment_routing() = enableSegmentRouting;
  config.dryrun() = dryrun;
  config.ip_tos() = 192;

  config.enable_rib_policy() = true;
  config.assume_drained() = false;
  config.prefix_hold_time_s() = 0;

  /*
   * [OVERRIDE] thrift::LinkMonitorConfig
   */
  openr::thrift::LinkMonitorConfig lmConf;
  lmConf.enable_perf_measurement() = false;
  lmConf.use_rtt_metric() = true;
  config.link_monitor_config() = lmConf;

  /*
   * [OVERRIDE] thrift::KvStoreConfig
   */
  openr::thrift::KvstoreConfig kvstoreConfig;
  config.kvstore_config() = kvstoreConfig;

  /*
   * [OVERRIDE] thrift::SparkConfig
   */
  openr::thrift::SparkConfig sparkConfig;
  sparkConfig.hello_time_s() = 2;
  sparkConfig.keepalive_time_s() = 1;
  sparkConfig.fastinit_hello_time_ms() = 100;
  sparkConfig.hold_time_s() = 2;
  sparkConfig.graceful_restart_time_s() = 6;
  config.spark_config() = sparkConfig;

  /*
   * [OVERRIDE] thrift::DecisionConfig
   */
  openr::thrift::DecisionConfig decisionConfig;
  decisionConfig.enable_bgp_route_programming() = true;
  config.decision_config() = decisionConfig;

  /*
   * [OVERRIDE] thrift::AreaConfig
   */
  if (areaCfg.empty()) {
    config.areas() = {createAreaConfig(
        kTestingAreaName, {".*"}, {".*"}, std::nullopt, enableAdjLabels)};
  } else {
    config.areas() = areaCfg;
  }

  /*
   * [OVERRIDE] (SR) thrift::SegmentRoutingConfig
   */
  openr::thrift::SegmentRoutingConfig srConfig;
  if (enablePrependLabels) {
    openr::thrift::MplsLabelRanges prepend_label_ranges;
    openr::thrift::LabelRange lr4;
    openr::thrift::LabelRange lr6;
    lr4.start_label() = openr::MplsConstants::kSrV4StaticMplsRouteRange.first;
    lr4.end_label() = openr::MplsConstants::kSrV4StaticMplsRouteRange.second;
    lr6.start_label() = openr::MplsConstants::kSrV6StaticMplsRouteRange.first;
    lr6.end_label() = openr::MplsConstants::kSrV6StaticMplsRouteRange.second;
    prepend_label_ranges.v4() = lr4;
    prepend_label_ranges.v6() = lr6;
    srConfig.prepend_label_ranges() = prepend_label_ranges;
  }
  config.segment_routing_config() = srConfig;

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
  path1.area() = std::to_string(areaId);
  DecisionRouteUpdate routeUpdate;

  for (auto& prefixEntry : prefixEntries) {
    prefixEntry.area_stack() = {"65000"};
    prefixEntry.metrics()->distance() = 1;
    prefixEntry.type() = thrift::PrefixType::DEFAULT;
    prefixEntry.forwardingAlgorithm() =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    prefixEntry.forwardingType() = thrift::PrefixForwardingType::SR_MPLS;
    prefixEntry.minNexthop() = 10;
    prefixEntry.prependLabel() = 70000;

    auto unicastRoute = RibUnicastEntry(
        toIPNetwork(*prefixEntry.prefix()),
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

/*
 * Util function to generate kvstore keyVal
 */
std::pair<std::string, thrift::Value>
genRandomKvStoreKeyVal(
    int64_t keyLen,
    int64_t valLen,
    int64_t version,
    const std::string& originatorId,
    int64_t ttl,
    int64_t ttlVersion,
    std::optional<int64_t> hash) {
  auto key = genRandomStr(keyLen);
  auto value = genRandomStr(valLen);
  auto thriftVal = createThriftValue(
      version /* version */,
      originatorId /* originatorId */,
      value /* value */,
      ttl /* ttl */,
      ttlVersion /* ttl version */,
      hash /* hash */);

  return std::make_pair(key, thriftVal);
}
} // namespace openr
