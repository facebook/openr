/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Context.h>
#include <folly/SocketAddress.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/gateway-11s-root-route-programmer/Gateway11sRootRouteProgrammer.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/MonitorClient.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/RouteDampener.h>
#include <openr/fbmeshd/routing/Routing.h>
#include <openr/prefix-manager/PrefixManagerClient.h>

namespace openr {
namespace fbmeshd {

class GatewayConnectivityMonitor : public fbzmq::ZmqEventLoop,
                                   public RouteDampener {
 public:
  explicit GatewayConnectivityMonitor(
      Nl80211Handler& nlHandler,
      const std::string& monitoredInterface,
      std::vector<folly::SocketAddress> monitoredAddresses,
      std::chrono::seconds monitorInterval,
      std::chrono::seconds monitorSocketTimeout,
      const MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext,
      unsigned int penalty,
      unsigned int suppressLimit,
      unsigned int reuseLimit,
      std::chrono::seconds halfLife,
      std::chrono::seconds maxSuppressLimit,
      unsigned int robustness,
      uint8_t setRootModeIfGate,
      Gateway11sRootRouteProgrammer* gateway11sRootRouteProgrammer,
      Routing* routing);

  GatewayConnectivityMonitor() = delete;
  ~GatewayConnectivityMonitor() override = default;
  GatewayConnectivityMonitor(const GatewayConnectivityMonitor&) = delete;
  GatewayConnectivityMonitor(GatewayConnectivityMonitor&&) = delete;
  GatewayConnectivityMonitor& operator=(const GatewayConnectivityMonitor&) =
      delete;
  GatewayConnectivityMonitor& operator=(GatewayConnectivityMonitor&&) = delete;

 private:
  void setStat(const std::string& path, int value) override;
  void dampen() override;
  void undampen() override;

  bool probeWanConnectivity();
  bool probeWanConnectivityRobustly();

  void checkRoutesAndAdvertise();

  void advertiseDefaultRoute();
  void withdrawDefaultRoute();

 private:
  Nl80211Handler& nlHandler_;

  const std::string monitoredInterface_;
  const std::vector<folly::SocketAddress> monitoredAddresses_;
  const std::chrono::seconds monitorSocketTimeout_;
  const unsigned int robustness_;
  const uint8_t setRootModeIfGate_;
  Gateway11sRootRouteProgrammer* gateway11sRootRouteProgrammer_{nullptr};
  Routing* routing_{nullptr};

  std::unique_ptr<fbzmq::ZmqTimeout> connectivityCheckTimer_;

  MonitorClient monitorClient_;

  bool isGatewayActive_{false};
};

} // namespace fbmeshd
} // namespace openr
