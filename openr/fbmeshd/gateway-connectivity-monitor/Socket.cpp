/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Socket.h"

#include <sys/socket.h>
#include <unistd.h>

#include <folly/FileUtil.h>

namespace openr {
namespace fbmeshd {

Socket::~Socket() {
  if (fd != -1) {
    if (::close(fd) != 0) {
      // TODO handle error
    }
  }
}

Socket::Result
Socket::connect(
    const std::string& interface,
    const folly::SocketAddress& address,
    const std::chrono::seconds& socketTimeout) {
  if (fd != -1) {
    return {false, "socket object in use"};
  }

  if ((fd = ::socket(
           address.getIPAddress().isV4() ? AF_INET : AF_INET6,
           SOCK_STREAM,
           0)) == -1) {
    return {false, "socket"};
  }

  int enable = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) != 0) {
    return {false, "setsockopt_reuseaddr"};
  }

  // Set socket as non-blocking
  int flags;
  if ((flags = ::fcntl(fd, F_GETFL, 0)) < 0) {
    return {false, "fcntl_getfl"};
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return {false, "fcntl_setfl"};
  }

  // Bind socket to the specific interface
  if (::setsockopt(
          fd,
          SOL_SOCKET,
          SO_BINDTODEVICE,
          interface.c_str(),
          interface.size()) != 0) {
    return {false, "setsockopt_bindtodevice"};
  }

  // Set destination for the connect
  sockaddr_storage addr;
  auto addr_len = address.getAddress(&addr);

  // Attempt connection
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) == 0) {
    return {true, ""};
  }

  if (errno != EINPROGRESS) {
    return {true, "not_einprogress"};
  }

  // Connection is in progress, wait for timeout
  fd_set writefds;
  FD_ZERO(&writefds);
  FD_SET(fd, &writefds);

  timeval timeout;
  timeout.tv_sec = socketTimeout.count();
  timeout.tv_usec = 0;

  if (::select(fd + 1, nullptr, &writefds, nullptr, &timeout) == 0) {
    return {false, "timeout"};
  }

  int err;
  socklen_t err_len{sizeof(err)};
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0) {
    return {false, "getsockopt_error"};
  }

  if (err == 0) {
    return {true, ""};
  } else {
    return {false, "err_non_zero"};
  }
}

} // namespace fbmeshd
} // namespace openr
