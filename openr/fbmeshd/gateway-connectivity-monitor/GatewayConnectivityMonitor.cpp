/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GatewayConnectivityMonitor.h"

#include <time.h>
#include <chrono>

#include <folly/FileUtil.h>

#include <openr/fbmeshd/gateway-connectivity-monitor/Socket.h>

static constexpr folly::StringPiece statPathPrefixTemplate{
    "fbmeshd.gateway_connectivity_monitor.{}"};

using namespace openr::fbmeshd;

template <typename... Params>
void
writeProcFs(
    folly::StringPiece value,
    const std::string& templateS,
    Params&&... params) {
  folly::writeFile(
      value,
      folly::sformat(templateS, std::forward<Params>(params)...).c_str(),
      O_WRONLY);
}

GatewayConnectivityMonitor::GatewayConnectivityMonitor(
    Nl80211Handler& nlHandler,
    const openr::PrefixManagerLocalCmdUrl& prefixManagerCmdUrl,
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
    Routing* routing,
    bool isOpenrEnabled)
    : RouteDampener{this,
                    penalty,
                    suppressLimit,
                    reuseLimit,
                    halfLife,
                    maxSuppressLimit},
      nlHandler_{nlHandler},
      prefixManagerClient_{prefixManagerCmdUrl, zmqContext, 3000ms},
      monitoredInterface_{monitoredInterface},
      monitoredAddresses_{monitoredAddresses},
      monitorSocketTimeout_{monitorSocketTimeout},
      robustness_{robustness},
      setRootModeIfGate_{setRootModeIfGate},
      gateway11sRootRouteProgrammer_{gateway11sRootRouteProgrammer},
      routing_{routing},
      monitorClient_{this, monitorSubmitUrl, zmqContext},
      isOpenrEnabled_{isOpenrEnabled} {
  // Disable reverse path filtering, i.e.
  // Do not drop packets from non-routable addresses on monitored interface
  writeProcFs("0", "/proc/sys/net/ipv4/conf/{}/rp_filter", monitoredInterface);
  writeProcFs("0", "/proc/sys/net/ipv4/conf/all/rp_filter");

  // Set timer to check routes
  connectivityCheckTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() mutable noexcept { checkRoutesAndAdvertise(); });
  connectivityCheckTimer_->scheduleTimeout(monitorInterval, true);
}

bool
GatewayConnectivityMonitor::probeWanConnectivityRobustly() {
  for (size_t tryNum{0}; tryNum < robustness_; ++tryNum) {
    if (probeWanConnectivity()) {
      return true;
    }
  }
  return false;
}

bool
GatewayConnectivityMonitor::probeWanConnectivity() {
  Socket socket;
  Socket::Result result;

  VLOG(8) << "Probing WAN connectivity...";
  bool connectionSucceeded = false;
  for (const auto& monitoredAddress : monitoredAddresses_) {
    if ((result = socket.connect(
             monitoredInterface_, monitoredAddress, monitorSocketTimeout_))
            .success) {
      VLOG(8) << "Successfully connected to " << monitoredAddress;
      connectionSucceeded = true;
      break;
    } else {
      VLOG(8) << "Failed to connect to " << monitoredAddress;
    }
  }

  if (connectionSucceeded) {
    VLOG(8) << "Probing WAN connectivity succeeded";
    monitorClient_.incrementSumStat(
        "fbmeshd.gateway_connectivity_monitor.probe_wan_connectivity.success");
  } else {
    VLOG(8) << "Probing WAN connectivity failed";
    // If all connection attempts failed, report failure mode of the last one
    monitorClient_.incrementSumStat(folly::sformat(
        "fbmeshd.gateway_connectivity_monitor.probe_wan_connectivity.failed.{}",
        result.errorMsg));
  }
  return connectionSucceeded;
}

void
GatewayConnectivityMonitor::setStat(const std::string& path, int value) {
  monitorClient_.setAvgStat(
      folly::sformat(statPathPrefixTemplate, path), value);
}

void
GatewayConnectivityMonitor::dampen() {
  if (isGatewayActive_) {
    withdrawDefaultRoute();
  }
}

void
GatewayConnectivityMonitor::undampen() {
  if (isGatewayActive_) {
    advertiseDefaultRoute();
  }
}

void
GatewayConnectivityMonitor::checkRoutesAndAdvertise() {
  if (probeWanConnectivityRobustly()) {
    VLOG(8) << "Successfully probed wan connectivity";
    if (!isDampened()) {
      advertiseDefaultRoute();
    } else {
      LOG(INFO) << "Default route dampened, not advertising";
    }
    if (!isGatewayActive_) {
      flap();
    }
    isGatewayActive_ = true;
  } else {
    withdrawDefaultRoute();
    isGatewayActive_ = false;
  }
}

void
GatewayConnectivityMonitor::advertiseDefaultRoute() {
  VLOG(8) << "Advertising default gateway";
  if (isOpenrEnabled_) {
    const auto ret = prefixManagerClient_.addPrefixes({createPrefixEntry(
        toIpPrefix("0.0.0.0/0"), openr::thrift::PrefixType::DEFAULT)});
    if (ret.hasError()) {
      LOG(ERROR) << "Announcing default prefix failed: " << ret.error();
      monitorClient_.incrementSumStat(
          "fbmeshd.gateway_connectivity_monitor.announce_prefix_failed");
    }
  }

  if (setRootModeIfGate_ != 0) {
    nlHandler_.setRootMode(setRootModeIfGate_);
  }
  if (gateway11sRootRouteProgrammer_) {
    gateway11sRootRouteProgrammer_->setGatewayStatus(true);
  }
  if (routing_) {
    routing_->setGatewayStatus(true);
  }
}

void
GatewayConnectivityMonitor::withdrawDefaultRoute() {
  VLOG(8) << "Withdrawing default gateway";
  if (isOpenrEnabled_) {
    const auto ret = prefixManagerClient_.withdrawPrefixes({createPrefixEntry(
        toIpPrefix("0.0.0.0/0"), openr::thrift::PrefixType::DEFAULT)});
    // if the default prefix doesn't exit, the calls still succeeds
    // (ret.hasError() == false) but sets ret.value().success = false.
    if (ret.hasError()) {
      LOG(ERROR) << "Withdrawing default prefix failed: " << ret.error();
      monitorClient_.incrementSumStat(
          "fbmeshd.gateway_connectivity_monitor.withdraw_prefix_failed");
    }
  }

  if (setRootModeIfGate_ != 0) {
    nlHandler_.setRootMode(0);
  }
  if (gateway11sRootRouteProgrammer_) {
    gateway11sRootRouteProgrammer_->setGatewayStatus(false);
  }
  if (routing_) {
    routing_->setGatewayStatus(false);
  }
}
