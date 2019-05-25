/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include <folly/MacAddress.h>

namespace openr {
namespace fbmeshd {

class MetricManager {
 public:
  virtual ~MetricManager(){};

  virtual std::unordered_map<folly::MacAddress, uint32_t>
  getLinkMetrics() {
    return {};
  };
};

} // namespace fbmeshd
} // namespace openr
