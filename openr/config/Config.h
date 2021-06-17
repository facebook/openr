/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#if __has_include("filesystem")
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif
#include <folly/IPAddress.h>
#include <folly/io/async/SSLContext.h>
#include <openr/common/MplsUtil.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <optional>

#include <openr/if/gen-cpp2/BgpConfig_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

namespace openr {

using PrefixAllocationParams = std::pair<folly::CIDRNetwork, uint8_t>;

class AreaConfiguration {
 public:
  explicit AreaConfiguration(thrift::AreaConfig const& area)
      : areaId_(area.get_area_id()), areaType_(area.get_area_type()) {
    if (area.area_sr_node_label_ref().has_value()) {
      srNodeLabel_ = *area.area_sr_node_label_ref();
    }
    neighborRegexSet_ = compileRegexSet(area.get_neighbor_regexes());
    interfaceIncludeRegexSet_ =
        compileRegexSet(area.get_include_interface_regexes());
    interfaceExcludeRegexSet_ =
        compileRegexSet(area.get_exclude_interface_regexes());
    interfaceRedistRegexSet_ =
        compileRegexSet(area.get_redistribute_interface_regexes());
    if (area.get_import_policy_name()) {
      importPolicyName_ = *area.import_policy_name_ref();
    }
  }

  std::string const&
  getAreaId() const {
    return areaId_;
  }

  std::optional<openr::thrift::SegmentRoutingNodeLabel>
  getNodeSegmentLabelConfig() const {
    return srNodeLabel_;
  }

  bool
  shouldDiscoverOnIface(std::string const& iface) const {
    return !interfaceExcludeRegexSet_->Match(iface, nullptr) &&
        interfaceIncludeRegexSet_->Match(iface, nullptr);
  }

  bool
  shouldPeerWithNeighbor(std::string const& neighbor) const {
    return neighborRegexSet_->Match(neighbor, nullptr);
  }

  bool
  shouldRedistributeIface(std::string const& iface) const {
    return interfaceRedistRegexSet_->Match(iface, nullptr);
  }

  std::optional<std::string>
  getImportPolicyName() const {
    return importPolicyName_;
  }

 private:
  const std::string areaId_;
  const openr::thrift::AreaType areaType_;
  std::optional<openr::thrift::SegmentRoutingNodeLabel> srNodeLabel_{
      std::nullopt};

  std::optional<std::string> importPolicyName_{std::nullopt};

  // given a list of strings we will convert is to a compiled RE2::Set
  static std::shared_ptr<re2::RE2::Set> compileRegexSet(
      std::vector<std::string> const& strings);

  std::shared_ptr<re2::RE2::Set> neighborRegexSet_, interfaceIncludeRegexSet_,
      interfaceExcludeRegexSet_, interfaceRedistRegexSet_;
};

class Config {
 public:
  explicit Config(const std::string& configFile);
  explicit Config(thrift::OpenrConfig config) : config_(std::move(config)) {
    populateInternalDb();
  }

  static PrefixAllocationParams createPrefixAllocationParams(
      const std::string& seedPfxStr, uint8_t allocationPfxLen);

  //
  // config
  //
  const thrift::OpenrConfig&
  getConfig() const {
    return config_;
  }
  std::string getRunningConfig() const;

  const std::string&
  getNodeName() const {
    return *config_.node_name_ref();
  }

  const std::string&
  getDomainName() const {
    return *config_.domain_ref();
  }

  //
  // feature knobs
  //

  bool
  isV4Enabled() const {
    return config_.enable_v4_ref().value_or(false);
  }

  bool
  isSegmentRoutingEnabled() const {
    return config_.enable_segment_routing_ref().value_or(false);
  }

  bool
  isAdjacencyLabelsEnabled() const {
    if (isSegmentRoutingEnabled() && isSegmentRoutingConfigured()) {
      const auto& srConfig = getSegmentRoutingConfig();
      return srConfig.sr_adj_label_ref().has_value() &&
          srConfig.sr_adj_label_ref()->sr_adj_label_type_ref() !=
          thrift::SegmentRoutingAdjLabelType::DISABLED;
    }
    return false;
  }

  bool
  isNewGRBehaviorEnabled() const {
    return *config_.enable_new_gr_behavior_ref();
  }

  bool
  isNetlinkFibHandlerEnabled() const {
    return config_.enable_netlink_fib_handler_ref().value_or(false);
  }

  bool
  isFibServiceWaitingEnabled() const {
    return *config_.enable_fib_service_waiting_ref();
  }

  bool
  isRibPolicyEnabled() const {
    return *config_.enable_rib_policy_ref();
  }

  bool
  isBestRouteSelectionEnabled() const {
    return *config_.enable_best_route_selection_ref();
  }

  bool
  isLogSubmissionEnabled() const {
    return *getMonitorConfig().enable_event_log_submission_ref();
  }

  bool
  isV4OverV6NexthopEnabled() const {
    return config_.v4_over_v6_nexthop_ref().value_or(false);
  }

  //
  // area
  //

  void populateAreaConfig();

  const std::unordered_map<std::string, AreaConfiguration>&
  getAreas() const {
    return areaConfigs_;
  }

  std::unordered_set<std::string>
  getAreaIds() const {
    std::unordered_set<std::string> ids;
    for (auto const& [id, _] : areaConfigs_) {
      ids.insert(id);
    }
    return ids;
  }

  //
  // spark
  //
  const thrift::SparkConfig&
  getSparkConfig() const {
    return *config_.spark_config_ref();
  }

  //
  // kvstore
  //
  const thrift::KvstoreConfig&
  getKvStoreConfig() const {
    return *config_.kvstore_config_ref();
  }

  std::chrono::milliseconds
  getKvStoreKeyTtl() const {
    return std::chrono::milliseconds(
        *config_.kvstore_config_ref()->key_ttl_ms_ref());
  }

  bool
  isFloodOptimizationEnabled() const {
    return getKvStoreConfig().enable_flood_optimization_ref().value_or(false);
  }

  //
  // decision
  //
  bool
  isBgpRouteProgrammingEnabled() const {
    return config_.get_decision_config().get_enable_bgp_route_programming();
  }

  //
  // link monitor
  //
  const thrift::LinkMonitorConfig&
  getLinkMonitorConfig() const {
    return *config_.link_monitor_config_ref();
  }

  //
  // segment routing
  //
  const thrift::SegmentRoutingConfig&
  getSegmentRoutingConfig() const {
    return *config_.segment_routing_config_ref();
  }

  const thrift::SegmentRoutingNodeLabel&
  getNodeSegmentLabel() const {
    CHECK(
        config_.segment_routing_config_ref().has_value() and
        config_.segment_routing_config_ref()->sr_node_label_ref().has_value());
    return *config_.segment_routing_config_ref()->sr_node_label_ref();
  }

  const thrift::SegmentRoutingAdjLabel&
  getAdjSegmentLabels() const {
    CHECK(
        config_.segment_routing_config_ref().has_value() and
        config_.segment_routing_config_ref()->sr_adj_label_ref().has_value());
    return *config_.segment_routing_config_ref()->sr_adj_label_ref();
  }

  bool
  isSegmentRoutingConfigured() const {
    return config_.segment_routing_config_ref().has_value();
  }

  //
  // prefix Allocation
  //
  bool
  isPrefixAllocationEnabled() const {
    return config_.enable_prefix_allocation_ref().value_or(false);
  }

  const thrift::PrefixAllocationConfig&
  getPrefixAllocationConfig() const {
    CHECK(isPrefixAllocationEnabled());
    return *config_.prefix_allocation_config_ref();
  }

  PrefixAllocationParams
  getPrefixAllocationParams() const {
    CHECK(isPrefixAllocationEnabled());
    return *prefixAllocationParams_;
  }

  // MPLS labels
  bool
  isLabelRangeValid(thrift::LabelRange range) const {
    if (not isMplsLabelValid(*range.start_label_ref())) {
      return false;
    }

    if (not isMplsLabelValid(*range.end_label_ref())) {
      return false;
    }

    if (*range.start_label_ref() > *range.end_label_ref()) {
      return false;
    }

    return true;
  }

  //
  // bgp peering
  //
  bool
  isBgpPeeringEnabled() const {
    return config_.enable_bgp_peering_ref().value_or(false);
  }

  const thrift::BgpConfig&
  getBgpConfig() const {
    CHECK(isBgpPeeringEnabled());
    return *config_.bgp_config_ref();
  }

  const thrift::BgpRouteTranslationConfig&
  getBgpTranslationConfig() const {
    CHECK(isBgpPeeringEnabled());
    return *config_.bgp_translation_config_ref();
  }

  //
  // watch dog
  //
  bool
  isWatchdogEnabled() const {
    return config_.enable_watchdog_ref().value_or(false);
  }

  const thrift::WatchdogConfig&
  getWatchdogConfig() const {
    CHECK(isWatchdogEnabled());
    return *config_.watchdog_config_ref();
  }

  //
  // monitor
  //
  const thrift::MonitorConfig&
  getMonitorConfig() const {
    return *config_.monitor_config_ref();
  }

  //
  // policy
  //
  std::optional<neteng::config::routing_policy::PolicyConfig>
  getAreaPolicies() const {
    return config_.area_policies_ref().to_optional();
  }

  //
  // thrift server
  //
  const thrift::ThriftServerConfig
  getThriftServerConfig() const {
    return config_.get_thrift_server();
  }
  bool
  isSecureThriftServerEnabled() const {
    return getThriftServerConfig().get_enable_secure_thrift_server();
  }

  const std::string
  getSSLCertPath() const {
    auto certPath = getThriftServerConfig().x509_cert_path_ref();
    if ((not certPath) && isSecureThriftServerEnabled()) {
      throw std::invalid_argument(
          "enable_secure_thrift_server = true, but x509_cert_path is empty");
    }
    return certPath.value();
  }

  const std::string
  getSSLEccCurve() const {
    auto eccCurve = getThriftServerConfig().ecc_curve_name_ref();
    if ((not eccCurve) && isSecureThriftServerEnabled()) {
      throw std::invalid_argument(
          "enable_secure_thrift_server = true, but ecc_curve_name is empty");
    }
    return eccCurve.value();
  }

  const std::string
  getSSLCaPath() const {
    auto caPath = getThriftServerConfig().x509_ca_path_ref();
    if ((not caPath) && isSecureThriftServerEnabled()) {
      throw std::invalid_argument(
          "enable_secure_thrift_server = true, but x509_ca_path is empty");
    }
    return caPath.value();
  }

  const std::string
  getSSLKeyPath() const {
    std::string keyPath;
    const auto& keyPathConfig = getThriftServerConfig().x509_key_path_ref();

    // If unspecified x509_key_path, will use x509_cert_path
    if (keyPathConfig) {
      keyPath = keyPathConfig.value();
    } else {
      keyPath = getSSLCertPath();
    }
    return keyPath;
  }

  const std::string
  getSSLSeedPath() const {
    auto seedPath = getThriftServerConfig().ticket_seed_path_ref();
    if ((not seedPath) && isSecureThriftServerEnabled()) {
      throw std::invalid_argument(
          "enable_secure_thrift_server = true, but ticket_seed_path is empty");
    }
    return seedPath.value();
  }

  const std::string
  getSSLAcceptablePeers() {
    // If unspecified, will use accept connection from any authenticated peer
    return getThriftServerConfig().acceptable_peers_ref().value_or("");
  }

  folly::SSLContext::VerifyClientCertificate
  getSSLContextVerifyType() const {
    // Get the verify_client_type config
    auto mode = getThriftServerConfig().verify_client_type_ref().value_or(
        thrift::VerifyClientType::DO_NOT_REQUEST);

    // Set the folly::SSLContext::VerifyClientCertificate for thrift server
    switch (mode) {
    case thrift::VerifyClientType::ALWAYS:
      return folly::SSLContext::VerifyClientCertificate::ALWAYS;

    case thrift::VerifyClientType::IF_PRESENTED:
      return folly::SSLContext::VerifyClientCertificate::IF_PRESENTED;

    default:
      return folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST;
    }
  }

  apache::thrift::SSLPolicy
  getSSLThriftPolicy() const {
    // Get the verify_client_type config
    auto mode = getThriftServerConfig().verify_client_type_ref().value_or(
        thrift::VerifyClientType::DO_NOT_REQUEST);

    // Set the apache::thrift::SSLPolicy for starting thrift server
    switch (mode) {
    case thrift::VerifyClientType::ALWAYS:
      return apache::thrift::SSLPolicy::REQUIRED;

    case thrift::VerifyClientType::IF_PRESENTED:
      return apache::thrift::SSLPolicy::PERMITTED;

    default:
      return apache::thrift::SSLPolicy::DISABLED;
    }
  }

  //
  // thrift client
  //
  std::optional<thrift::ThriftClientConfig>
  getThriftClientConfig() const {
    return config_.thrift_client_ref().to_optional();
  }

  //
  // VIP thrift injection service
  //
  bool
  isVipServiceEnabled() const {
    return config_.enable_vip_service_ref().value_or(false);
  }

  //
  // Drain state
  //
  bool
  isAssumeDrained() const {
    auto undrainedFlagPath = config_.undrained_flag_path_ref();
    // Do not assume drain if the undrained_flag_path is set and the file exists
    if (undrainedFlagPath && fs::exists(*undrainedFlagPath)) {
      return false;
    }
    return *config_.assume_drained_ref();
  }

  //
  // Memory profiling
  //
  bool
  isMemoryProfilingEnabled() const {
    auto memProfileConf = config_.memory_profiling_config_ref();
    return memProfileConf.has_value() and
        memProfileConf.value().enable_memory_profiling_ref().value();
  }

  std::chrono::seconds
  getMemoryProfilingInterval() const {
    if (isMemoryProfilingEnabled()) {
      return std::chrono::seconds(
          config_.memory_profiling_config_ref()->get_heap_dump_interval_s());
    } else {
      throw std::invalid_argument(
          "Trying to set memory profile timer with heap_dump_interval_s, but enable_memory_profiling = false");
    }
  }

 private:
  void populateInternalDb();

  // thrift config
  thrift::OpenrConfig config_;
  // prefix allocation
  folly::Optional<PrefixAllocationParams> prefixAllocationParams_{folly::none};

  // areaId -> neighbor regex and interface regex mapped
  std::unordered_map<std::string /* areaId */, AreaConfiguration> areaConfigs_;
};

} // namespace openr
