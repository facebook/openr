// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/FileUtil.h>
#include <glog/logging.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "Config.h"

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

void
Config::populateInternalDb() {
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
    if (not areaIds_.emplace(area.area_id).second) {
      throw std::invalid_argument(
          folly::sformat("Duplicate area config: area_id {}", area.area_id));
    }
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
}
} // namespace openr
