/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/fbmeshd/routing/MetricManager80211s.h"

#include <cstddef>

using namespace openr::fbmeshd;

MetricManager80211s::MetricManager80211s(
    std::chrono::milliseconds interval,
    Nl80211Handler& nlHandler,
    uint32_t ewmaFactor,
    uint32_t hysteresisFactor,
    uint32_t baseBitrate,
    double rssiWeight)
    : nlHandler_{nlHandler},
      ewmaFactor_{ewmaFactor},
      hysteresisFactor_{hysteresisFactor},
      baseBitrate_{baseBitrate},
      rssiWeight_{rssiWeight} {
  // Set timer to update metrics
  metricManagerTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { updateMetrics(); });
  metricManagerTimer_->scheduleTimeout(interval, true);
}

uint32_t
MetricManager80211s::bitrateToAirtime(uint32_t rate) {
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

uint32_t
MetricManager80211s::rssiToAirtime(int32_t rssi) {
  if (rssi == 0) {
    /* Don't return UINT_MAX because old_metric+last_hop_metric would overflow
     */
    return uint32_t{0x7fffffff};
  }
  /* RSSI is not the best way to calculate airtime, but sometimes it is all
   * we have available. We assume NSS-2 VHT80 and use this chart to convert
   * rssi to bitrate, then airtime: https://www.wlanpros.com/mcs-index-charts/
   */
  uint32_t rate;
  if (rssi >= -51) {
    rate = 867;
  } else if (rssi >= -53) {
    rate = 780;
  } else if (rssi >= -58) {
    rate = 650;
  } else if (rssi >= -59) {
    rate = 585;
  } else if (rssi >= -60) {
    rate = 520;
  } else if (rssi >= -64) {
    rate = 390;
  } else if (rssi >= -68) {
    rate = 260;
  } else if (rssi >= -71) {
    rate = 195;
  } else if (rssi >= -73) {
    rate = 130;
  } else {
    rate = 65;
  }

  return uint32_t{8192 / rate};
}

void
MetricManager80211s::updateMetrics() {
  VLOG(8) << "MetricManager80211s: updating metrics...";
  const auto stas = nlHandler_.getStationsInfo();
  for (const auto& it : stas) {
    auto mac = it.macAddress;
    uint32_t newMetricBitrate{bitrateToAirtime(it.expectedThroughput)};
    uint32_t newMetricRssi{rssiToAirtime(it.signalAvgDbm)};
    uint32_t newMetric;

    /* Filter bitrates of 0 so it doesn't polute the average */
    if (it.expectedThroughput == 0) {
      continue;
    }

    /* Filter base bitrate (6Mbps) rates */
    if (it.expectedThroughput == baseBitrate_) {
      VLOG(8) << "MetricManager80211s: filtered base bitrate metric update for "
              << mac;
      continue;
    }

    // if the RSSI based metric seems broken, ignore it
    if (newMetricRssi == 0 || newMetricRssi > 1000) {
      VLOG(1) << "MetricManager80211s: rssi based metric seems broken: "
              << newMetricRssi;
      newMetricRssi = newMetricBitrate;
    }

    newMetric = round(
        (1.0 - rssiWeight_) * newMetricBitrate + rssiWeight_ * newMetricRssi);

    /* Initialize to newMetric to speed up convergence  */
    if (metrics_.count(mac) == 0) {
      VLOG(10) << "MetricManager80211s: first metric entry for " << mac;
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

    VLOG(10) << "MetricManager80211s: " << mac << " adding metric " << newMetric
             << " new metric " << (metrics_[mac].ewmaMetric >> ewmaFactor_);
  }
}

uint32_t
MetricManager80211s::getLinkMetric(const StationInfo& sta) {
  auto mac = sta.macAddress;

  /* If we don't have a metric for this station yet, use expected throughput*/
  if (metrics_.count(mac) == 0) {
    VLOG(10) << "MetricManager80211s: request for unknown: " << mac;
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

  VLOG(10) << "MetricManager80211s: request for " << mac
           << " reply: " << (metrics_[mac].reportedMetric >> ewmaFactor_);

  return metrics_[mac].reportedMetric >> ewmaFactor_;
}

std::unordered_map<folly::MacAddress, uint32_t>
MetricManager80211s::getLinkMetrics() {
  std::unordered_map<folly::MacAddress, uint32_t> metrics;
  for (const auto& sta : nlHandler_.getStationsInfo()) {
    if (sta.expectedThroughput == 0) {
      continue;
    }
    metrics.emplace(sta.macAddress, getLinkMetric(sta));
  }
  return metrics;
}
