/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/FileUtil.h>
#include <glog/logging.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <stdexcept>

#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

using apache::thrift::util::enumName;
using openr::thrift::PrefixAllocationMode;
using openr::thrift::PrefixForwardingAlgorithm;
using openr::thrift::PrefixForwardingType;

namespace openr {

std::shared_ptr<re2::RE2::Set>
AreaConfiguration::compileRegexSet(std::vector<std::string> const& strings) {
  re2::RE2::Options regexOpts;
  std::string regexErr;
  regexOpts.set_case_sensitive(false);

  auto reSet =
      std::make_shared<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);

  if (strings.empty()) {
    // make this regex set unmatchable
    std::string const unmatchable = "a^";
    CHECK_NE(-1, reSet->Add(unmatchable, &regexErr)) << fmt::format(
        "Failed to add regex: {}. Error: {}", unmatchable, regexErr);
  }
  for (const auto& str : strings) {
    if (reSet->Add(str, &regexErr) == -1) {
      throw std::invalid_argument(
          fmt::format("Failed to add regex: {}. Error: {}", str, regexErr));
    }
  }
  CHECK(reSet->Compile()) << "Regex compilation failed";
  return reSet;
}

Config::Config(const std::string& configFile) {
  std::string contents;
  if (not folly::readFile(configFile.c_str(), contents)) {
    auto errStr = fmt::format("Could not read config file: {}", configFile);
    LOG(ERROR) << errStr;
    throw thrift::ConfigError(errStr);
  }

  auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
  try {
    jsonSerializer.deserialize(contents, config_);
  } catch (const std::exception& ex) {
    auto errStr = fmt::format(
        "Could not parse OpenrConfig struct: {}", folly::exceptionStr(ex));
    LOG(ERROR) << errStr;
    throw thrift::ConfigError(errStr);
  }
  populateInternalDb();
}

std::string
Config::getRunningConfig() const {
  auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
  std::string contents;
  try {
    jsonSerializer.serialize(config_, &contents);
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Could not serialize config: " << folly::exceptionStr(ex);
  }

  return contents;
}

PrefixAllocationParams
Config::createPrefixAllocationParams(
    const std::string& seedPfxStr, uint8_t allocationPfxLen) {
  // check seed_prefix and allocate_prefix_len are set
  if (seedPfxStr.empty() or allocationPfxLen == 0) {
    throw std::invalid_argument(
        "seed_prefix and allocate_prefix_len must be filled.");
  }

  // validate seed prefix
  auto seedPfx = folly::IPAddress::createNetwork(seedPfxStr);

  // validate allocate_prefix_len
  if (seedPfx.first.isV4() and
      (allocationPfxLen <= seedPfx.second or allocationPfxLen > 32)) {
    throw std::out_of_range(fmt::format(
        "invalid allocate_prefix_len ({}), valid range = ({}, 32]",
        allocationPfxLen,
        seedPfx.second));
  }

  if ((seedPfx.first.isV6()) and
      (allocationPfxLen <= seedPfx.second or allocationPfxLen > 128)) {
    throw std::out_of_range(fmt::format(
        "invalid allocate_prefix_len ({}), valid range = ({}, 128]",
        allocationPfxLen,
        seedPfx.second));
  }

  return {seedPfx, allocationPfxLen};
}

void
Config::populateAreaConfig() {
  if (config_.get_areas().empty()) {
    // TODO remove once transition to areas is complete
    thrift::AreaConfig defaultArea;
    defaultArea.area_id_ref() = thrift::Types_constants::kDefaultArea();
    config_.areas_ref() = {defaultArea};
  }

  std::optional<neteng::config::routing_policy::Filters> propagationPolicy{
      std::nullopt};
  if (auto areaPolicies = getAreaPolicies()) {
    propagationPolicy =
        areaPolicies->filters_ref()->routePropagationPolicy_ref().to_optional();
  }

  if (getDomainName().empty()) {
    throw std::invalid_argument("domain name must be non-empty");
  }

  for (auto& areaConf : *config_.areas_ref()) {
    // Fill these values from linkMonitor config if not provided
    // TODO remove once transition to areas is complete
    auto const& lmConf = config_.get_link_monitor_config();
    if (areaConf.get_redistribute_interface_regexes().empty()) {
      areaConf.redistribute_interface_regexes_ref() =
          lmConf.get_redistribute_interface_regexes();
    }
    if (areaConf.get_include_interface_regexes().empty()) {
      areaConf.include_interface_regexes_ref() =
          lmConf.get_include_interface_regexes();
    }
    if (areaConf.get_exclude_interface_regexes().empty()) {
      areaConf.exclude_interface_regexes_ref() =
          lmConf.get_exclude_interface_regexes();
    }
    if (areaConf.get_neighbor_regexes().empty()) {
      areaConf.neighbor_regexes_ref() = {".*"};
    }

    if (auto importPolicyName = areaConf.import_policy_name_ref()) {
      if (not propagationPolicy or
          propagationPolicy->objects_ref()->count(*importPolicyName) == 0) {
        throw std::invalid_argument(fmt::format(
            "No area policy definition found for {}", *importPolicyName));
      }
    }

    if (!areaConfigs_.emplace(areaConf.get_area_id(), areaConf).second) {
      throw std::invalid_argument(
          fmt::format("Duplicate area config id: {}", areaConf.get_area_id()));
    }

    // Check if Node Segment Label is configured or if label range is valid
    if (areaConf.area_sr_node_label_ref().has_value()) {
      const auto& srNodeConfig = *areaConf.area_sr_node_label_ref();
      if (*srNodeConfig.sr_node_label_type_ref() ==
          thrift::SegmentRoutingNodeLabelType::AUTO) {
        // Automatic node segment label allocation
        if (not srNodeConfig.node_segment_label_range_ref().has_value()) {
          throw std::invalid_argument(fmt::format(
              "node segment label range is not configured for area config id: {}",
              areaConf.get_area_id()));
        } else if (not isLabelRangeValid(
                       *srNodeConfig.node_segment_label_range_ref())) {
          const auto& label_range =
              *srNodeConfig.node_segment_label_range_ref();
          throw std::invalid_argument(fmt::format(
              "node segment label range [{}, {}] is invalid for area config id: {}",
              *label_range.start_label_ref(),
              *label_range.end_label_ref(),
              areaConf.get_area_id()));
        }
      } else if (not srNodeConfig.node_segment_label_ref().has_value()) {
        throw std::invalid_argument(fmt::format(
            "static node segment label is not configured for area config id: {}",
            areaConf.get_area_id()));
      } else if (not isMplsLabelValid(*srNodeConfig.node_segment_label_ref())) {
        throw std::invalid_argument(fmt::format(
            "node segment label {} is invalid for area config id: {}",
            *srNodeConfig.node_segment_label_ref(),
            areaConf.get_area_id()));
      }
    }
  }
}

void
Config::populateInternalDb() {
  populateAreaConfig();

  // prefix forwarding type and algorithm
  const auto& pfxType = *config_.prefix_forwarding_type_ref();
  const auto& pfxAlgo = *config_.prefix_forwarding_algorithm_ref();

  if (not enumName(pfxType) or not enumName(pfxAlgo)) {
    throw std::invalid_argument(
        "invalid prefix_forwarding_type or prefix_forwarding_algorithm");
  }

  if (pfxAlgo == PrefixForwardingAlgorithm::KSP2_ED_ECMP and
      pfxType != PrefixForwardingType::SR_MPLS) {
    throw std::invalid_argument(
        "prefix_forwarding_type must be set to SR_MPLS for KSP2_ED_ECMP");
  }

  // IP-TOS checking
  if (const auto& ipTos = config_.ip_tos_ref()) {
    if (*ipTos < 0 or *ipTos >= 256) {
      throw std::out_of_range(
          "ip_tos must be greater or equal to 0 and less than 256");
    }
  }

  //
  // Kvstore
  //
  const auto& kvConf = *config_.kvstore_config_ref();
  if (const auto& floodRate = kvConf.flood_rate_ref()) {
    if (*floodRate->flood_msg_per_sec_ref() <= 0) {
      throw std::out_of_range("kvstore flood_msg_per_sec should be > 0");
    }
    if (*floodRate->flood_msg_burst_size_ref() <= 0) {
      throw std::out_of_range("kvstore flood_msg_burst_size should be > 0");
    }
  }

  //
  // Decision
  //
  const auto& decisionConfig = *config_.decision_config_ref();
  if (*decisionConfig.debounce_min_ms_ref() >
      *decisionConfig.debounce_max_ms_ref()) {
    throw std::invalid_argument(fmt::format(
        "decision_config.debounce_min_ms ({}) should be <= decision_config.debounce_max_ms ({})",
        *decisionConfig.debounce_min_ms_ref(),
        *decisionConfig.debounce_max_ms_ref()));
  }

  //
  // Spark
  //
  const auto& sparkConfig = *config_.spark_config_ref();
  if (*sparkConfig.neighbor_discovery_port_ref() <= 0 ||
      *sparkConfig.neighbor_discovery_port_ref() > 65535) {
    throw std::out_of_range(fmt::format(
        "neighbor_discovery_port ({}) should be in range [0, 65535]",
        *sparkConfig.neighbor_discovery_port_ref()));
  }

  if (*sparkConfig.hello_time_s_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "hello_time_s ({}) should be > 0", *sparkConfig.hello_time_s_ref()));
  }

  // When a node starts or a new link comes up we perform fast initial neighbor
  // discovery by sending hello packets with solicitResponse bit set to request
  // an immediate reply. This allows us to discover new neighbors in hundreds
  // of milliseconds (or as configured).
  if (*sparkConfig.fastinit_hello_time_ms_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "fastinit_hello_time_ms ({}) should be > 0",
        *sparkConfig.fastinit_hello_time_ms_ref()));
  }

  if (*sparkConfig.fastinit_hello_time_ms_ref() >
      1000 * *sparkConfig.hello_time_s_ref()) {
    throw std::invalid_argument(fmt::format(
        "fastinit_hello_time_ms ({}) should be <= hold_time_s ({}) * 1000",
        *sparkConfig.fastinit_hello_time_ms_ref(),
        *sparkConfig.hello_time_s_ref()));
  }

  // The rate of hello packet send is defined by keepAliveTime.
  // This time must be less than the holdTime for each node.
  if (*sparkConfig.keepalive_time_s_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "keepalive_time_s ({}) should be > 0",
        *sparkConfig.keepalive_time_s_ref()));
  }

  if (*sparkConfig.keepalive_time_s_ref() > *sparkConfig.hold_time_s_ref()) {
    throw std::invalid_argument(fmt::format(
        "keepalive_time_s ({}) should be <= hold_time_s ({})",
        *sparkConfig.keepalive_time_s_ref(),
        *sparkConfig.hold_time_s_ref()));
  }

  // Hold time tells the receiver how long to keep the information valid for.
  if (*sparkConfig.hold_time_s_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "hold_time_s ({}) should be > 0", *sparkConfig.hold_time_s_ref()));
  }

  if (*sparkConfig.graceful_restart_time_s_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "graceful_restart_time_s ({}) should be > 0",
        *sparkConfig.graceful_restart_time_s_ref()));
  }

  if (*sparkConfig.graceful_restart_time_s_ref() <
      3 * *sparkConfig.keepalive_time_s_ref()) {
    throw std::invalid_argument(fmt::format(
        "graceful_restart_time_s ({}) should be >= 3 * keepalive_time_s ({})",
        *sparkConfig.graceful_restart_time_s_ref(),
        *sparkConfig.keepalive_time_s_ref()));
  }

  if (*sparkConfig.step_detector_conf_ref()->lower_threshold_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->upper_threshold_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->lower_threshold_ref() >=
          *sparkConfig.step_detector_conf_ref()->upper_threshold_ref()) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.lower_threshold ({}) should be < step_detector_conf.upper_threshold ({}), and they should be >= 0",
        *sparkConfig.step_detector_conf_ref()->lower_threshold_ref(),
        *sparkConfig.step_detector_conf_ref()->upper_threshold_ref()));
  }

  if (*sparkConfig.step_detector_conf_ref()->fast_window_size_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->slow_window_size_ref() < 0 ||
      (*sparkConfig.step_detector_conf_ref()->fast_window_size_ref() >
       *sparkConfig.step_detector_conf_ref()->slow_window_size_ref())) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.fast_window_size ({}) should be <= step_detector_conf.slow_window_size ({}), and they should be >= 0",
        *sparkConfig.step_detector_conf_ref()->fast_window_size_ref(),
        *sparkConfig.step_detector_conf_ref()->slow_window_size_ref()));
  }

  if (*sparkConfig.step_detector_conf_ref()->lower_threshold_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->upper_threshold_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->lower_threshold_ref() >=
          *sparkConfig.step_detector_conf_ref()->upper_threshold_ref()) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.lower_threshold ({}) should be < step_detector_conf.upper_threshold ({})",
        *sparkConfig.step_detector_conf_ref()->lower_threshold_ref(),
        *sparkConfig.step_detector_conf_ref()->upper_threshold_ref()));
  }

  //
  // Monitor
  //
  const auto& monitorConfig = *config_.monitor_config_ref();
  if (*monitorConfig.max_event_log_ref() < 0) {
    throw std::out_of_range(fmt::format(
        "monitor_max_event_log ({}) should be >= 0",
        *monitorConfig.max_event_log_ref()));
  }

  //
  // Link Monitor
  //
  const auto& lmConf = *config_.link_monitor_config_ref();

  // backoff validation
  if (*lmConf.linkflap_initial_backoff_ms_ref() < 0) {
    throw std::out_of_range(fmt::format(
        "linkflap_initial_backoff_ms ({}) should be >= 0",
        *lmConf.linkflap_initial_backoff_ms_ref()));
  }

  if (*lmConf.linkflap_max_backoff_ms_ref() < 0) {
    throw std::out_of_range(fmt::format(
        "linkflap_max_backoff_ms ({}) should be >= 0",
        *lmConf.linkflap_max_backoff_ms_ref()));
  }

  if (*lmConf.linkflap_initial_backoff_ms_ref() >
      *lmConf.linkflap_max_backoff_ms_ref()) {
    throw std::out_of_range(fmt::format(
        "linkflap_initial_backoff_ms ({}) should be < linkflap_max_backoff_ms ({})",
        *lmConf.linkflap_initial_backoff_ms_ref(),
        *lmConf.linkflap_max_backoff_ms_ref()));
  }

  //
  // Segment Routing Config
  //
  if (config_.segment_routing_config_ref().has_value()) {
    const auto& srConfig = *config_.segment_routing_config_ref();
    // Check label range values for prepend labels
    if (srConfig.prepend_label_ranges_ref().has_value()) {
      const auto& v4LblRange = *srConfig.prepend_label_ranges_ref()->v4_ref();
      if (not isLabelRangeValid(v4LblRange)) {
        throw std::invalid_argument(fmt::format(
            "v4: prepend label range [{}, {}] is invalid",
            *v4LblRange.start_label_ref(),
            *v4LblRange.end_label_ref()));
      }

      const auto& v6LblRange = *srConfig.prepend_label_ranges_ref()->v6_ref();
      if (not isLabelRangeValid(v6LblRange)) {
        throw std::invalid_argument(fmt::format(
            "v6: prepend label range [{}, {}] is invalid",
            *v6LblRange.start_label_ref(),
            *v6LblRange.end_label_ref()));
      }
    }

    if (srConfig.sr_adj_label_ref().has_value()) {
      // Check adj segment labels if configured or if label range is valid
      if (srConfig.sr_adj_label_ref()->sr_adj_label_type_ref() ==
          thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX) {
        if (not srConfig.sr_adj_label_ref()
                    ->adj_label_range_ref()
                    .has_value()) {
          throw std::invalid_argument(fmt::format(
              "label range for adjacency labels is not configured"));
        } else if (not isLabelRangeValid(
                       *srConfig.sr_adj_label_ref()->adj_label_range_ref())) {
          const auto& label_range =
              *srConfig.sr_adj_label_ref()->adj_label_range_ref();
          throw std::invalid_argument(fmt::format(
              "label range [{}, {}] for adjacency labels is invalid",
              *label_range.start_label_ref(),
              *label_range.end_label_ref()));
        }
      }
    }
  }

  //
  // Prefix Allocation
  //
  if (isPrefixAllocationEnabled()) {
    // by now areaConfigs_ should be filled.
    if (areaConfigs_.size() > 1) {
      throw std::invalid_argument(
          "prefix_allocation only support single area config");
    }
    const auto& paConf = config_.prefix_allocation_config_ref();
    // check if config exists
    if (not paConf) {
      throw std::invalid_argument(
          "enable_prefix_allocation = true, but prefix_allocation_config is empty");
    }

    // sanity check enum prefix_allocation_mode
    if (not enumName(*paConf->prefix_allocation_mode_ref())) {
      throw std::invalid_argument("invalid prefix_allocation_mode");
    }

    auto seedPrefix = paConf->seed_prefix_ref().value_or("");
    auto allocatePfxLen = paConf->allocate_prefix_len_ref().value_or(0);

    switch (*paConf->prefix_allocation_mode_ref()) {
    case PrefixAllocationMode::DYNAMIC_ROOT_NODE: {
      // populate prefixAllocationParams_ from seed_prefix and
      // allocate_prefix_len
      prefixAllocationParams_ =
          createPrefixAllocationParams(seedPrefix, allocatePfxLen);

      if (prefixAllocationParams_->first.first.isV4() and not isV4Enabled()) {
        throw std::invalid_argument(
            "v4 seed_prefix detected, but enable_v4 = false");
      }
      break;
    }
    case PrefixAllocationMode::DYNAMIC_LEAF_NODE:
    case PrefixAllocationMode::STATIC: {
      // seed_prefix and allocate_prefix_len have to to empty
      if (not seedPrefix.empty() or allocatePfxLen > 0) {
        throw std::invalid_argument(
            "prefix_allocation_mode != DYNAMIC_ROOT_NODE, seed_prefix and allocate_prefix_len must be empty");
      }
      break;
    }
    }
  } // if enable_prefix_allocation_ref()

  //
  // bgp peering
  //
  if (isBgpPeeringEnabled() and not config_.bgp_config_ref()) {
    throw std::invalid_argument(
        "enable_bgp_peering = true, but bgp_config is empty");
  }
  if (isBgpPeeringEnabled() and not config_.bgp_translation_config_ref()) {
    // Hack for transioning phase. TODO: Remove after coop is on-boarded
    config_.bgp_translation_config_ref() = thrift::BgpRouteTranslationConfig();
    // throw std::invalid_argument(
    //     "enable_bgp_peering = true, but bgp_translation_config is empty");
  }

  if (isBgpPeeringEnabled() and config_.bgp_config_ref()) {
    // Identify if BGP Add Path is enabled for local peering or not
    for (const auto& peer : *config_.bgp_config_ref()->peers_ref()) {
      if (folly::IPAddress(peer.peer_addr_ref().value()).isLoopback() and
          peer.add_path_ref() and
          *peer.add_path_ref() == thrift::AddPath::RECEIVE and
          not isSegmentRoutingEnabled()) {
        // TODO
        // Additionally check later if prepend label range is also convered.
        throw std::invalid_argument(
            "segment routing should be congfigured when BGP add_path is configured");
      }
    }
  }

  //
  // BGP Translation Config
  //
  if (isBgpPeeringEnabled()) {
    const auto& bgpTranslationConf = config_.bgp_translation_config_ref();
    CHECK(bgpTranslationConf.has_value());
    if (*bgpTranslationConf->disable_legacy_translation_ref() and
        (not *bgpTranslationConf->enable_openr_to_bgp_ref() or
         not *bgpTranslationConf->enable_bgp_to_openr_ref())) {
      throw std::invalid_argument(
          "Legacy translation can be disabled only when new translation is "
          "enabled");
    }
  }

  // To avoid bgp and vip service advertise the same prefixes,
  // bgp speaker and vip service shouldn't co-exist
  if (isBgpPeeringEnabled() && isVipServiceEnabled()) {
    throw std::invalid_argument(
        "Bgp Peering and Vip Service can not be both enabled");
  }

  //
  // watchdog
  //
  if (isWatchdogEnabled() and not config_.watchdog_config_ref()) {
    throw std::invalid_argument(
        "enable_watchdog = true, but watchdog_config is empty");
  }

} // namespace openr
} // namespace openr
