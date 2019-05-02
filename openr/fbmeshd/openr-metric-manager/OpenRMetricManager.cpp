/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OpenRMetricManager.h"

#include <chrono>
#include <thread>

#include <folly/IPAddressV6.h>
#include <folly/MacAddress.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/fbmeshd/common/Util.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>

DEFINE_int32(
    step_detector_fast_wnd_size,
    10,
    "size of step-detector's fast moving window (samples)");

DEFINE_int32(
    step_detector_slow_wnd_size,
    30,
    "size of step-detector's slow moving window (samples)");

DEFINE_int32(
    step_detector_low_threshold,
    10,
    "step detector's lower threshold (percent)");

DEFINE_int32(
    step_detector_high_threshold,
    40,
    "step detector's upper threshold (percent)");

DEFINE_int32(
    step_detector_abs_threshold, 500, "step detector's absolute threshold");

DEFINE_int32(
    step_detector_airtime_metric_sample_interval_s,
    1,
    "802.11 metrics sampling interval (second)");

DEFINE_int32(
    step_detector_airtime_metric_avg_interval_s,
    10,
    "802.11 metrics averaging interval (second)");

DEFINE_bool(
    step_detector_static,
    false,
    "True if step-detector should not change metrics after initialization");

DEFINE_int32(
    step_detector_max_metric,
    1366,
    "maximum link metric");


using namespace openr::fbmeshd;

OpenRMetricManager::OpenRMetricManager(
    fbzmq::ZmqEventLoop& zmqLoop,
    openr::fbmeshd::Nl80211Handler* nlHandler,
    const std::string& ifName,
    const std::string& linkMonitorCmdUrl,
    const openr::MonitorSubmitUrl& monitorSubmitUrl,
    fbzmq::Context& zmqContext)
    : zmqLoop_{zmqLoop},
      zmqContext_{zmqContext},
      nlHandler_{nlHandler},
      ifName_{ifName},
      stepDetectorSubmitInterval_(std::chrono::seconds(
          FLAGS_step_detector_airtime_metric_avg_interval_s)),
      linkMonitorCmdUrl_{linkMonitorCmdUrl},
      linkMonitorCmdSock_{nullptr},
      zmqMonitorClient_{zmqContext, monitorSubmitUrl} {
  prepareLinkMonitorCmdSocket();
  prepareTimers();
}

void
OpenRMetricManager::prepareLinkMonitorCmdSocket() noexcept {
  if (linkMonitorCmdSock_) {
    return;
  }
  linkMonitorCmdSock_ =
      std::make_unique<fbzmq::Socket<ZMQ_DEALER, fbzmq::ZMQ_CLIENT>>(
          zmqContext_);
  const auto linkMonitorCmd =
      linkMonitorCmdSock_->connect(fbzmq::SocketUrl{linkMonitorCmdUrl_});
  if (linkMonitorCmd.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << linkMonitorCmdUrl_ << "' "
               << linkMonitorCmd.error();
  }
}

void
OpenRMetricManager::prepareTimers() noexcept {
  // schedule periodic submission of monitor counters
  monitorTimer_ = fbzmq::ZmqTimeout::make(&zmqLoop_, [this]() noexcept {
    zmqMonitorClient_.setCounters(
        openr::prepareSubmitCounters(tData_.getCounters()));
  });
  monitorTimer_->scheduleTimeout(
      openr::Constants::kMonitorSubmitInterval, true);

  // schedule periodic query of airtime metrics
  getAirtimeMetricsTimer_ = fbzmq::ZmqTimeout::make(
      &zmqLoop_, [this]() mutable noexcept { getAirtimeMetrics(); });
  getAirtimeMetricsTimer_->scheduleTimeout(
      std::chrono::seconds(
          FLAGS_step_detector_airtime_metric_sample_interval_s),
      true);

  // schedule periodic submission of averaged airtime metrics to step detectors
  submitMetricsTimer_ = fbzmq::ZmqTimeout::make(
      &zmqLoop_, [this]() mutable noexcept { submitAvgAirTimeMetrics(); });
  submitMetricsTimer_->scheduleTimeout(stepDetectorSubmitInterval_, true);
}

void
OpenRMetricManager::setOpenrMetric(
    const std::string& nodeName, const uint32_t newMetric) {
  // A step detector detected a change in the metric value, send a request
  // to OpenR link monitor to update the metric value for this adjacent node
  auto setAdjMetricRequest = openr::thrift::LinkMonitorRequest(
      apache::thrift::FRAGILE,
      openr::thrift::LinkMonitorCommand::SET_ADJ_METRIC,
      ifName_,
      newMetric,
      nodeName);
  prepareLinkMonitorCmdSocket();
  VLOG(1) << "Set adj metric for " << nodeName << " to " << newMetric;
  const auto res =
      linkMonitorCmdSock_->sendThriftObj(setAdjMetricRequest, serializer_);
  if (res.hasError()) {
    LOG(ERROR) << "Exception in sending setAdjMetricRequest to link monitor. "
               << " exception: " << res.error();
    linkMonitorCmdSock_.reset();
    tData_.addStatValue(
        "fbmeshd.openr_metric_manager.num_metric_changes.failed",
        1,
        fbzmq::SUM);
    return;
  }
  tData_.addStatValue(
      "fbmeshd.openr_metric_manager.num_metric_changes.success", 1, fbzmq::SUM);
}

void
OpenRMetricManager::addStepDetector(folly::MacAddress peer, uint32_t metric) {
  auto stepDetector = stepDetectors_.find(peer);
  if (stepDetector != stepDetectors_.end()) {
    return;
  }
  VLOG(1) << "creating new stepdetector for peer " << peer;
  const auto& nodeName = macAddrToNodeName(peer);
  auto metricChangeCb = [this, nodeName](const uint32_t newMetric) {
    setOpenrMetric(nodeName, newMetric);
  };
  bool inserted;
  std::tie(stepDetector, inserted) = stepDetectors_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peer),
      std::forward_as_tuple(
          stepDetectorSubmitInterval_,
          FLAGS_step_detector_fast_wnd_size,
          FLAGS_step_detector_slow_wnd_size,
          FLAGS_step_detector_low_threshold,
          FLAGS_step_detector_high_threshold,
          FLAGS_step_detector_abs_threshold,
          std::move(metricChangeCb)));
  numSubmittedMetrics_[peer] = 0;
  setOpenrMetric(nodeName, metric);

  // for initialization, we call addValue N times, where N is
  // the size of slow_wnd_size. This is needed to set initial slow avg
  // and fast avg correctly
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  for (int i = 0; i < FLAGS_step_detector_slow_wnd_size; i++) {
    stepDetector->second.addValue(
        std::chrono::duration_cast<std::chrono::seconds>(
            now + i * stepDetectorSubmitInterval_),
        metric);
  }
}

void
OpenRMetricManager::getAirtimeMetrics() {
  auto newMetrics = nlHandler_->getMetrics();
  for (const auto& it : newMetrics) {
    // skip if metric is zero
    if (it.second == 0) {
      continue;
    }
    peers_[it.first].emplace_back(it.second);
  }
}

float
OpenRMetricManager::getAvgAirtimeMetric(std::vector<uint32_t>& samples) {
  // remove bogus samples, valued at max-metric=1366, which correspond to
  // the minimum transmission rate used for beacon frames.
  // An exception is when all samples are at max, as this may be caused
  // by a very poor connection that can only send data at minimum rate.
  auto count = std::count(
      samples.begin(), samples.end(), FLAGS_step_detector_max_metric);
  auto end = samples.end();
  if (count < samples.size()) {
    end = std::remove(
        samples.begin(), samples.end(), FLAGS_step_detector_max_metric);
    count = samples.size() - count;
  }

  return std::accumulate(samples.begin(), end, 0.0) / count;
}

void
OpenRMetricManager::updateStepDetectors(
    std::unordered_map<folly::MacAddress, uint32_t> peerMetrics) {
  auto now = std::chrono::steady_clock::now().time_since_epoch();

  // add new step detectors and update the existing ones
  // add slow_wnd_size * submitMetricsInterval to the current time for
  // consistency with the initialization step
  for (auto it : peerMetrics) {
    auto peer = it.first;
    auto metric = it.second;
    auto stepDetector = stepDetectors_.find(peer);
    if (stepDetector == stepDetectors_.end()) {
      addStepDetector(peer, metric);
    } else {
      stepDetector->second.addValue(
          std::chrono::duration_cast<std::chrono::seconds>(
              now +
              FLAGS_step_detector_slow_wnd_size * stepDetectorSubmitInterval_),
          metric);
    }
  }

  // remove stepDetectors of peers not in the new peer set
  for (auto it = stepDetectors_.begin(); it != stepDetectors_.end();) {
    if (peers_.find(it->first) == peers_.end()) {
      VLOG(1) << "removing stepdetector for peer " << it->first;
      it = stepDetectors_.erase(it);
    } else {
      it++;
    }
  }
}

void
OpenRMetricManager::submitAvgAirTimeMetrics() {
  std::unordered_map<folly::MacAddress, uint32_t> avgAirTimeMetrics;
  for (auto it = peers_.begin(); it != peers_.end();) {
    auto peer = it->first;
    // remove the peer if it has fewer than 2 samples
    // todo: change the threshold to a fraction of max possible sample count
    if (it->second.size() < 2) {
      VLOG(1) << "removing peer " << peer << ": no recent samples";
      it = peers_.erase(it);
      continue;
    }

    auto metric = getAvgAirtimeMetric(it->second);

    // reset metric samples
    it->second.clear();

    // this is to stop reporting metrics after adding N initial
    // samples to the step-detector, where N=step_detector_slow_wnd_size
    if (FLAGS_step_detector_static) {
      if (numSubmittedMetrics_[peer] > FLAGS_step_detector_slow_wnd_size) {
        VLOG(3) << peer << " reached slow_wnd_size limit";
        continue;
      } else {
        numSubmittedMetrics_[peer]++;
        VLOG(3) << peer
                << " numSubmittedMetrics: " << numSubmittedMetrics_[peer];
      }
    }

    // apply metric overrides
    metricsOverrideMutex_.lock();
    const auto& ovrr_it = metricsOverride_.find(peer);
    if (ovrr_it != metricsOverride_.end()) {
      auto actualMetric = metric;
      metric = ovrr_it->second;
      VLOG(1) << folly::sformat(
          "{} - {} (overridden from actual {})",
          peer.toString(),
          metric,
          actualMetric);
    } else {
      VLOG(1) << folly::sformat("{} - {}", peer.toString(), metric);
    }
    metricsOverrideMutex_.unlock();

    avgAirTimeMetrics[peer] = metric;
  }
  updateStepDetectors(avgAirTimeMetrics);
}

// override the airtime link metric for the given peer
void
OpenRMetricManager::setMetricOverride(
    folly::MacAddress macAddr, uint32_t metric) noexcept {
  VLOG(1) << "setMetricOverride " << macAddr << " " << metric;
  std::lock_guard<std::mutex> guard{metricsOverrideMutex_};
  metricsOverride_[macAddr] = metric;
}

uint32_t
OpenRMetricManager::getMetricOverride(folly::MacAddress macAddr) noexcept {
  VLOG(2) << "getMetricOverride " << macAddr;
  std::lock_guard<std::mutex> guard{metricsOverrideMutex_};
  const auto& it = metricsOverride_.find(macAddr);
  if (it == metricsOverride_.end()) {
    VLOG(1) << "getMetricOverride not found";
    return 0;
  }
  return it->second;
}

int
OpenRMetricManager::clearMetricOverride(folly::MacAddress macAddr) noexcept {
  std::lock_guard<std::mutex> guard{metricsOverrideMutex_};
  VLOG(2) << "clearMetricOverride " << macAddr;
  return metricsOverride_.erase(macAddr);
}

int
OpenRMetricManager::clearMetricOverride() noexcept {
  std::lock_guard<std::mutex> guard{metricsOverrideMutex_};
  VLOG(2) << "clearMetricOverride (all)";
  int size = metricsOverride_.size();
  metricsOverride_.clear();
  return size;
}
