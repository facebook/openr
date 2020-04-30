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
  // areas
  thrift::AreaConfig defaultArea;
  defaultArea.area_id = thrift::KvStore_constants::kDefaultArea();
  defaultArea.neighbor_regexes.emplace_back(".*");

  const auto& areas = config_.areas.empty()
      ? std::vector<thrift::AreaConfig>({defaultArea})
      : config_.areas;

  for (const auto& area : areas) {
    CHECK(areaIds_.emplace(area.area_id).second)
        << folly::sformat("Duplicate area config: area_id {}", area.area_id);
  }

  // Kvstore
  const auto& kvConf = config_.kvstore_config;
  if (const auto& floodRate = kvConf.flood_rate_ref()) {
    CHECK_GT(floodRate->flood_msg_per_sec, 0)
        << "kvstore flood_msg_per_sec should be > 0";
    CHECK_GT(floodRate->flood_msg_burst_size, 0)
        << "kvstore flood_msg_burst_size should be > 0";
  }
}
} // namespace openr
