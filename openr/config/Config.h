/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <re2/re2.h>
#include <re2/set.h>

#include <openr/if/gen-cpp2/BgpConfig_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

namespace openr {

using PrefixAllocationParams = std::pair<folly::CIDRNetwork, uint8_t>;

class AreaConfiguration {
 public:
  explicit AreaConfiguration(thrift::AreaConfig const& area)
      : areaId_(area.get_area_id()) {
    neighborRegexSet_ = compileRegexSet(area.get_neighbor_regexes());
    interfaceIncludeRegexSet_ =
        compileRegexSet(area.get_include_interface_regexes());
    interfaceExcludeRegexSet_ =
        compileRegexSet(area.get_exclude_interface_regexes());
    interfaceRedistRegexSet_ =
        compileRegexSet(area.get_redistribute_interface_regexes());
    if (area.get_ingress_policy()) {
      ingressPolicy_ = *area.ingress_policy_ref();
    }
  }

  std::string const&
  getAreaId() const {
    return areaId_;
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
  getIngressPolicy() const {
    return ingressPolicy_;
  }

 private:
  const std::string areaId_;

  std::optional<std::string> ingressPolicy_{std::nullopt};

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
  isNewGRBehaviorEnabled() const {
    return *config_.enable_new_gr_behavior_ref();
  }

  bool
  isOrderedFibProgrammingEnabled() const {
    return config_.enable_ordered_fib_programming_ref().value_or(false);
  }

  bool
  isNetlinkFibHandlerEnabled() const {
    return config_.enable_netlink_fib_handler_ref().value_or(false);
  }

  bool
  isRibPolicyEnabled() const {
    return *config_.enable_rib_policy_ref();
  }

  bool
  isKvStoreThriftEnabled() const {
    return *config_.enable_kvstore_thrift_ref();
  }

  bool
  isPeriodicSyncEnabled() const {
    return *config_.enable_periodic_sync_ref();
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

  void addAreaConfig(thrift::AreaConfig const& areaConfig);

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
  // link monitor
  //
  const thrift::LinkMonitorConfig&
  getLinkMonitorConfig() const {
    return *config_.link_monitor_config_ref();
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
