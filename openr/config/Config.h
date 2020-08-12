/**
 * Copyright (c) 2014-present, Facebook, Inc.
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

typedef std::pair<folly::CIDRNetwork, uint8_t> PrefixAllocationParams;

struct AreaConfiguration {
  AreaConfiguration(
      const std::string& area,
      std::shared_ptr<re2::RE2::Set> neighborRegexList,
      std::shared_ptr<re2::RE2::Set> interfaceRegexList)
      : area_(area),
        neighborRegexList(std::move(neighborRegexList)),
        interfaceRegexList(std::move(interfaceRegexList)) {}
  const std::string area_;
  std::shared_ptr<re2::RE2::Set> neighborRegexList{nullptr};
  std::shared_ptr<re2::RE2::Set> interfaceRegexList{nullptr};
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
    return config_.node_name;
  }

  const std::string&
  getDomainName() const {
    return config_.domain;
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

  //
  // area
  //
  const std::unordered_set<std::string>&
  getAreaIds() const {
    return areaIds_;
  }

  const std::vector<thrift::AreaConfig>&
  getAreas() const {
    return config_.areas;
  }

  void addAreaRegex(
      const std::string& areaId,
      const std::vector<std::string>& neighborRegexes,
      const std::vector<std::string>& interfaceRegexes);

  void populateAreaConfig();

  const std::unordered_map<std::string, AreaConfiguration>&
  getAreaConfiguration() const {
    return areaConfigs_;
  }

  //
  // spark
  //
  const thrift::SparkConfig&
  getSparkConfig() const {
    return config_.spark_config;
  }

  //
  // kvstore
  //
  const thrift::KvstoreConfig&
  getKvStoreConfig() const {
    return config_.kvstore_config;
  }

  std::chrono::milliseconds
  getKvStoreKeyTtl() const {
    return std::chrono::milliseconds(config_.kvstore_config.key_ttl_ms);
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
    return config_.link_monitor_config;
  }

  std::shared_ptr<const re2::RE2::Set>
  getIncludeItfRegexes() const {
    return includeItfRegexes_;
  }

  std::shared_ptr<const re2::RE2::Set>
  getExcludeItfRegexes() const {
    return excludeItfRegexes_;
  }

  std::shared_ptr<const re2::RE2::Set>
  getRedistributeItfRegexes() const {
    return redistributeItfRegexes_;
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
    return config_.monitor_config;
  }

 private:
  void populateInternalDb();
  // thrift config
  thrift::OpenrConfig config_;
  std::unordered_set<std::string> areaIds_;
  // link monitor regexes
  std::shared_ptr<re2::RE2::Set> includeItfRegexes_{nullptr};
  std::shared_ptr<re2::RE2::Set> excludeItfRegexes_{nullptr};
  std::shared_ptr<re2::RE2::Set> redistributeItfRegexes_{nullptr};
  // prefix allocation
  folly::Optional<PrefixAllocationParams> prefixAllocationParams_{folly::none};

  // areaId -> neighbor regex and interface regex mapped
  std::unordered_map<std::string /* areaId */, AreaConfiguration> areaConfigs_;
};

} // namespace openr
