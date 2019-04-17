/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RouteUpdateMonitor.h"

#include <thread>

#include <folly/Subprocess.h>
#include <folly/system/Shell.h>

using namespace openr::fbmeshd;

RouteUpdateMonitor::RouteUpdateMonitor(
    fbzmq::ZmqEventLoop& zmqLoop, Nl80211Handler& nlHandler)
    : zmqLoop_{zmqLoop}, nlHandler_{nlHandler} {
  sock_ = nl_socket_alloc();
  eventSock_ = nl_socket_alloc();

  if (!sock_) {
    throw std::runtime_error(folly::sformat(
        "RouteUpdateMonitor::{}() failed to allocate rtnetlink socket",
        __func__));
  }

  if (!eventSock_) {
    throw std::runtime_error(folly::sformat(
        "RouteUpdateMonitor::{}() failed to allocate rtnetlink event socket",
        __func__));
  }

  nl_socket_disable_seq_check(eventSock_);

  // set up libnl callbacks to invoke processRouteUpdate on events
  auto valid_cb = [](nl_msg* /* msg */, void* arg) -> int {
    VLOG(8) << folly::sformat("RouteUpdateMonitor::{}() nl callback", __func__);
    RouteUpdateMonitor* obj = reinterpret_cast<RouteUpdateMonitor*>(arg);
    obj->processRouteUpdate();
    return NL_SKIP;
  };
  nl_socket_modify_cb(eventSock_, NL_CB_VALID, NL_CB_CUSTOM, valid_cb, this);

  if (nl_connect(sock_, NETLINK_ROUTE)) {
    throw std::runtime_error(folly::sformat(
        "RouteUpdateMonitor::{}() failed to connect cmd socket", __func__));
  }
  if (nl_connect(eventSock_, NETLINK_ROUTE)) {
    throw std::runtime_error(folly::sformat(
        "RouteUpdateMonitor::{}() failed to connect event socket", __func__));
  }

  // load route cache
  int ret = rtnl_route_alloc_cache(sock_, AF_UNSPEC, 0, &routeCache_);
  if (ret < 0) {
    throw std::runtime_error(folly::sformat(
        "RouteUpdateMonitor::{}() failed to allocate route cache", __func__));
  }

  // subscribe to mcast route updates
  std::array<rtnetlink_groups, 2> groups = {
      RTNLGRP_IPV4_ROUTE,
      RTNLGRP_IPV6_ROUTE,
  };
  for (size_t i = 0; i < groups.size(); i++) {
    ret = nl_socket_add_membership(eventSock_, groups[i]);
    if (ret < 0) {
      throw std::runtime_error(folly::sformat(
          "RouteUpdateMonitor::{}() failed to join multicast group {}",
          __func__,
          (int)groups[i]));
    }
  }

  zmqLoop_.addSocketFd(
      nl_socket_get_fd(eventSock_), POLLIN, [this](int) noexcept {
        VLOG(10) << folly::sformat(
            "RouteUpdateMonitor::{}() select returned", __func__);
        nl_recvmsgs_default(eventSock_);
      });
}

RouteUpdateMonitor::~RouteUpdateMonitor() {
  nl_cache_free(routeCache_);
  nl_socket_free(eventSock_);
  nl_socket_free(sock_);
}

bool
RouteUpdateMonitor::hasDefaultRoute() {
  nl_object* obj = nl_cache_get_first(routeCache_);
  for (; obj; obj = nl_cache_get_next(obj)) {
    rtnl_route* route = (rtnl_route*)obj;
    nl_addr* addr = rtnl_route_get_dst(route);
    if (addr && nl_addr_iszero(addr)) {
      return true;
    }
  }
  return false;
}

void
RouteUpdateMonitor::processRouteUpdate() {
  VLOG(8) << __func__;
  if (nl_cache_refill(sock_, routeCache_) < 0) {
    LOG(ERROR) << "Could not refill routing cache";
    return;
  }
  bool isConnected = hasDefaultRoute();
  nlHandler_.setMeshConnectedToGate(isConnected);
}
