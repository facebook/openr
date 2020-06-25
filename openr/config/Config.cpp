// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/FileUtil.h>
#include <glog/logging.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "Config.h"

using apache::thrift::util::enumName;
using openr::thrift::PrefixAllocationMode;
using openr::thrift::PrefixForwardingAlgorithm;
using openr::thrift::PrefixForwardingType;

namespace openr {

Config::Config(const std::string& configFile) {
  std::string contents;
  if (not folly::readFile(configFile.c_str(), contents)) {
    LOG(FATAL) << folly::sformat("Could not read config file: {}", configFile);
  }

  auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
  try {
    jsonSerializer.deserialize(contents, config_);
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Could not parse OpenrConfig struct: "
               << folly::exceptionStr(ex);
    throw;
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
    throw std::out_of_range(folly::sformat(
        "invalid allocate_prefix_len ({}), valid range = ({}, 32]",
        allocationPfxLen,
        seedPfx.second));
  }

  if ((seedPfx.first.isV6()) and
      (allocationPfxLen <= seedPfx.second or allocationPfxLen > 128)) {
    throw std::out_of_range(folly::sformat(
        "invalid allocate_prefix_len ({}), valid range = ({}, 128]",
        allocationPfxLen,
        seedPfx.second));
  }

  return {seedPfx, allocationPfxLen};
}

void
Config::addAreaRegex(
    const std::string& areaId,
    const std::vector<std::string>& neighborRegexes,
    const std::vector<std::string>& interfaceRegexes) {
  if (neighborRegexes.empty() and interfaceRegexes.empty()) {
    throw std::invalid_argument(folly::sformat(
        "Invalid config. At least one non-empty regexes for neighbor or interface"));
  }

  re2::RE2::Options regexOpts;
  regexOpts.set_case_sensitive(false);
  std::string regexErr;
  std::shared_ptr<re2::RE2::Set> neighborRegexList{nullptr},
      interfaceRegexList{nullptr};

  // neighbor regex
  if (not neighborRegexes.empty()) {
    neighborRegexList =
        std::make_shared<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);

    for (const auto& regexStr : neighborRegexes) {
      if (-1 == neighborRegexList->Add(regexStr, &regexErr)) {
        throw std::invalid_argument(folly::sformat(
            "Failed to add neighbor regex: {} for area: {}. Error: {}",
            regexStr,
            areaId,
            regexErr));
      }
    }
    if (not neighborRegexList->Compile()) {
      throw std::invalid_argument(
          folly::sformat("Neighbor regex compilation failed"));
    }
  }

  // interface regex
  if (not interfaceRegexes.empty()) {
    interfaceRegexList =
        std::make_shared<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);

    for (const auto& regexStr : interfaceRegexes) {
      if (-1 == interfaceRegexList->Add(regexStr, &regexErr)) {
        throw std::invalid_argument(folly::sformat(
            "Failed to add interface regex: {} for area: {}. Error: {}",
            regexStr,
            areaId,
            regexErr));
      }
    }
    if (not interfaceRegexList->Compile()) {
      throw std::invalid_argument(
          folly::sformat("Interface regex compilation failed"));
    }
  }

  areaConfigs_.emplace(
      areaId,
      AreaConfiguration(
          areaId, std::move(neighborRegexList), std::move(interfaceRegexList)));
}

// parse openrConfig to initialize:
//  1) areaId => [node_name|interface_name] regex matching;
//  2) etc.
void
Config::populateAreaConfig() {
  //
  // Area
  //
  thrift::AreaConfig defaultArea;
  defaultArea.area_id = thrift::KvStore_constants::kDefaultArea();
  defaultArea.interface_regexes.emplace_back(".*");
  defaultArea.neighbor_regexes.emplace_back(".*");

  const auto& areas = config_.areas.empty()
      ? std::vector<thrift::AreaConfig>({defaultArea})
      : config_.areas;

  for (const auto& area : areas) {
    // TODO: Check if we can remove areaIds_ and
    // use areaConfigs_.
    if (not areaIds_.emplace(area.area_id).second) {
      throw std::invalid_argument(
          folly::sformat("Duplicate area config: area_id {}", area.area_id));
    }
  }

  for (const auto& areaConfig : config_.areas) {
    addAreaRegex(
        areaConfig.area_id,
        areaConfig.neighbor_regexes,
        areaConfig.interface_regexes);
  }
}

void
Config::populateInternalDb() {
  populateAreaConfig();

  // prefix forwarding type and algorithm
  const auto& pfxType = config_.prefix_forwarding_type;
  const auto& pfxAlgo = config_.prefix_forwarding_algorithm;

  if (not enumName(pfxType) or not enumName(pfxAlgo)) {
    throw std::invalid_argument(
        "invalid prefix_forwarding_type or prefix_forwarding_algorithm");
  }

  if (pfxAlgo == PrefixForwardingAlgorithm::KSP2_ED_ECMP and
      pfxType != PrefixForwardingType::SR_MPLS) {
    throw std::invalid_argument(
        "prefix_forwarding_type must be set to SR_MPLS for KSP2_ED_ECMP");
  }

  //
  // Fib
  //
  if (isOrderedFibProgrammingEnabled() and areaIds_.size() > 1) {
    throw std::invalid_argument(folly::sformat(
        "enable_ordered_fib_programming only support single area config"));
  }

  //
  // Kvstore
  //
  const auto& kvConf = config_.kvstore_config;
  if (const auto& floodRate = kvConf.flood_rate_ref()) {
    if (floodRate->flood_msg_per_sec <= 0) {
      throw std::out_of_range("kvstore flood_msg_per_sec should be > 0");
    }
    if (floodRate->flood_msg_burst_size <= 0) {
      throw std::out_of_range("kvstore flood_msg_burst_size should be > 0");
    }
  }

  //
  // Spark
  //
  const auto& sparkConfig = config_.spark_config;
  if (sparkConfig.neighbor_discovery_port <= 0 ||
      sparkConfig.neighbor_discovery_port > 65535) {
    throw std::out_of_range(folly::sformat(
        "neighbor_discovery_port ({}) should be in range [0, 65535]",
        sparkConfig.neighbor_discovery_port));
  }

  if (sparkConfig.hello_time_s <= 0) {
    throw std::out_of_range(folly::sformat(
        "hello_time_s ({}) should be > 0", sparkConfig.hello_time_s));
  }

  // When a node starts or a new link comes up we perform fast initial neighbor
  // discovery by sending hello packets with solicitResponse bit set to request
  // an immediate reply. This allows us to discover new neighbors in hundreds
  // of milliseconds (or as configured).
  if (sparkConfig.fastinit_hello_time_ms <= 0) {
    throw std::out_of_range(folly::sformat(
        "fastinit_hello_time_ms ({}) should be > 0",
        sparkConfig.fastinit_hello_time_ms));
  }

  if (sparkConfig.fastinit_hello_time_ms > 1000 * sparkConfig.hello_time_s) {
    throw std::invalid_argument(folly::sformat(
        "fastinit_hello_time_ms ({}) should be <= hold_time_s ({}) * 1000",
        sparkConfig.fastinit_hello_time_ms,
        sparkConfig.hello_time_s));
  }

  // The rate of hello packet send is defined by keepAliveTime.
  // This time must be less than the holdTime for each node.
  if (sparkConfig.keepalive_time_s <= 0) {
    throw std::out_of_range(folly::sformat(
        "keepalive_time_s ({}) should be > 0", sparkConfig.keepalive_time_s));
  }

  if (sparkConfig.keepalive_time_s > sparkConfig.hold_time_s) {
    throw std::invalid_argument(folly::sformat(
        "keepalive_time_s ({}) should be <= hold_time_s ({})",
        sparkConfig.keepalive_time_s,
        sparkConfig.hold_time_s));
  }

  // Hold time tells the receiver how long to keep the information valid for.
  if (sparkConfig.hold_time_s <= 0) {
    throw std::out_of_range(folly::sformat(
        "hold_time_s ({}) should be > 0", sparkConfig.hold_time_s));
  }

  if (sparkConfig.graceful_restart_time_s <= 0) {
    throw std::out_of_range(folly::sformat(
        "graceful_restart_time_s ({}) should be > 0",
        sparkConfig.graceful_restart_time_s));
  }

  if (sparkConfig.graceful_restart_time_s < 3 * sparkConfig.keepalive_time_s) {
    throw std::invalid_argument(folly::sformat(
        "graceful_restart_time_s ({}) should be >= 3 * keepalive_time_s ({})",
        sparkConfig.graceful_restart_time_s,
        sparkConfig.keepalive_time_s));
  }

  //
  // Link Monitor
  //
  const auto& lmConf = config_.link_monitor_config;

  // backoff validation
  if (lmConf.linkflap_initial_backoff_ms < 0) {
    throw std::out_of_range(folly::sformat(
        "linkflap_initial_backoff_ms ({}) should be >= 0",
        lmConf.linkflap_initial_backoff_ms));
  }

  if (lmConf.linkflap_max_backoff_ms < 0) {
    throw std::out_of_range(folly::sformat(
        "linkflap_max_backoff_ms ({}) should be >= 0",
        lmConf.linkflap_max_backoff_ms));
  }

  if (lmConf.linkflap_initial_backoff_ms > lmConf.linkflap_max_backoff_ms) {
    throw std::out_of_range(folly::sformat(
        "linkflap_initial_backoff_ms ({}) should be < linkflap_max_backoff_ms ({})",
        lmConf.linkflap_initial_backoff_ms,
        lmConf.linkflap_max_backoff_ms));
  }

  // Construct the regular expressions to match interface names against
  re2::RE2::Options regexOpts;
  std::string regexErr;

  // include_interface_regexes and exclude_interface_regexes together
  // define RE, which is fed into link-monitor

  // Compiling empty Re2 Set will cause undefined error
  if (lmConf.include_interface_regexes.size()) {
    includeItfRegexes_ =
        std::make_shared<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    for (const auto& regexStr : lmConf.include_interface_regexes) {
      if (includeItfRegexes_->Add(regexStr, &regexErr) == -1) {
        throw std::invalid_argument(folly::sformat(
            "Add include_interface_regexes failed: {}", regexErr));
      }
    }
    if (not includeItfRegexes_->Compile()) {
      throw std::invalid_argument(
          folly::sformat("include_interface_regexes compile failed"));
    }
  }

  if (lmConf.exclude_interface_regexes.size()) {
    excludeItfRegexes_ =
        std::make_shared<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    for (const auto& regexStr : lmConf.exclude_interface_regexes) {
      if (excludeItfRegexes_->Add(regexStr, &regexErr) == -1) {
        throw std::invalid_argument(folly::sformat(
            "Add exclude_interface_regexes failed: {}", regexErr));
      }
    }
    if (not excludeItfRegexes_->Compile()) {
      throw std::invalid_argument(
          folly::sformat("exclude_interface_regexes compile failed"));
    }
  }

  // redistribute_interface_regexes defines interface to be advertised
  if (lmConf.redistribute_interface_regexes.size()) {
    redistributeItfRegexes_ =
        std::make_shared<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    for (const auto& regexStr : lmConf.redistribute_interface_regexes) {
      if (redistributeItfRegexes_->Add(regexStr, &regexErr) == -1) {
        throw std::invalid_argument(folly::sformat(
            "Add redistribute_interface_regexes failed: {}", regexErr));
      }
    }
    if (not redistributeItfRegexes_->Compile()) {
      throw std::invalid_argument(
          folly::sformat("redistribute_interface_regexes compile failed"));
    }
  }

  //
  // Prefix Allocation
  //
  if (isPrefixAllocationEnabled()) {
    // by now areaIds should be filled.
    if (areaIds_.size() > 1) {
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
    if (not enumName(paConf->prefix_allocation_mode)) {
      throw std::invalid_argument("invalid prefix_allocation_mode");
    }

    auto seedPrefix = paConf->seed_prefix_ref().value_or("");
    auto allocatePfxLen = paConf->allocate_prefix_len_ref().value_or(0);

    switch (paConf->prefix_allocation_mode) {
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

  //
  // watchdog
  //
  if (isWatchdogEnabled() and not config_.watchdog_config_ref()) {
    throw std::invalid_argument(
        "enable_watchdog = true, but watchdog_config is empty");
  }

} // namespace openr
} // namespace openr
