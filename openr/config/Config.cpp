/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <stdexcept>

#include <openr/common/Constants.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <re2/re2.h>

using apache::thrift::util::enumName;
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
  if (not FileUtil::readFileToString(configFile, contents)) {
    auto errStr = fmt::format("Could not read config file: {}", configFile);
    XLOG(ERR) << errStr;
    throw thrift::ConfigError(errStr);
  }

  auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
  try {
    jsonSerializer.deserialize(contents, config_);
  } catch (const std::exception& ex) {
    auto errStr = fmt::format(
        "Could not parse OpenrConfig struct: {}", folly::exceptionStr(ex));
    XLOG(ERR) << errStr;
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
    XLOG(ERR) << "Could not serialize config: " << folly::exceptionStr(ex);
  }

  return contents;
}

void
Config::populateAreaConfig() {
  if (config_.areas()->empty()) {
    // TODO remove once transition to areas is complete
    thrift::AreaConfig defaultArea;
    defaultArea.area_id() = Constants::kDefaultArea.toString();
    config_.areas() = {defaultArea};
  }

  std::optional<neteng::config::routing_policy::Filters> propagationPolicy{
      std::nullopt};
  if (auto areaPolicies = getAreaPolicies()) {
    propagationPolicy =
        areaPolicies->filters()->routePropagationPolicy().to_optional();
  }

  for (auto& areaConf : *config_.areas()) {
    if (areaConf.neighbor_regexes()->empty()) {
      areaConf.neighbor_regexes() = {".*"};
    }

    if (auto importPolicyName = areaConf.import_policy_name()) {
      if (not propagationPolicy or
          propagationPolicy->objects()->count(*importPolicyName) == 0) {
        throw std::invalid_argument(fmt::format(
            "No area policy definition found for {}", *importPolicyName));
      }
    }

    if (!areaConfigs_.emplace(*areaConf.area_id(), areaConf).second) {
      throw std::invalid_argument(
          fmt::format("Duplicate area config id: {}", *areaConf.area_id()));
    }
  }
}

void
Config::checkKvStoreConfig() const {
  auto& kvStoreConf = *config_.kvstore_config();
  if (const auto& floodRate = kvStoreConf.flood_rate()) {
    if (*floodRate->flood_msg_per_sec() <= 0) {
      throw std::out_of_range("kvstore flood_msg_per_sec should be > 0");
    }
    if (*floodRate->flood_msg_burst_size() <= 0) {
      throw std::out_of_range("kvstore flood_msg_burst_size should be > 0");
    }
  }

  if (kvStoreConf.key_ttl_ms() == Constants::kTtlInfinity) {
    throw std::out_of_range("kvstore key_ttl_ms should be a finite number");
  }
}

void
Config::checkDecisionConfig() const {
  auto& decisionConf = *config_.decision_config();
  if (*decisionConf.debounce_min_ms() > *decisionConf.debounce_max_ms()) {
    throw std::invalid_argument(fmt::format(
        "decision_config.debounce_min_ms ({}) should be <= decision_config.debounce_max_ms ({})",
        *decisionConf.debounce_min_ms(),
        *decisionConf.debounce_max_ms()));
  }
}

void
Config::checkSparkConfig() const {
  auto& sparkConfig = *config_.spark_config();
  if (*sparkConfig.neighbor_discovery_port() <= 0 ||
      *sparkConfig.neighbor_discovery_port() > 65535) {
    throw std::out_of_range(fmt::format(
        "neighbor_discovery_port ({}) should be in range [0, 65535]",
        *sparkConfig.neighbor_discovery_port()));
  }

  if (*sparkConfig.hello_time_s() <= 0) {
    throw std::out_of_range(fmt::format(
        "hello_time_s ({}) should be > 0", *sparkConfig.hello_time_s()));
  }

  // When a node starts or a new link comes up we perform fast initial neighbor
  // discovery by sending hello packets with solicitResponse bit set to request
  // an immediate reply. This allows us to discover new neighbors in hundreds
  // of milliseconds (or as configured).
  if (*sparkConfig.fastinit_hello_time_ms() <= 0) {
    throw std::out_of_range(fmt::format(
        "fastinit_hello_time_ms ({}) should be > 0",
        *sparkConfig.fastinit_hello_time_ms()));
  }

  if (*sparkConfig.fastinit_hello_time_ms() >
      1000 * *sparkConfig.hello_time_s()) {
    throw std::invalid_argument(fmt::format(
        "fastinit_hello_time_ms ({}) should be <= hold_time_s ({}) * 1000",
        *sparkConfig.fastinit_hello_time_ms(),
        *sparkConfig.hello_time_s()));
  }

  // The rate of hello packet send is defined by keepAliveTime.
  // This time must be less than the holdTime for each node.
  if (*sparkConfig.keepalive_time_s() <= 0) {
    throw std::out_of_range(fmt::format(
        "keepalive_time_s ({}) should be > 0",
        *sparkConfig.keepalive_time_s()));
  }

  if (*sparkConfig.keepalive_time_s() > *sparkConfig.hold_time_s()) {
    throw std::invalid_argument(fmt::format(
        "keepalive_time_s ({}) should be <= hold_time_s ({})",
        *sparkConfig.keepalive_time_s(),
        *sparkConfig.hold_time_s()));
  }

  // Hold time tells the receiver how long to keep the information valid for.
  if (*sparkConfig.hold_time_s() <= 0) {
    throw std::out_of_range(fmt::format(
        "hold_time_s ({}) should be > 0", *sparkConfig.hold_time_s()));
  }

  if (*sparkConfig.graceful_restart_time_s() <= 0) {
    throw std::out_of_range(fmt::format(
        "graceful_restart_time_s ({}) should be > 0",
        *sparkConfig.graceful_restart_time_s()));
  }

  if (*sparkConfig.graceful_restart_time_s() <
      3 * *sparkConfig.keepalive_time_s()) {
    throw std::invalid_argument(fmt::format(
        "graceful_restart_time_s ({}) should be >= 3 * keepalive_time_s ({})",
        *sparkConfig.graceful_restart_time_s(),
        *sparkConfig.keepalive_time_s()));
  }

  if (*sparkConfig.step_detector_conf()->lower_threshold() < 0 ||
      *sparkConfig.step_detector_conf()->upper_threshold() < 0 ||
      *sparkConfig.step_detector_conf()->lower_threshold() >=
          *sparkConfig.step_detector_conf()->upper_threshold()) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.lower_threshold ({}) should be < step_detector_conf.upper_threshold ({}), and they should be >= 0",
        *sparkConfig.step_detector_conf()->lower_threshold(),
        *sparkConfig.step_detector_conf()->upper_threshold()));
  }

  if (*sparkConfig.step_detector_conf()->fast_window_size() < 0 ||
      *sparkConfig.step_detector_conf()->slow_window_size() < 0 ||
      (*sparkConfig.step_detector_conf()->fast_window_size() >
       *sparkConfig.step_detector_conf()->slow_window_size())) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.fast_window_size ({}) should be <= step_detector_conf.slow_window_size ({}), and they should be >= 0",
        *sparkConfig.step_detector_conf()->fast_window_size(),
        *sparkConfig.step_detector_conf()->slow_window_size()));
  }

  if (*sparkConfig.step_detector_conf()->lower_threshold() < 0 ||
      *sparkConfig.step_detector_conf()->upper_threshold() < 0 ||
      *sparkConfig.step_detector_conf()->lower_threshold() >=
          *sparkConfig.step_detector_conf()->upper_threshold()) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.lower_threshold ({}) should be < step_detector_conf.upper_threshold ({})",
        *sparkConfig.step_detector_conf()->lower_threshold(),
        *sparkConfig.step_detector_conf()->upper_threshold()));
  }
}

void
Config::checkMonitorConfig() const {
  auto& monitorConfig = *config_.monitor_config();
  if (*monitorConfig.max_event_log() < 0) {
    throw std::out_of_range(fmt::format(
        "monitor_max_event_log ({}) should be >= 0",
        *monitorConfig.max_event_log()));
  }
}

void
Config::checkLinkMonitorConfig() const {
  auto& lmConf = *config_.link_monitor_config();
  // backoff validation
  if (*lmConf.linkflap_initial_backoff_ms() < 0) {
    throw std::out_of_range(fmt::format(
        "linkflap_initial_backoff_ms ({}) should be >= 0",
        *lmConf.linkflap_initial_backoff_ms()));
  }

  if (*lmConf.linkflap_max_backoff_ms() < 0) {
    throw std::out_of_range(fmt::format(
        "linkflap_max_backoff_ms ({}) should be >= 0",
        *lmConf.linkflap_max_backoff_ms()));
  }

  if (*lmConf.linkflap_initial_backoff_ms() >
      *lmConf.linkflap_max_backoff_ms()) {
    throw std::out_of_range(fmt::format(
        "linkflap_initial_backoff_ms ({}) should be < linkflap_max_backoff_ms ({})",
        *lmConf.linkflap_initial_backoff_ms(),
        *lmConf.linkflap_max_backoff_ms()));
  }
}

void
Config::checkVipServiceConfig() const {
  if (isVipServiceEnabled()) {
    if (not config_.vip_service_config()) {
      throw std::invalid_argument(
          "enable_vip_service = true, but vip_service_config is empty");
    } else {
      if (config_.vip_service_config()->ingress_policy().has_value()) {
        std::optional<neteng::config::routing_policy::Filters>
            propagationPolicy{std::nullopt};
        if (auto areaPolicies = getAreaPolicies()) {
          propagationPolicy =
              areaPolicies->filters()->routePropagationPolicy().to_optional();
        }
        auto ingress_policy = *config_.vip_service_config()->ingress_policy();
        if (not propagationPolicy or
            propagationPolicy->objects()->count(ingress_policy) == 0) {
          throw std::invalid_argument(fmt::format(
              "No area policy definition found for {}", ingress_policy));
        }
      }
    }
  }
}

void
Config::checkBgpPeeringConfig() {
  if (isBgpPeeringEnabled() and not config_.bgp_config()) {
    throw std::invalid_argument(
        "enable_bgp_peering = true, but bgp_config is empty");
  }

  // Set BGP Translation Config if unset
  if (isBgpPeeringEnabled() and not config_.bgp_translation_config()) {
    // Hack for transioning phase. TODO: Remove after coop is on-boarded
    config_.bgp_translation_config() = thrift::BgpRouteTranslationConfig();
    // throw std::invalid_argument(
    //     "enable_bgp_peering = true, but bgp_translation_config is empty");
  }

  // Validate BGP Translation config
  if (isBgpPeeringEnabled()) {
    const auto& bgpTranslationConf = config_.bgp_translation_config();
    CHECK(bgpTranslationConf.has_value());
    if (*bgpTranslationConf->disable_legacy_translation() and
        (not *bgpTranslationConf->enable_openr_to_bgp() or
         not *bgpTranslationConf->enable_bgp_to_openr())) {
      throw std::invalid_argument(
          "Legacy translation can be disabled only when new translation is "
          "enabled");
    }
  }
}

void
Config::checkThriftServerConfig() const {
  const auto& thriftServerConfig = getThriftServerConfig();

  // Checking the fields needed when we enable the secure thrift server
  const auto& caPath = thriftServerConfig.x509_ca_path();
  const auto& certPath = thriftServerConfig.x509_cert_path();
  const auto& eccCurve = thriftServerConfig.ecc_curve_name();
  if (not(caPath and certPath and eccCurve)) {
    throw std::invalid_argument(
        "enable_secure_thrift_server = true, but x509_ca_path, x509_cert_path or ecc_curve_name is empty.");
  }
  if ((not fs::exists(caPath.value())) or (not fs::exists(certPath.value()))) {
    throw std::invalid_argument(
        "x509_ca_path or x509_cert_path is specified in the config but not found in the disk.");
  }

  // x509_key_path could be empty. If specified, need to be present in the
  // file system.
  const auto& keyPath = getThriftServerConfig().x509_key_path();
  if (keyPath and (not fs::exists(keyPath.value()))) {
    throw std::invalid_argument(
        "x509_key_path is specified in the config but not found in the disk.");
  }
}

void
Config::populateInternalDb() {
  populateAreaConfig();

  // validate prefix forwarding type and algorithm
  const auto& pfxType = *config_.prefix_forwarding_type();
  const auto& pfxAlgo = *config_.prefix_forwarding_algorithm();

  if (not enumName(pfxType) or not enumName(pfxAlgo)) {
    throw std::invalid_argument(
        "invalid prefix_forwarding_type or prefix_forwarding_algorithm");
  }

  if (pfxAlgo == PrefixForwardingAlgorithm::KSP2_ED_ECMP and
      pfxType != PrefixForwardingType::SR_MPLS) {
    throw std::invalid_argument(
        "prefix_forwarding_type must be set to SR_MPLS for KSP2_ED_ECMP");
  }

  // validate IP-TOS
  if (const auto& ipTos = config_.ip_tos()) {
    if (*ipTos < 0 or *ipTos >= 256) {
      throw std::out_of_range(
          "ip_tos must be greater or equal to 0 and less than 256");
    }
  }

  // To avoid bgp and vip service advertise the same prefixes,
  // bgp speaker and vip service shouldn't co-exist
  if (isBgpPeeringEnabled() && isVipServiceEnabled()) {
    throw std::invalid_argument(
        "Bgp Peering and Vip Service can not be both enabled");
  }

  // check watchdog has config if enabled
  if (isWatchdogEnabled() and not config_.watchdog_config()) {
    throw std::invalid_argument(
        "enable_watchdog = true, but watchdog_config is empty");
  }

  // Check Route Deletion Parameter
  if (*config_.route_delete_delay_ms() < 0) {
    throw std::invalid_argument("Route delete duration must be >= 0ms");
  }

  // validate KvStore config (e.g. ttl/flood-rate/etc.)
  checkKvStoreConfig();

  // validate Decision config (e.g. debounce)
  checkDecisionConfig();

  // validate Spark config
  checkSparkConfig();

  // validate Monitor config (e.g. event log)
  checkMonitorConfig();

  // validate Link Monitor config (e.g. backoff)
  checkLinkMonitorConfig();

  // validate VipServiceConfig config
  checkVipServiceConfig();

  // validate BGP Peering config and BGP Translation config
  checkBgpPeeringConfig();

  // validate thrift server config
  if (isSecureThriftServerEnabled()) {
    checkThriftServerConfig();
  }
}

/**
 * TODO: This is the util method to do a translation from:
 *
 * thrift::KvstoreConfig => if/OpenrConfig.thrift
 *
 * to:
 *
 * thrift::KvStoreConfig => if/KvStore.thrift
 *
 * to give smooth migration toward KvStore isolation.
 */
thrift::KvStoreConfig
Config::toThriftKvStoreConfig() const {
  // ATTN: oldConfig and config are defined in different thrift files
  thrift::KvStoreConfig config;

  auto oldConfig = getKvStoreConfig();
  config.node_name() = getNodeName();
  config.key_ttl_ms() = *oldConfig.key_ttl_ms();
  config.ttl_decrement_ms() = *oldConfig.ttl_decrement_ms();
  config.sync_initial_backoff_ms() = *oldConfig.sync_initial_backoff_ms();
  config.sync_max_backoff_ms() = *oldConfig.sync_max_backoff_ms();

  if (auto floodRate = oldConfig.flood_rate()) {
    thrift::KvStoreFloodRate rate;
    rate.flood_msg_per_sec() = *floodRate->flood_msg_per_sec();
    rate.flood_msg_burst_size() = *floodRate->flood_msg_burst_size();

    config.flood_rate() = std::move(rate);
  }
  if (auto setLeafNode = oldConfig.set_leaf_node()) {
    config.set_leaf_node() = *setLeafNode;
  }
  if (auto keyPrefixFilters = oldConfig.key_prefix_filters()) {
    config.key_prefix_filters() = *keyPrefixFilters;
  }
  if (auto keyOriginatorIdFilters = oldConfig.key_originator_id_filters()) {
    config.key_originator_id_filters() = *keyOriginatorIdFilters;
  }
  if (auto maybeIpTos = getConfig().ip_tos()) {
    config.ip_tos() = *maybeIpTos;
  }
  if (auto thriftClientConfig = getThriftClientConfig()) {
    config.enable_secure_thrift_client() =
        *thriftClientConfig->enable_secure_thrift_client();
  }
  auto thriftServer = getThriftServerConfig();
  if (auto x509_cert_path = thriftServer.x509_cert_path()) {
    config.x509_cert_path() = *x509_cert_path;
  }
  if (auto x509_key_path = thriftServer.x509_key_path()) {
    config.x509_key_path() = *x509_key_path;
  }
  if (auto x509_ca_path = thriftServer.x509_ca_path()) {
    config.x509_ca_path() = *x509_ca_path;
  }
  return config;
}

} // namespace openr
