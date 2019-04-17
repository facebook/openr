/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MonitorClient.h"

#include <openr/common/Constants.h>
#include <openr/common/Util.h>

namespace openr {
namespace fbmeshd {

MonitorClient::MonitorClient(
    fbzmq::ZmqEventLoop* eventLoop,
    const MonitorSubmitUrl& monitorSubmitUrl,
    fbzmq::Context& zmqContext)
    : zmqMonitorClient_{zmqContext, monitorSubmitUrl} {
  // Schedule periodic timer for submission to monitor
  monitorTimer_ = fbzmq::ZmqTimeout::make(eventLoop, [this]() noexcept {
    zmqMonitorClient_.setCounters(
        openr::prepareSubmitCounters(tData_.getCounters()));
  });
  monitorTimer_->scheduleTimeout(
      openr::Constants::kMonitorSubmitInterval, true);
}

void
MonitorClient::incrementSumStat(const std::string& stat) {
  tData_.addStatValue(stat, 1, fbzmq::SUM);
}

void
MonitorClient::setAvgStat(const std::string& stat, int value) {
  tData_.addStatValue(stat, value, fbzmq::AVG);
}

} // namespace fbmeshd
} // namespace openr
