/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/messaging/MessagingStats.h>

#include <array>
#include <string_view>

#include <fb303/ServiceData.h>
#include <fmt/format.h>

namespace openr::messaging {

namespace {

using facebook::fb303::ExportTypeConsts;
using facebook::fb303::SlidingWindowPeriodConsts;

/*
 * p50/p95/p99 plus p100. The 1.0 quantile exports as `.p100.60`, the
 * rolling-window max that covers the old lifetime-max use case (a few
 * messages stalled behind a stuck queue are invisible in p99 once the
 * 0-ns direct-handoff samples dilute the window).
 */
constexpr std::array<double, 4> kDwellQuantiles{{0.5, 0.95, 0.99, 1.0}};

constexpr std::string_view kDwellKeyFormat{
    "messaging.rw_queue.{}-{}.time_spent_ns"};

} // namespace

std::shared_ptr<facebook::fb303::QuantileStat>
getRwQueueDwellStat(const std::string& queueName, const std::string& readerId) {
  return facebook::fb303::fbData->getQuantileStat(
      fmt::format(fmt::runtime(kDwellKeyFormat), queueName, readerId),
      ExportTypeConsts::kCountAvg,
      kDwellQuantiles,
      SlidingWindowPeriodConsts::kOneMinTenMin);
}

} // namespace openr::messaging
