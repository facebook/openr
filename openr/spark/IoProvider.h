/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fcntl.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <chrono>

#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>

namespace openr {

//
// This class provides API to mock some syscalls that
// could be useful for testing. The default version
// simply forwards to the system implementation
//
class IoProvider {
 public:
  IoProvider() = default;
  virtual ~IoProvider(){};

  //
  // mocked syscalls
  //
  virtual int socket(int domain, int type, int protocol);

  virtual int fcntl(int fd, int cmd, int arg);

  virtual int bind(
      int sockfd, const struct sockaddr* my_addr, socklen_t addrlen);

  virtual ssize_t recvfrom(
      int sockfd,
      void* buf,
      size_t len,
      int flags,
      struct sockaddr* src_addr,
      socklen_t* addrlen);

  virtual ssize_t sendto(
      int sockfd,
      const void* buf,
      size_t len,
      int flags,
      const struct sockaddr* dest_addr,
      socklen_t addrlen);

  virtual ssize_t recvmsg(int sockfd, struct msghdr* msg, int flags);

  virtual ssize_t sendmsg(int sockfd, const struct msghdr* msg, int flags);

  virtual int setsockopt(
      int sockfd, int level, int optname, const void* optval, socklen_t optlen);

  // Utility functions that operate on sockets

  /*
   * Receive a message on fd, and return its size, interface index,
   * and the source address
   */
  static std::tuple<
      ssize_t /* size */,
      int /* ifIndex */,
      folly::SocketAddress /* srcAddr */,
      int /* hopLimit */,
      std::chrono::microseconds /* kernel timestamp */>
  recvMessage(int fd, unsigned char* buf, int len, IoProvider* ioProvider);

  /*
   * Send message on fd via given interface to the address provided
   * We supply socket address, which has dst IPv6 and port
   */
  static ssize_t sendMessage(
      int fd,
      int ifIndex,
      folly::IPAddressV6 srcAddr,
      folly::SocketAddress dstAddr,
      std::string const& packet,
      IoProvider* ioProvider);

 private:
  IoProvider(IoProvider const&) = delete;
  IoProvider& operator=(IoProvider const&) = delete;
};

} // namespace openr
