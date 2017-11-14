/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IoProvider.h"

#include <net/if.h>

#include <folly/Format.h>
#include <folly/SocketAddress.h>

namespace openr {

int
IoProvider::socket(int domain, int type, int protocol) {
  return ::socket(domain, type, protocol);
}

int
IoProvider::fcntl(int fd, int cmd, int arg) {
  return ::fcntl(fd, cmd, arg);
}

int
IoProvider::bind(
    int sockfd, const struct sockaddr* my_addr, socklen_t addrlen) {
  return ::bind(sockfd, my_addr, addrlen);
}

ssize_t
IoProvider::recvfrom(
    int sockfd,
    void* buf,
    size_t len,
    int flags,
    struct sockaddr* src_addr,
    socklen_t* addrlen) {
  return ::recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

ssize_t
IoProvider::sendto(
    int sockfd,
    const void* buf,
    size_t len,
    int flags,
    const struct sockaddr* dest_addr,
    socklen_t addrlen) {
  return ::sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

int
IoProvider::setsockopt(
    int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
  return ::setsockopt(sockfd, level, optname, optval, optlen);
}

ssize_t
IoProvider::recvmsg(int sockfd, struct msghdr* msg, int flags) {
  return ::recvmsg(sockfd, msg, flags);
}

ssize_t
IoProvider::sendmsg(int sockfd, const struct msghdr* msg, int flags) {
  return ::sendmsg(sockfd, msg, flags);
}

} // namespace openr
