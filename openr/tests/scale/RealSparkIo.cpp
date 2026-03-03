/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/RealSparkIo.h>

#include <fmt/format.h>
#include <glog/logging.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace openr {

RealSparkIo::RealSparkIo() = default;

RealSparkIo::~RealSparkIo() {
  stopReceiving();

  /*
   * Close all sockets
   */
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [ifIndex, sockFd] : ifIndexToSockFd_) {
    close(sockFd);
  }
}

void
RealSparkIo::addInterface(const std::string& ifName, int ifIndex) {
  std::lock_guard<std::mutex> lock(mutex_);

  ifNameToIndex_[ifName] = ifIndex;
  ifIndexToName_[ifIndex] = ifName;

  LOG(INFO) << fmt::format(
      "[REAL-SPARK-IO] Added interface {} (ifIndex={})", ifName, ifIndex);
}

void
RealSparkIo::setMulticastAddress(
    const folly::IPAddressV6& addr, uint16_t port) {
  mcastAddr_ = addr;
  mcastPort_ = port;
}

int
RealSparkIo::createSocket(const std::string& ifName, int ifIndex) {
  /*
   * Create UDP IPv6 socket
   */
  int sockFd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (sockFd < 0) {
    LOG(ERROR) << fmt::format(
        "[REAL-SPARK-IO] ERROR: Failed to create socket: {}", strerror(errno));
    return -1;
  }

  /*
   * Set socket options
   */
  int on = 1;

  /* Allow reuse of address */
  if (setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    LOG(ERROR) << fmt::format(
        "[REAL-SPARK-IO] ERROR: Failed to set SO_REUSEADDR: {}",
        strerror(errno));
    close(sockFd);
    return -1;
  }

  /* Bind to specific interface */
  if (setsockopt(
          sockFd,
          SOL_SOCKET,
          SO_BINDTODEVICE,
          ifName.c_str(),
          static_cast<socklen_t>(ifName.size())) < 0) {
    LOG(ERROR) << fmt::format(
        "[REAL-SPARK-IO] ERROR: Failed to bind to {}: {} (need CAP_NET_RAW?)",
        ifName,
        strerror(errno));
    close(sockFd);
    return -1;
  }

  /* Enable receiving pktinfo (to get interface index) */
  if (setsockopt(sockFd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)) < 0) {
    LOG(ERROR) << fmt::format(
        "[REAL-SPARK-IO] ERROR: Failed to set IPV6_RECVPKTINFO: {}",
        strerror(errno));
    close(sockFd);
    return -1;
  }

  /* Set hop limit for multicast */
  int hopLimit = 1;
  if (setsockopt(
          sockFd,
          IPPROTO_IPV6,
          IPV6_MULTICAST_HOPS,
          &hopLimit,
          sizeof(hopLimit)) < 0) {
    LOG(WARNING) << fmt::format(
        "[REAL-SPARK-IO] WARN: Failed to set IPV6_MULTICAST_HOPS: {}",
        strerror(errno));
  }

  /* Bind to multicast port */
  struct sockaddr_in6 bindAddr{};
  bindAddr.sin6_family = AF_INET6;
  bindAddr.sin6_addr = in6addr_any;
  bindAddr.sin6_port = htons(mcastPort_);

  if (::bind(sockFd, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
    LOG(ERROR) << fmt::format(
        "[REAL-SPARK-IO] ERROR: Failed to bind to port {}: {}",
        mcastPort_,
        strerror(errno));
    close(sockFd);
    return -1;
  }

  /* Join multicast group on this interface */
  struct ipv6_mreq mreq{};
  memcpy(&mreq.ipv6mr_multiaddr, mcastAddr_.bytes(), 16);
  mreq.ipv6mr_interface = ifIndex;

  if (setsockopt(sockFd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) <
      0) {
    LOG(ERROR) << fmt::format(
        "[REAL-SPARK-IO] ERROR: Failed to join multicast group on {}: {}",
        ifName,
        strerror(errno));
    close(sockFd);
    return -1;
  }

  LOG(INFO) << fmt::format(
      "[REAL-SPARK-IO] Socket created on {} (ifIndex={}, fd={}, port={})",
      ifName,
      ifIndex,
      sockFd,
      mcastPort_);

  return sockFd;
}

void
RealSparkIo::registerCallback(
    const std::string& ifName, PacketCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callbacks_[ifName] = std::move(callback);

  VLOG(1) << fmt::format("[REAL-SPARK-IO] Registered callback for {}", ifName);
}

void
RealSparkIo::startReceiving() {
  if (running_.exchange(true)) {
    return; /* already running */
  }

  std::lock_guard<std::mutex> lock(mutex_);

  LOG(INFO) << fmt::format(
      "[REAL-SPARK-IO] Starting receivers for {} interfaces",
      ifNameToIndex_.size());

  /*
   * Create sockets and start receive threads for each interface
   */
  for (const auto& [ifName, ifIndex] : ifNameToIndex_) {
    int sockFd = createSocket(ifName, ifIndex);
    if (sockFd < 0) {
      continue;
    }

    ifIndexToSockFd_[ifIndex] = sockFd;

    /*
     * Start receive thread
     */
    receiveThreads_[ifName] = std::make_unique<std::thread>(
        [this, ifName, sockFd]() { receiveLoop(ifName, sockFd); });

    LOG(INFO)
        << fmt::format("[REAL-SPARK-IO] Started receive thread for {}", ifName);
  }
}

void
RealSparkIo::stopReceiving() {
  if (!running_.exchange(false)) {
    return; /* already stopped */
  }

  std::lock_guard<std::mutex> lock(mutex_);

  LOG(INFO) << "[REAL-SPARK-IO] Stopping receivers...";

  /*
   * Shutdown sockets to unblock recv calls
   */
  for (const auto& [ifIndex, sockFd] : ifIndexToSockFd_) {
    shutdown(sockFd, SHUT_RDWR);
  }

  /*
   * Join all receive threads
   */
  for (auto& [ifName, thread] : receiveThreads_) {
    if (thread && thread->joinable()) {
      thread->join();
    }
  }
  receiveThreads_.clear();

  LOG(INFO) << "[REAL-SPARK-IO] All receivers stopped";
}

void
RealSparkIo::receiveLoop(const std::string& ifName, int sockFd) {
  LOG(INFO)
      << fmt::format("[REAL-SPARK-IO] Receive loop started for {}", ifName);

  constexpr size_t kMaxPacketSize = 65536;
  std::vector<uint8_t> buf(kMaxPacketSize);
  uint64_t packetsReceived = 0;
  uint64_t bytesReceived = 0;

  while (running_.load()) {
    struct sockaddr_in6 srcAddr{};
    socklen_t srcAddrLen = sizeof(srcAddr);

    /*
     * Use recvmsg to get both packet and pktinfo (interface index)
     */
    struct iovec iov{};
    iov.iov_base = buf.data();
    iov.iov_len = buf.size();

    char ctrlBuf[256];
    struct msghdr msg{};
    msg.msg_name = &srcAddr;
    msg.msg_namelen = srcAddrLen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrlBuf;
    msg.msg_controllen = sizeof(ctrlBuf);

    ssize_t bytesRead = recvmsg(sockFd, &msg, 0);
    if (bytesRead < 0) {
      if (running_.load() && VLOG_IS_ON(2)) {
        VLOG(2) << "[REAL-SPARK-IO] recvmsg error on " << ifName << ": "
                << strerror(errno);
      }
      continue;
    }

    if (bytesRead == 0) {
      continue;
    }

    packetsReceived++;
    bytesReceived += bytesRead;

    /*
     * Extract source address
     */
    folly::IPAddressV6 srcIp;
    try {
      srcIp = folly::IPAddressV6::fromBinary(
          folly::ByteRange(
              reinterpret_cast<const uint8_t*>(&srcAddr.sin6_addr), 16));
    } catch (const std::exception& e) {
      if (VLOG_IS_ON(2)) {
        VLOG(2) << "[REAL-SPARK-IO] Failed to parse source IP: " << e.what();
      }
      continue;
    }

    /*
     * Create packet string
     */
    std::string packet(reinterpret_cast<char*>(buf.data()), bytesRead);

    if (VLOG_IS_ON(2)) {
      VLOG(2) << "[REAL-SPARK-IO] RECV: " << bytesRead << " bytes on " << ifName
              << " from " << srcIp.str() << " (total: " << packetsReceived
              << " pkts, " << bytesReceived << " bytes)";
    }

    /*
     * Call callback if registered
     */
    PacketCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = callbacks_.find(ifName);
      if (it != callbacks_.end()) {
        callback = it->second;
      }
    }

    if (callback) {
      try {
        /*
         * srcIfName is the DUT's interface (we don't know it, use "dut")
         * dstIfName is our interface
         */
        callback("dut", ifName, folly::IPAddress(srcIp), packet);
      } catch (const std::exception& e) {
        if (VLOG_IS_ON(2)) {
          VLOG(2) << "[REAL-SPARK-IO] Callback exception: " << e.what();
        }
      }
    }
  }

  LOG(INFO) << fmt::format(
      "[REAL-SPARK-IO] Receive loop stopped for {} (received {} packets, {} bytes)",
      ifName,
      packetsReceived,
      bytesReceived);
}

void
RealSparkIo::sendPacket(
    int dstIfIndex,
    const folly::IPAddress& srcAddr,
    const std::string& packet,
    std::chrono::milliseconds /* latency - ignored for real network */) {
  int sockFd;
  std::string ifName;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ifIndexToSockFd_.find(dstIfIndex);
    if (it == ifIndexToSockFd_.end()) {
      if (VLOG_IS_ON(2)) {
        VLOG(2) << "[REAL-SPARK-IO] WARN: No socket for ifIndex " << dstIfIndex;
      }
      return;
    }
    sockFd = it->second;
    auto nameIt = ifIndexToName_.find(dstIfIndex);
    if (nameIt != ifIndexToName_.end()) {
      ifName = nameIt->second;
    }
  }

  /*
   * Build destination address (multicast)
   */
  struct sockaddr_in6 dstAddr{};
  dstAddr.sin6_family = AF_INET6;
  memcpy(&dstAddr.sin6_addr, mcastAddr_.bytes(), 16);
  dstAddr.sin6_port = htons(mcastPort_);
  dstAddr.sin6_scope_id = dstIfIndex;

  /*
   * Send packet via sendmsg with pktinfo to set source address
   */
  struct iovec iov{};
  iov.iov_base = const_cast<char*>(packet.data());
  iov.iov_len = packet.size();

  /*
   * Build control message for pktinfo (set source address and interface)
   */
  char ctrlBuf[CMSG_SPACE(sizeof(struct in6_pktinfo))];
  memset(ctrlBuf, 0, sizeof(ctrlBuf));

  struct msghdr msg{};
  msg.msg_name = &dstAddr;
  msg.msg_namelen = sizeof(dstAddr);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrlBuf;
  msg.msg_controllen = sizeof(ctrlBuf);

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = IPPROTO_IPV6;
  cmsg->cmsg_type = IPV6_PKTINFO;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

  auto* pktinfo = reinterpret_cast<struct in6_pktinfo*>(CMSG_DATA(cmsg));
  pktinfo->ipi6_ifindex = dstIfIndex;

  /*
   * Set source address in pktinfo (if it's a v6 address)
   */
  if (srcAddr.isV6()) {
    memcpy(&pktinfo->ipi6_addr, srcAddr.asV6().bytes(), 16);
  }

  ssize_t bytesSent = sendmsg(sockFd, &msg, 0);
  if (bytesSent < 0) {
    if (VLOG_IS_ON(2)) {
      VLOG(2) << "[REAL-SPARK-IO] ERROR: sendmsg failed on " << ifName
              << " (ifIndex " << dstIfIndex << "): " << strerror(errno);
    }
    return;
  }

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "[REAL-SPARK-IO] SENT: " << bytesSent << " bytes on " << ifName
            << " (ifIndex " << dstIfIndex << ") from " << srcAddr.str();
  }
}

} // namespace openr
