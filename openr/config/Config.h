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
  explicit Config(thrift::OpenrConfig config) : config_(std::move(config)) {}

  //
  // Migration function to create config from gflag values.
  //
  // This populate thrift::OpenrConfig structure from flag for 2 purposes:
  //   1. internal module could be migrated to use thrift::OpenrConfig
  //   2. Config loaded with '--config' could be compared to config here
  static std::shared_ptr<Config> createConfigFromGflag();

  // getter
  const thrift::OpenrConfig&
  getConfig() const {
    return config_;
  }

 private:
  thrift::OpenrConfig config_;
};

} // namespace openr
