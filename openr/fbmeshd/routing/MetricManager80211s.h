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
#include <openr/fbmeshd/routing/MetricManager.h>

namespace openr {
namespace fbmeshd {

class MetricManager80211s : public MetricManager {
 public:
  struct Metric {
    uint32_t ewmaMetric{0};
    uint32_t reportedMetric{0};
    uint32_t count{0};
  };

  MetricManager80211s(
      folly::EventBase* evb,
      std::chrono::milliseconds interval,
      Nl80211Handler& nlHandler,
      uint32_t ewmaFactor,
      uint32_t hysteresisFactor,
      uint32_t baseBitrate,
      double rssiWeight);

  // This class should never be copied; remove default copy/move
  MetricManager80211s() = delete;
  ~MetricManager80211s() override = default;
  MetricManager80211s(const MetricManager80211s&) = delete;
  MetricManager80211s(MetricManager80211s&&) = delete;
  MetricManager80211s& operator=(const MetricManager80211s&) = delete;
  MetricManager80211s& operator=(MetricManager80211s&&) = delete;

  uint32_t getLinkMetric(const StationInfo& sta);

  virtual std::unordered_map<folly::MacAddress, uint32_t> getLinkMetrics()
      override;

 private:
  void updateMetrics();
  uint32_t bitrateToAirtime(uint32_t rate);
  uint32_t rssiToAirtime(int32_t rssi);

  folly::EventBase* evb_;
  Nl80211Handler& nlHandler_;
  std::unordered_map<folly::MacAddress, Metric> metrics_;
  uint32_t ewmaFactor_;
  uint32_t hysteresisFactor_;
  uint32_t baseBitrate_;
  double rssiWeight_;
  std::unique_ptr<folly::AsyncTimeout> metricManagerTimer_;
};

} // namespace fbmeshd
} // namespace openr
