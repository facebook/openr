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
class MonitorClient final {
 public:
  MonitorClient(
      fbzmq::ZmqEventLoop* eventLoop,
      const MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext);

  MonitorClient() = delete;
  ~MonitorClient() = default;
  MonitorClient(const MonitorClient&) = delete;
  MonitorClient& operator=(const MonitorClient&) = delete;
  MonitorClient(MonitorClient&&) = delete;
  MonitorClient& operator=(MonitorClient&&) = delete;

  void incrementSumStat(const std::string& stat);
  void setAvgStat(const std::string& stat, int value);

 private:
  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_;

  // DS to keep track of stats
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  fbzmq::ZmqMonitorClient zmqMonitorClient_;
};

} // namespace fbmeshd
} // namespace openr
