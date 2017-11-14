/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkIfSocket.h"
#include "NetlinkException.h"

#include <algorithm>
#include <thread>
#include <vector>

#include <folly/Format.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>

namespace {
const std::string kLinkObjectStr("route/link");

// Global thread-local variable
thread_local bool socketExistsOnThread{false};
} // anonymous namespace

namespace openr {

NetlinkIfSocket::NetlinkIfSocket(IfEventHandler ifEventHandler)
    : ifEventHandler_(std::move(ifEventHandler)),
      threadId_(std::this_thread::get_id()) {
  if (socketExistsOnThread) {
    throw NetlinkException("Cannot create another socket on same thread");
  }

  // Global thread-local variable
  SCOPE_SUCCESS {
    socketExistsOnThread = true;
  };

  // We setup the socket explicitly to create our cache explicitly
  int err = 0;
  socket_ = nl_socket_alloc();
  if (socket_ == nullptr) {
    throw NetlinkException("Failed to create socket");
  }

  SCOPE_FAIL {
    nl_socket_free(socket_);
  };

  err = nl_cache_mngr_alloc(
      socket_, NETLINK_ROUTE, NL_AUTO_PROVIDE, &cacheManager_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to create cache Manager. Error: {}", nl_geterror(err)));
  }

  SCOPE_FAIL {
    nl_cache_mngr_free(cacheManager_);
  };

  // Preferring this to nl_cache_mngr_add which uses a string
  // to map cache..
  err = rtnl_link_alloc_cache(socket_, AF_UNSPEC, &cache_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to allocate cache . Error: {}", nl_geterror(err)));
  }

  SCOPE_FAIL {
    nl_cache_free(cache_);
  };

  // We store user handler as opaque data into libnl
  err = nl_cache_mngr_add_cache(
      cacheManager_,
      cache_,
      LinkEventFunc,
      reinterpret_cast<void*>(&ifEventHandler_));

  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to add cache to manager. Error: {}", nl_geterror(err)));
  }
}

bool
NetlinkIfSocket::isInCreatorThread() const {
  return (threadId_ == std::this_thread::get_id());
}

NetlinkIfSocket::~NetlinkIfSocket() {
  if (!isInCreatorThread()) {
    LOG(ERROR) << "Destroying cache from a different thread:"
               << std::this_thread::get_id()
               << " Expected thread: " << threadId_;
  }

  DCHECK(isInCreatorThread());
  DCHECK(socketExistsOnThread);

  LOG(INFO) << "Destroying cache we created";

  // Manager will release our cache_ internally
  // socket_ was created explicitly, so manager will not free it for us
  nl_cache_mngr_free(cacheManager_);
  nl_socket_free(socket_);

  // Global thread-local variable
  socketExistsOnThread = false;
}

void
NetlinkIfSocket::LinkEventFunc(
    struct nl_cache*, struct nl_object* obj, int, void* data) noexcept {
  // Dont throw, log and return early
  const char* objectStr = nl_object_get_type(obj);
  if (objectStr && (std::string(objectStr) != kLinkObjectStr)) {
    LOG(ERROR) << "Invalid nl_object type: " << nl_object_get_type(obj);
    return;
  }

  struct rtnl_link* link = reinterpret_cast<struct rtnl_link*>(obj);
  unsigned int flags = rtnl_link_get_flags(link);
  bool isUp = !!(flags & IFF_RUNNING);
  std::string ifName;
  const char* ifNameStr = rtnl_link_get_name(link);
  if (ifNameStr) {
    ifName.assign(ifNameStr);
  }

  VLOG(4) << folly::sformat(
      "Interface {} -> isUp ? {} : IFI_FLAGS: {:0x}",
      ifName.c_str(),
      isUp,
      flags);

  // Get the user handler back from opaque data
  IfEventHandler& ifEventHandler = *reinterpret_cast<IfEventHandler*>(data);
  ifEventHandler(ifName, isUp);
}

int
NetlinkIfSocket::getSocketFd() const {
  if (!isInCreatorThread()) {
    LOG(ERROR) << "Getting Socket fd from a different thread:"
               << std::this_thread::get_id()
               << " Expected thread: " << threadId_;
    throw NetlinkException("Failed to get socket fd from different thread");
  }
  int fd = nl_socket_get_fd(socket_);
  if (fd != -1) {
    return fd;
  }
  throw NetlinkException("Failed to get socket fd");
}

// Data is ready on socket, read. This will invoke our registered
// handler which in turn will retrieve the user stored handler and
// invoke it
void
NetlinkIfSocket::dataReady() {
  if (!isInCreatorThread()) {
    LOG(ERROR) << "Requesting data ready from a different thread:"
               << std::this_thread::get_id()
               << " Expected thread: " << threadId_;
    throw NetlinkException("Failed to read data from different thread");
  }
  int err = nl_cache_mngr_data_ready(cacheManager_);
  if (err < 0) {
    throw NetlinkException(folly::sformat(
        "Failed to read ready data from cache manager. Error: {}",
        nl_geterror(err)));
  }
}
} // namespace openr
