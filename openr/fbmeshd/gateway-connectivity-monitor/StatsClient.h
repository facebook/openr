/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Context.h>
#include <openr/common/Types.h>

namespace openr {
namespace fbmeshd {

// A class which wraps the fbzmq stats infra allowed for auto submitting of
// stats.
class StatsClient final {
 public:
  StatsClient() = default;
  ~StatsClient() = default;
  StatsClient(const StatsClient&) = delete;
  StatsClient& operator=(const StatsClient&) = delete;
  StatsClient(StatsClient&&) = delete;
  StatsClient& operator=(StatsClient&&) = delete;

  void incrementSumStat(const std::string& stat);
  void setAvgStat(const std::string& stat, int value);

  const std::unordered_map<std::string, int64_t> getStats();

 private:
  // DS to keep track of stats
  fbzmq::ThreadData tData_;
};

} // namespace fbmeshd
} // namespace openr
