/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/ScopeGuard.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <glog/logging.h>

#include <openr/tests/mocks/MockIoProviderUtils.h>

namespace openr {

template <typename T>
void
MockIoProviderUtils::prepareSendMessage(
    bufferArgs<T> args, struct networkArgs netargs) {
  auto& msg = args.msg;
  auto data = args.data;
  auto& len = args.len;
  auto& entry = args.entry;
  auto& u = args.u;

  auto srcIfIndex = netargs.srcIfIndex;
  auto srcIPAddr = netargs.srcIPAddr;
  auto dstIPAddr = netargs.dstIPAddr;
  auto kUdpPort = netargs.dstPort;
  auto dstAddrStorage = netargs.dstAddrStorage;

  msg.msg_control = u.cbuf;
  msg.msg_controllen = sizeof(u.cbuf);

  folly::SocketAddress dstSockAddr(dstIPAddr, kUdpPort);
  dstSockAddr.getAddress(&dstAddrStorage);
  msg.msg_name = reinterpret_cast<void*>(&dstAddrStorage);
  msg.msg_namelen = dstSockAddr.getActualSize();

  struct cmsghdr* cmsg{nullptr};
  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = IPPROTO_IPV6;
  cmsg->cmsg_type = IPV6_PKTINFO;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

  auto pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
  pktinfo->ipi6_ifindex = srcIfIndex;
  std::memcpy(&pktinfo->ipi6_addr, srcIPAddr.bytes(), srcIPAddr.byteCount());

  entry.iov_base = data;
  entry.iov_len = len;

  msg.msg_iov = &entry;
  msg.msg_iovlen = 1;
}

template void MockIoProviderUtils::prepareSendMessage<in6_pktinfo>(
    bufferArgs<in6_pktinfo>, struct networkArgs);

template <typename T>
void
MockIoProviderUtils::prepareRecvMessage(
    bufferArgs<T> args, sockaddr_storage& srcAddrStorage) {
  auto& msg = args.msg;
  auto& buf = args.data;
  auto len = args.len;
  auto& entry = args.entry;
  auto& u = args.u;

  std::memset(&msg, 0, sizeof(msg));

  entry.iov_base = buf;
  entry.iov_len = len;

  msg.msg_iov = &entry;
  msg.msg_iovlen = 1;

  std::memset(&u.cbuf[0], 0, sizeof(u.cbuf));
  msg.msg_control = u.cbuf;
  msg.msg_controllen = sizeof(u.cbuf);

  std::memset(&srcAddrStorage, 0, sizeof(srcAddrStorage));
  msg.msg_name = &srcAddrStorage;
  msg.msg_namelen = sizeof(sockaddr_storage);
}

template void MockIoProviderUtils::prepareRecvMessage<char[1024]>(
    bufferArgs<char[1024]>, sockaddr_storage&);

int
MockIoProviderUtils::getMsgIfIndex(struct msghdr* msg) {
  for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg); cmsg;
       cmsg = CMSG_NXTHDR(msg, cmsg)) {
    if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
      auto pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
      return pktinfo->ipi6_ifindex;
    }
  }
  return -1;
}

int
MockIoProviderUtils::createSocketAndJoinGroup(
    std::shared_ptr<openr::MockIoProvider> mockIoProvider,
    int ifIndex,
    folly::IPAddress /* mcastGroup */) {
  int fd = mockIoProvider->socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  CHECK(fd);

  struct ipv6_mreq mreq;
  mreq.ipv6mr_interface = ifIndex;
  if (mockIoProvider->setsockopt(
          fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
    LOG(ERROR) << "setsockopt on ipv6_join_group failed with error "
               << folly::errnoStr(errno);
  }
  return fd;
}

} // namespace openr
