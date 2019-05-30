/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <fbzmq/async/ZmqEventLoop.h>

#include <openr/fbmeshd/routing/Routing.h>
#include <openr/nl/NetlinkSocket.h>

namespace openr {
namespace fbmeshd {

class SyncRoutes80211s {
 public:
  SyncRoutes80211s(Routing* routing, folly::MacAddress nodeAddr);

  // This class should never be copied; remove default copy/move
  SyncRoutes80211s() = delete;
  ~SyncRoutes80211s() = default;
  SyncRoutes80211s(const SyncRoutes80211s&) = delete;
  SyncRoutes80211s(SyncRoutes80211s&&) = delete;
  SyncRoutes80211s& operator=(const SyncRoutes80211s&) = delete;
  SyncRoutes80211s& operator=(SyncRoutes80211s&&) = delete;

 private:
  void doSyncRoutes();

  Routing* routing_;
  folly::MacAddress nodeAddr_;

  fbzmq::ZmqEventLoop zmqEvl_;
  openr::fbnl::NetlinkSocket netlinkSocket_;
  std::thread zmqEvlThread_;

  folly::Optional<std::pair<folly::MacAddress, uint32_t>> currentGate_;
  bool isGateBeforeRouteSync_{false};
};

} // namespace fbmeshd
} // namespace openr
