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

  // getter
  const thrift::OpenrConfig&
  getConfig() const {
    return config_;
  }

  std::string getRunningConfig() const;

 private:
  // thrift config
  thrift::OpenrConfig config_;
};

} // namespace openr
