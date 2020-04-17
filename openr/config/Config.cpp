// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/FileUtil.h>
#include <glog/logging.h>
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
} // namespace openr
