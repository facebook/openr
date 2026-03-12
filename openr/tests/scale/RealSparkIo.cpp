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
  ifIndexToNames_[ifIndex].insert(ifName);

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
   * Resolve the real Linux interface name from ifIndex.
   * The passed ifName may be a virtual/fake name (e.g., "spine-0-to-dut")
   * that doesn't exist as a Linux interface. SO_BINDTODEVICE requires
   * a real Linux interface name.
   */
  char realIfName[IF_NAMESIZE];
  const char* bindName = ifName.c_str();
  size_t bindNameLen = ifName.size();

  if (if_indextoname(ifIndex, realIfName) != nullptr) {
    bindName = realIfName;
    bindNameLen = strlen(realIfName);
    if (ifName != realIfName) {
      LOG(INFO) << fmt::format(
          "[REAL-SPARK-IO] Resolved ifIndex {} to real interface '{}' "
          "(registered as '{}')",
          ifIndex,
          realIfName,
          ifName);
    }
  } else {
    LOG(WARNING) << fmt::format(
        "[REAL-SPARK-IO] WARN: Could not resolve ifIndex {} to real name, "
        "using '{}' (may fail)",
        ifIndex,
        ifName);
  }

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

  /* Bind to specific interface using the resolved real name */
  if (setsockopt(
          sockFd,
          SOL_SOCKET,
          SO_BINDTODEVICE,
          bindName,
          static_cast<socklen_t>(bindNameLen)) < 0) {
    LOG(ERROR) << fmt::format(
        "[REAL-SPARK-IO] ERROR: Failed to bind to {}: {} (need CAP_NET_RAW?)",
        bindName,
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

  /* Disable multicast loopback — we don't want to receive our own packets */
  int off = 0;
  if (setsockopt(sockFd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &off, sizeof(off)) <
      0) {
    LOG(WARNING) << fmt::format(
        "[REAL-SPARK-IO] WARN: Failed to disable IPV6_MULTICAST_LOOP: {}",
        strerror(errno));
  }

  /* Set hop limit for multicast — Spark requires 255 (link-local only) */
  int hopLimit = 255;
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

  /*
   * Collect unique ifIndexes. Multiple registered ifNames may map to the
   * same physical interface (ifIndex). We only need one socket per physical
   * interface.
   */
  std::set<int> uniqueIfIndexes;
  for (const auto& [ifName, ifIndex] : ifNameToIndex_) {
    uniqueIfIndexes.insert(ifIndex);
  }

  LOG(INFO) << fmt::format(
      "[REAL-SPARK-IO] Starting receivers for {} physical interfaces "
      "({} registered names)",
      uniqueIfIndexes.size(),
      ifNameToIndex_.size());

  /*
   * Create one socket and one receive thread per physical interface
   */
  for (int ifIndex : uniqueIfIndexes) {
    auto nameIt = ifIndexToName_.find(ifIndex);
    std::string ifName = nameIt != ifIndexToName_.end() ? nameIt->second : "";

    int sockFd = createSocket(ifName, ifIndex);
    if (sockFd < 0) {
      continue;
    }

    ifIndexToSockFd_[ifIndex] = sockFd;

    /*
     * Start receive thread keyed by ifIndex
     */
    receiveThreads_[ifIndex] = std::make_unique<std::thread>(
        [this, ifIndex, sockFd]() { receiveLoop(ifIndex, sockFd); });

    LOG(INFO) << fmt::format(
        "[REAL-SPARK-IO] Started receive thread for ifIndex {} ({} names)",
        ifIndex,
        ifIndexToNames_.count(ifIndex) ? ifIndexToNames_[ifIndex].size() : 0);
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
  for (auto& [ifIndex, thread] : receiveThreads_) {
    if (thread && thread->joinable()) {
      thread->join();
    }
  }
  receiveThreads_.clear();

  LOG(INFO) << "[REAL-SPARK-IO] All receivers stopped";
}

void
RealSparkIo::receiveLoop(int ifIndex, int sockFd) {
  /*
   * Resolve a display name for logging
   */
  std::string displayName;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ifIndexToName_.find(ifIndex);
    if (it != ifIndexToName_.end()) {
      displayName = it->second;
    } else {
      displayName = fmt::format("ifIndex={}", ifIndex);
    }
  }

  LOG(INFO) << fmt::format(
      "[REAL-SPARK-IO] Receive loop started for {} (ifIndex={})",
      displayName,
      ifIndex);

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
        VLOG(2) << "[REAL-SPARK-IO] recvmsg error on " << displayName << ": "
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
      VLOG(2) << "[REAL-SPARK-IO] RECV: " << bytesRead << " bytes on "
              << displayName << " from " << srcIp.str()
              << " (total: " << packetsReceived << " pkts, " << bytesReceived
              << " bytes)";
    }

    /*
     * Dispatch to ALL callbacks registered for ifNames sharing this ifIndex.
     * Multiple fake neighbors may share a physical interface; each needs
     * to receive the packet via its own callback with its own dstIfName.
     */
    std::vector<std::pair<std::string, PacketCallback>> matchingCallbacks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto namesIt = ifIndexToNames_.find(ifIndex);
      if (namesIt != ifIndexToNames_.end()) {
        for (const auto& name : namesIt->second) {
          auto cbIt = callbacks_.find(name);
          if (cbIt != callbacks_.end()) {
            matchingCallbacks.emplace_back(name, cbIt->second);
          }
        }
      }
    }

    for (const auto& [dstIfName, callback] : matchingCallbacks) {
      try {
        /*
         * srcIfName is the DUT's interface (unknown for real network)
         * dstIfName is the registered interface name for this callback
         */
        callback("dut", dstIfName, folly::IPAddress(srcIp), packet);
      } catch (const std::exception& e) {
        if (VLOG_IS_ON(2)) {
          VLOG(2) << "[REAL-SPARK-IO] Callback exception for " << dstIfName
                  << ": " << e.what();
        }
      }
    }
  }

  LOG(INFO) << fmt::format(
      "[REAL-SPARK-IO] Receive loop stopped for {} (received {} packets, {} bytes)",
      displayName,
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
   * Send packet via sendto. The socket is already bound to the correct
   * interface via SO_BINDTODEVICE, so we don't need pktinfo control
   * messages. The destination scope_id routes the multicast correctly.
   */
  struct iovec iov{};
  iov.iov_base = const_cast<char*>(packet.data());
  iov.iov_len = packet.size();

  ssize_t bytesSent = ::sendto(
      sockFd,
      packet.data(),
      packet.size(),
      0,
      reinterpret_cast<const struct sockaddr*>(&dstAddr),
      sizeof(dstAddr));
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
