/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netlink/cache.h> // @manual
#include <netlink/route/route.h> // @manual

#include <string>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Optional.h>
#include <folly/Portability.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>

namespace openr {
namespace fbmeshd {

class RouteUpdateMonitor final {
  // This class should never be copied; remove default copy/move
  RouteUpdateMonitor() = delete;
  RouteUpdateMonitor(const RouteUpdateMonitor&) = delete;
  RouteUpdateMonitor(RouteUpdateMonitor&&) = delete;
  RouteUpdateMonitor& operator=(const RouteUpdateMonitor&) = delete;
  RouteUpdateMonitor& operator=(RouteUpdateMonitor&&) = delete;

 public:
  RouteUpdateMonitor(fbzmq::ZmqEventLoop& zmqLoop, Nl80211Handler& nlHandler);
  ~RouteUpdateMonitor();

 private:
  // called whenever the routing table is updated
  void processRouteUpdate();

  // checks whether this node has an L3 route (aka connected to gate)
  FOLLY_NODISCARD bool hasDefaultRoute();

  fbzmq::ZmqEventLoop& zmqLoop_;
  Nl80211Handler& nlHandler_;

  nl_sock* sock_;
  nl_sock* eventSock_;
  nl_cache* routeCache_;

}; // RouteUpdateMonitor

} // namespace fbmeshd
} // namespace openr
