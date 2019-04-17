/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Context.h>
#include <openr/nl/NetlinkSocket.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>

namespace openr {
namespace fbmeshd {

class Gateway11sRootRouteProgrammer : public fbzmq::ZmqEventLoop {
 public:
  explicit Gateway11sRootRouteProgrammer(
      openr::fbmeshd::Nl80211Handler& nlHandler,
      std::chrono::seconds const interval,
      double const gatewayChangeThresholdFactor);

  Gateway11sRootRouteProgrammer() = delete;
  ~Gateway11sRootRouteProgrammer() override = default;
  Gateway11sRootRouteProgrammer(const Gateway11sRootRouteProgrammer&) = delete;
  Gateway11sRootRouteProgrammer(Gateway11sRootRouteProgrammer&&) = delete;
  Gateway11sRootRouteProgrammer& operator=(
      const Gateway11sRootRouteProgrammer&) = delete;
  Gateway11sRootRouteProgrammer& operator=(Gateway11sRootRouteProgrammer&&) =
      delete;

  void setGatewayStatus(bool isGate);

 private:
  void determineBestRoot();

  // netlink handler used to request mpath from the kernel
  Nl80211Handler& nlHandler_;

  std::unique_ptr<fbzmq::ZmqTimeout> timer_;

  openr::fbnl::NetlinkSocket netlinkSocket_;

  folly::Optional<std::pair<folly::MacAddress, int32_t>> currentRoot_;

  double const gatewayChangeThresholdFactor_;

  bool isGate_{false};
};

} // namespace fbmeshd
} // namespace openr
