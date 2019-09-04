/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StatsClient.h"

namespace openr {
namespace fbmeshd {

void
StatsClient::incrementSumStat(const std::string& stat) {
  tData_.addStatValue(stat, 1, fbzmq::SUM);
}

void
StatsClient::setAvgStat(const std::string& stat, int value) {
  tData_.addStatValue(stat, value, fbzmq::AVG);
}

const std::unordered_map<std::string, int64_t>
StatsClient::getStats() {
  return tData_.getCounters();
}

} // namespace fbmeshd
} // namespace openr
