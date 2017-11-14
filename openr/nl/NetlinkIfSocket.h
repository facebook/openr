/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <thread>

extern "C" {
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/socket.h>
#include <linux/if.h>
}

namespace openr {

// This is a user handler which the object registers.
// Any interface event is reported to user via this handler
using IfEventHandler =
    std::function<void(const std::string& ifName, bool isUp)>;

// A simple wrapper over Netlink Socket for Interface Events
// Contain all netlink details. We throw execptions for anything gone wrong
// User provides a handler which will be invoked for each interface event
// The user is also expected to poll on the socket and invoke the
// data-processing handler

class NetlinkIfSocket final {
 public:
  // Register the ifEventHandler which will be invoked for each interface event
  explicit NetlinkIfSocket(IfEventHandler ifEventHandler);
  ~NetlinkIfSocket();

  // Owners' responsibilty to poll on our socket
  int getSocketFd() const;

  // Once socket has data, this function will read it from the socket
  // and invoke the registered ifEventHandler for each event
  void dataReady();

 private:
  NetlinkIfSocket(const NetlinkIfSocket&) = delete;
  NetlinkIfSocket& operator=(const NetlinkIfSocket&) = delete;

  // This is the callback we pass into libnl when data is ready on the socket
  // The opaque data will contain the user registered ifEventHandler
  // This is static to match C function vector prototype
  static void LinkEventFunc(
      struct nl_cache*, struct nl_object* obj, int, void* data) noexcept;

  bool isInCreatorThread() const;
  struct nl_sock* socket_{nullptr};
  struct nl_cache* cache_{nullptr};
  struct nl_cache_mngr* cacheManager_{nullptr};
  IfEventHandler ifEventHandler_{nullptr};
  const std::thread::id threadId_;
};
} // namespace openr
