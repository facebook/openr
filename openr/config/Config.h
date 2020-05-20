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
  isNetlinkSystemHandlerEnabled() const {
    return config_.enable_netlink_system_handler_ref().value_or(false);
  }

  //
  // area
  //
  const std::unordered_set<std::string>&
  getAreaIds() const {
    return areaIds_;
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
};

} // namespace openr
