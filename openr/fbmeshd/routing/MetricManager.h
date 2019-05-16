/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <openr/fbmeshd/802.11s/Nl80211Handler.h>

namespace openr {
namespace fbmeshd {

class MetricManager : public folly::AsyncTimeout {
 public:
  struct Metric {
    uint32_t ewmaMetric{0};
    uint32_t reportedMetric{0};
    uint32_t count{0};
  };

  MetricManager(
      folly::EventBase* evb,
      std::chrono::milliseconds interval,
      Nl80211Handler& nlHandler,
      uint32_t ewmaFactor,
      uint32_t hysteresisFactor,
      uint32_t baseBitrate);

  // This class should never be copied; remove default copy/move
  MetricManager() = delete;
  ~MetricManager() override = default;
  MetricManager(const MetricManager&) = delete;
  MetricManager(MetricManager&&) = delete;
  MetricManager& operator=(const MetricManager&) = delete;
  MetricManager& operator=(MetricManager&&) = delete;

  uint32_t getLinkMetric(const StationInfo& sta);

 private:
  virtual void timeoutExpired() noexcept override;
  uint32_t bitrateToAirtime(uint32_t rate);

  std::chrono::milliseconds interval_;
  Nl80211Handler& nlHandler_;
  std::unordered_map<folly::MacAddress, Metric> metrics_;
  uint32_t ewmaFactor_;
  uint32_t hysteresisFactor_;
  uint32_t baseBitrate_;
};

} // namespace fbmeshd
} // namespace openr
