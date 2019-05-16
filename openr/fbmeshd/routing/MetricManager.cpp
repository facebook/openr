/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/fbmeshd/routing/MetricManager.h"

#include <cstddef>

using namespace openr::fbmeshd;

MetricManager::MetricManager(
    folly::EventBase* evb,
    std::chrono::milliseconds interval,
    Nl80211Handler& nlHandler,
    uint32_t ewmaFactor,
    uint32_t hysteresisFactor,
    uint32_t baseBitrate)
    : folly::AsyncTimeout{evb},
      interval_{interval},
      nlHandler_{nlHandler},
      ewmaFactor_{ewmaFactor},
      hysteresisFactor_{hysteresisFactor},
      baseBitrate_{baseBitrate} {}

uint32_t
MetricManager::bitrateToAirtime(uint32_t rate) {
  if (rate == 0) {
    /* Don't return UINT_MAX because old_metric+last_hop_metric would overflow
     */
    return uint32_t{0x7fffffff};
  }
  /* bitrate is in units of 100 Kbps, while we need rate in units of
   * 1Mbps. This will be corrected on txTime computation.
   */
  rate = 1 + ((rate - 1) / 100);
  uint32_t txTime{((1 << 8) + 10 * (8192 << 8) / rate)};
  uint32_t estimatedRetx{((1 << (2 * 8)) / (1 << 8))};
  uint64_t result{(txTime * estimatedRetx) >> (2 * 8)};
  return static_cast<uint32_t>(result);
}

void
MetricManager::timeoutExpired() noexcept {
  VLOG(8) << "MetricManager: updating metrics...";
  const auto stas = nlHandler_.getStationsInfo();
  for (const auto& it : stas) {
    auto mac = it.macAddress;
    uint32_t newMetric{bitrateToAirtime(it.expectedThroughput)};

    /* Filter bitrates of 0 so it doesn't polute the average */
    if (it.expectedThroughput == 0) {
      continue;
    }

    /* Filter base bitrate (6Mbps) rates */
    if (it.expectedThroughput == baseBitrate_) {
      VLOG(8) << "MetricManager: filtered base bitrate metric update for "
              << mac;
      continue;
    }

    /* Initialize to newMetric to speed up convergence  */
    if (metrics_.count(mac) == 0) {
      VLOG(10) << "MetricManager: first metric entry for " << mac;
      metrics_[mac].ewmaMetric = newMetric << ewmaFactor_;
    }

    uint32_t oldMetric{metrics_[mac].ewmaMetric >> ewmaFactor_};
    metrics_[mac].ewmaMetric += newMetric - oldMetric;

    /* We generally see very high metrics initially after links are established,
    which can cause long convergence times because the inital value is far off.
    This hack speeds up convergence for the first 10 samples, by pretending we
    received multiple samples. */
    if (metrics_[mac].count < 10) {
      for (uint32_t i = 10; i > metrics_[mac].count; i--) {
        oldMetric = metrics_[mac].ewmaMetric >> ewmaFactor_;
        metrics_[mac].ewmaMetric += newMetric - oldMetric;
      }
      metrics_[mac].count++;
    }

    VLOG(10) << "MetricManager: " << mac << " adding metric " << newMetric
             << " new metric " << (metrics_[mac].ewmaMetric >> ewmaFactor_);
  }

  scheduleTimeout(interval_);
}

uint32_t
MetricManager::getLinkMetric(const StationInfo& sta) {
  auto mac = sta.macAddress;

  /* If we don't have a metric for this station yet, use expected throughput*/
  if (metrics_.count(mac) == 0) {
    VLOG(10) << "MetricManager: request for unknown: " << mac;
    return bitrateToAirtime(sta.expectedThroughput);
  }

  if (metrics_[mac].reportedMetric == 0) {
    metrics_[mac].reportedMetric = metrics_[mac].ewmaMetric;
  }

  uint32_t hysteresis{metrics_[mac].reportedMetric >> hysteresisFactor_};
  if (metrics_[mac].ewmaMetric > metrics_[mac].reportedMetric + hysteresis ||
      metrics_[mac].ewmaMetric < metrics_[mac].reportedMetric - hysteresis) {
    metrics_[mac].reportedMetric = metrics_[mac].ewmaMetric;
  }

  VLOG(10) << "MetricManager: request for " << mac
           << " reply: " << (metrics_[mac].reportedMetric >> ewmaFactor_);

  return metrics_[mac].reportedMetric >> ewmaFactor_;
}
