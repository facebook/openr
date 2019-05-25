/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Portability.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/StepDetector.h>
#include <openr/common/Types.h>
#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/pinger/PeerPinger.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/kvstore/KvStoreClient.h>

class OpenRMetricManager final {
  // This class should never be copied; remove default copy/move
  OpenRMetricManager() = delete;
  OpenRMetricManager(const OpenRMetricManager&) = delete;
  OpenRMetricManager(OpenRMetricManager&&) = delete;
  OpenRMetricManager& operator=(const OpenRMetricManager&) = delete;
  OpenRMetricManager& operator=(OpenRMetricManager&&) = delete;

 public:
  OpenRMetricManager(
      fbzmq::ZmqEventLoop& zmqLoop,
      openr::fbmeshd::Nl80211Handler* nlHandler,
      std::unique_ptr<PeerPinger> peerPinger,
      const std::string& ifName,
      const std::string& linkMonitorCmdUrl,
      const openr::MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext);

  // override the airtime link metric for the given peer
  void setMetricOverride(folly::MacAddress macAddr, uint32_t metric) noexcept;
  FOLLY_NODISCARD uint32_t
  getMetricOverride(folly::MacAddress macAddr) noexcept;
  int clearMetricOverride(folly::MacAddress macAddr) noexcept;
  int clearMetricOverride() noexcept;

 private:
  /**
   * set all required periodic timers
   */
  void prepareTimers() noexcept;

  /**
   * connect to link monitor's cmdSocket.
   * put it in a separate function from prepareSockets as
   * it may be called again after initialization
   */
  void prepareLinkMonitorCmdSocket() noexcept;

  // get 802.11s metrics from nlHandler
  void getAirtimeMetrics();

  // average airtime metrics over all samples
  float getAvgAirtimeMetric(std::vector<uint32_t>& samples);

  /**
   * This callback is called by step detectors. If a step detector in
   * `stepDetectors_` detects that a metric change is significant, it calls
   * this function with the new metric value. This callback makes a
   * request to OpenR Link monitor to change the adjancency metric for
   * `nodeName`.
   *
   * @param nodeName  the node name for which the metric changed
   * @param newMetric the value of the new metric
   */
  void setOpenrMetric(const std::string& nodeName, const uint32_t newMetric);

  /**
   * This function is periodically called to calculate averaged airtime metrics
   * and submit that information to `stepDetectors_`. It is not responsible
   * for actually updating the metric values in OpenR.
   */
  void submitAvgMetrics();

  void submitPingMetrics();

  /**
   * create a new step-detector with initial metric
   */
  void addStepDetector(folly::MacAddress peer, uint32_t metric);

  // ZmqEventLoop for scheduling async events and socket callback registration
  fbzmq::ZmqEventLoop& zmqLoop_;

  // ZMQ context for processing
  fbzmq::Context& zmqContext_;

  // netlink handler used to request metrics from the kernel
  openr::fbmeshd::Nl80211Handler* nlHandler_;

  std::unique_ptr<PeerPinger> peerPinger_;

  // the mesh interface name
  const std::string ifName_;

  // interval for submitting averaged metrics to step detectors
  std::chrono::seconds stepDetectorSubmitInterval_;

  // timer for periodic query of airtime metrics
  std::unique_ptr<fbzmq::ZmqTimeout> getAirtimeMetricsTimer_;

  // timer for periodic submission of averaged airtime metrics to step detectors
  std::unique_ptr<fbzmq::ZmqTimeout> submitMetricsTimer_;

  // timer for periodic submission of ping metrics to step detectors
  std::unique_ptr<fbzmq::ZmqTimeout> submitPingMetricsTimer_;

  apache::thrift::CompactSerializer serializer_;

  // socket for sending metric changes to link monitor
  const std::string linkMonitorCmdUrl_;
  std::unique_ptr<fbzmq::Socket<ZMQ_DEALER, fbzmq::ZMQ_CLIENT>>
      linkMonitorCmdSock_;

  // detect metric changes
  std::unordered_map<
      folly::MacAddress,
      openr::StepDetector<uint32_t, std::chrono::milliseconds>>
      stepDetectors_;

  // current peers and their metric samples
  std::unordered_map<folly::MacAddress, std::vector<uint32_t>> peers_;

  // number of times metric are submitted to each peer's step-detector
  std::unordered_map<folly::MacAddress, uint32_t> numSubmittedMetrics_;

  std::mutex metricsOverrideMutex_;

  std::unordered_map<folly::MacAddress, int32_t> metricsOverride_;

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_;

  // DS to keep track of stats
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  fbzmq::ZmqMonitorClient zmqMonitorClient_;

}; // OpenRMetricManager
