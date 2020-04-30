/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/if/gen-cpp2/OpenrConfig_types.h>

namespace openr {

class Config {
 public:
  explicit Config(const std::string& configFile);
  explicit Config(thrift::OpenrConfig config) : config_(std::move(config)) {
    populateInternalDb();
  }

  // getter
  const thrift::OpenrConfig&
  getConfig() const {
    return config_;
  }
  std::string getRunningConfig() const;

  const std::string&
  getNodeName() const {
    return config_.node_name;
  }

  const thrift::KvstoreConfig&
  getKvStoreConfig() const {
    return config_.kvstore_config;
  }

  const std::unordered_set<std::string>&
  getAreaIds() const {
    return areaIds_;
  }

 private:
  // thrift config

  void populateInternalDb();
  thrift::OpenrConfig config_;
  std::unordered_set<std::string> areaIds_;
};

} // namespace openr
