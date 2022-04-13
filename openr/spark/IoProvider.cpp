/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IoProvider.h"

#include <net/if.h>

#include <glog/logging.h>

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

std::tuple<
    ssize_t /* size */,
    int /* ifIndex */,
    folly::SocketAddress /* srcAddr */,
    int /* hopLimit */,
    std::chrono::microseconds /* kernel timestamp */>
IoProvider::recvMessage(
    int fd, unsigned char* buf, int len, openr::IoProvider* ioProvider) {
  // the control message buffer
  // XXX: hardcoded, but this hardly should be a problem
  union {
    char ctrlBuf[CMSG_SPACE(1024)];
    struct cmsghdr align;
  } u;

  // the message header to receive into
  struct msghdr msg;

  // the IO vector for data to be received with recvmsg
  struct iovec entry;

  // for address of the sender
  sockaddr_storage addrStorage;

  ::memset(&msg, 0, sizeof(msg));

  // we only expect to receive one block of data, single entry
  // in the vector
  msg.msg_iov = &entry;
  msg.msg_iovlen = 1;

  // this part is important - if we don't zero the buffer,
  // the CMSG_NXTHDR may burp, because it tries extracting
  // fields from "next header" in the buffer
  ::memset(&u.ctrlBuf[0], 0, sizeof(u.ctrlBuf));

  // control message buffer used to receive dest IP from the kernel
  msg.msg_control = u.ctrlBuf;
  msg.msg_controllen = sizeof(u.ctrlBuf);

  // prepare to receive either v4 or v6 addresses
  ::memset(&addrStorage, 0, sizeof(addrStorage));
  msg.msg_name = &addrStorage;
  msg.msg_namelen = sizeof(sockaddr_storage);

  // write the data here
  entry.iov_base = buf;
  entry.iov_len = len;

  ssize_t bytesRead = ioProvider->recvmsg(fd, &msg, MSG_DONTWAIT);

  if (bytesRead < 0) {
    throw std::runtime_error(folly::sformat(
        "Failed reading message on fd {}: {}", fd, folly::errnoStr(errno)));
  }

  if (msg.msg_flags & MSG_TRUNC) {
    throw std::runtime_error("Message truncated");
  }

  // grab the inIndex we received this packet on and the hopLimit
  // those are available since we requested them via socket options
  struct cmsghdr* cmsg{nullptr};
  int ifIndex{-1};
  int hopLimit{0};

  // use user space timestamp if kernel timestamp is not found
  std::chrono::microseconds recvTs{
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())};

  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == IPPROTO_IPV6) {
      if (cmsg->cmsg_type == IPV6_PKTINFO) {
        struct in6_pktinfo pktinfo;
        memcpy(
            reinterpret_cast<void*>(&pktinfo),
            CMSG_DATA(cmsg),
            sizeof(pktinfo));
        ifIndex = pktinfo.ipi6_ifindex;
      } else if (cmsg->cmsg_type == IPV6_HOPLIMIT) {
        memcpy(
            reinterpret_cast<void*>(&hopLimit),
            CMSG_DATA(cmsg),
            sizeof(hopLimit));
      }
    }
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPNS) {
      struct timespec ts {
        0, 0
      };
      memcpy(reinterpret_cast<void*>(&ts), CMSG_DATA(cmsg), sizeof(ts));

      // cast to int64_t since ts.tv_sec is 32 bits on some platforms like arm
      const int64_t usecs =
          static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
      const std::chrono::microseconds kernelRecvTs(usecs);

      // sanity check
      DCHECK(recvTs >= kernelRecvTs) << "Time anomaly";
      VLOG(4) << "Got kernel-timestamp. It took "
              << (recvTs - kernelRecvTs).count()
              << " us for the packet to get from kernel to user space";
      recvTs = kernelRecvTs;
    }
  } // for

  // build the source socket address from recvmsg data
  folly::SocketAddress srcAddr{};
  // this will throw if sender address was not filled in
  srcAddr.setFromSockaddr(reinterpret_cast<struct sockaddr*>(&addrStorage));

  DCHECK(ifIndex != -1) << "ifIndex is not found";
  DCHECK(hopLimit) << "hopLimit is not found";

  return std::make_tuple(bytesRead, ifIndex, srcAddr, hopLimit, recvTs);
}

ssize_t
IoProvider::sendMessage(
    int fd,
    int ifIndex,
    folly::IPAddressV6 srcAddr,
    folly::SocketAddress dstAddr,
    std::string const& packet,
    IoProvider* ioProvider) {
  struct msghdr msg;
  struct cmsghdr* cmsg{nullptr};

  // pack control buffer, aligned by control message hdr
  union {
    char cbuf[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    struct cmsghdr align;
  } u;

  // Set the destination address for the message
  sockaddr_storage addrStorage;
  dstAddr.getAddress(&addrStorage);

  ::memset(&msg, 0, sizeof(msg));
  msg.msg_name = reinterpret_cast<void*>(&addrStorage);
  msg.msg_namelen = dstAddr.getActualSize();

  // set the source address and source if index for this message
  // this goes into ancilliary data fields
  msg.msg_control = u.cbuf;
  msg.msg_controllen = sizeof(u.cbuf);
  cmsg = CMSG_FIRSTHDR(&msg);

  cmsg->cmsg_level = IPPROTO_IPV6;
  cmsg->cmsg_type = IPV6_PKTINFO;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

  auto pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
  pktinfo->ipi6_ifindex = ifIndex;
  ::memcpy(&pktinfo->ipi6_addr, srcAddr.bytes(), srcAddr.byteCount());

  // the IO vector for data to be sent
  struct iovec entry;
  msg.msg_iov = &entry;
  msg.msg_iovlen = 1;

  // write the data here (we need to remove the const qualifier)
  entry.iov_base = const_cast<char*>(packet.data());
  entry.iov_len = packet.size();

  return ioProvider->sendmsg(fd, &msg, MSG_DONTWAIT);
}

} // namespace openr
