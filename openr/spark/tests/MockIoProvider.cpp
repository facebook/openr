/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockIoProvider.h"

#include <unistd.h>
#include <cerrno>
#include <chrono>

#include <folly/Exception.h>
#include <folly/ScopeGuard.h>
#include <folly/SocketAddress.h>

namespace {
// the UDP port we use in datagrams (dst)
const int kMockedUdpPort{6666};

// the hop limit we set in datagrams
const int kSparkHopLimit{255};
} // namespace

namespace openr {

void
MockIoProvider::setConnectedPairs(ConnectedIfPairs connectedIfPairs) {
  VLOG(4) << "MockIoProvider::setConnectedPairs called";

  std::lock_guard<std::mutex> lock(mutex_);
  connectedIfPairs_ = std::move(connectedIfPairs);
}

int
MockIoProvider::socket(int /* domain */, int /* type */, int /* protocol */) {
  VLOG(4) << "MockIoProvider::socket called";

  // Create pipe for this spark socket. All received messages are written to
  // write end of the pipe and read by spark via read-end
  int fds[2];
  if (pipe2(fds, O_NONBLOCK /* flags */) < 0) {
    LOG(FATAL) << "Failed to create pipe for spark mcast emulation.";
  }

  std::lock_guard<std::mutex> lock(mutex_);
  pipeFds_.emplace(fds[0] /* read-fd */, fds[1] /* write-fd */);
  return fds[0]; // return read-fd
}

int
MockIoProvider::fcntl(int sockFd, int /* cmd */, int /* arg */) {
  VLOG(4) << "MockIoProvider::fcntl called";

  std::lock_guard<std::mutex> lock(mutex_);
  CHECK(pipeFds_.count(sockFd));
  return 0;
}

int
MockIoProvider::bind(
    int sockFd, const struct sockaddr* /* my_addr */, socklen_t /* addrlen */) {
  VLOG(4) << "MockIoProvider::bind called";

  std::lock_guard<std::mutex> lock(mutex_);
  CHECK(pipeFds_.count(sockFd));
  return 0;
}

ssize_t
MockIoProvider::recvmsg(int sockFd, struct msghdr* msg, int /* flags */) {
  std::lock_guard<std::mutex> lock(mutex_);

  SCOPE_FAIL {
    LOG(ERROR) << "MockIoProvider::recvmsg failed";
  };

  VLOG(4) << "MockIoProvider::recvmsg called ";

  CHECK(pipeFds_.count(sockFd));

  auto it = mailboxes_.find(sockFd);
  CHECK_THROW(it != mailboxes_.end(), std::invalid_argument);
  if (it->second.size() == 0) {
    VLOG(4) << "Empty mailbox for fd " << sockFd << " ifName "
            << fdToIfName_[sockFd];
    return -1;
  }

  // Read a byte from the buffer if any. There can be multiple read attempts
  uint8_t buf;
  if (read(sockFd, &buf, sizeof(buf)) > 0) {
    CHECK_EQ(1, buf); // We must receive what we send
  }

  // pull the addr and the message from queue
  auto const ioMessage = it->second.front(); // NOTE copy on purpose
  auto const& srcAddr = ioMessage.srcAddr;
  auto const& packet = ioMessage.data;

  // discard message from queue
  it->second.pop_front();

  // deliver the address
  sockaddr_storage addrStorage;
  folly::SocketAddress sockAddr(srcAddr, kMockedUdpPort);
  sockAddr.getAddress(&addrStorage);

  CHECK(msg->msg_namelen >= sizeof(sockaddr_storage));

  ::memcpy(msg->msg_name, &addrStorage, sizeof(sockaddr_storage));
  msg->msg_namelen = sizeof(sockaddr_storage);

  // croak if user didn't supply big enough buffer
  CHECK(msg->msg_iov->iov_len >= packet.size());

  // deliver the actual data
  ::memcpy(msg->msg_iov->iov_base, packet.data(), packet.size());

  //
  // deliver the control data
  //

  // set the if index and ipv6 address of the sender
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg);
  CHECK(cmsg);
  cmsg->cmsg_level = IPPROTO_IPV6;
  cmsg->cmsg_type = IPV6_PKTINFO;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

  auto pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
  pktinfo->ipi6_ifindex = ioMessage.ifIndex;
  ::memcpy(&pktinfo->ipi6_addr, srcAddr.bytes(), srcAddr.byteCount());

  // set the hop limit
  cmsg = CMSG_NXTHDR(msg, cmsg);
  CHECK(cmsg);
  cmsg->cmsg_level = IPPROTO_IPV6;
  cmsg->cmsg_type = IPV6_HOPLIMIT;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));

  memcpy(
      CMSG_DATA(cmsg),
      reinterpret_cast<const void*>(&kSparkHopLimit),
      sizeof(kSparkHopLimit));

  // set the kernel timestamp
  cmsg = CMSG_NXTHDR(msg, cmsg);
  CHECK(cmsg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SO_TIMESTAMPNS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct timespec));

  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  auto sec = std::chrono::duration_cast<std::chrono::seconds>(ns);
  struct timespec curTime;
  curTime.tv_sec = sec.count();
  curTime.tv_nsec = (ns - sec).count();

  memcpy(
      CMSG_DATA(cmsg),
      reinterpret_cast<const void*>(&curTime),
      sizeof(curTime));

  return packet.size();
}

ssize_t
MockIoProvider::sendmsg(int sockFd, const struct msghdr* msg, int /* flags */) {
  VLOG(4) << "MockIoProvider::sendmsg called";

  SCOPE_FAIL {
    LOG(ERROR) << "MockIoProvider::sendmsg failed";
  };

  std::lock_guard<std::mutex> lock(mutex_);

  CHECK(pipeFds_.count(sockFd));

  struct cmsghdr* cmsg{nullptr};
  int srcIfIndex{-1};
  folly::IPAddress srcAddr;

  for (cmsg = CMSG_FIRSTHDR(const_cast<struct msghdr*>(msg)); cmsg;
       cmsg = CMSG_NXTHDR(const_cast<struct msghdr*>(msg), cmsg)) {
    if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
      struct in6_pktinfo pktinfo;
      memcpy(static_cast<void*>(&pktinfo), CMSG_DATA(cmsg), sizeof(pktinfo));
      srcIfIndex = pktinfo.ipi6_ifindex;
      srcAddr = folly::IPAddress(pktinfo.ipi6_addr);
      break;
    }
  }

  CHECK(srcIfIndex != -1);

  auto srcIfName = ifIndexToIfName_.at(srcIfIndex);

  VLOG(4) << "MockIoProvider::sendmsg sending message from iface " << srcIfName;

  // walk over all connected interfaces
  bool sent = false;
  for (auto const& dstIfNameLatency : connectedIfPairs_[srcIfName]) {
    auto& dstIfName = dstIfNameLatency.first;
    auto& latency = dstIfNameLatency.second;

    int otherFd{-1};
    int dstIfIndex{0};

    try {
      dstIfIndex = ifNameToIfIndex_.at(dstIfName);
    } catch (std::exception const& err) {
      VLOG(1) << "ifname " << dstIfName << " is not bound to an ifIndex";
      continue;
    }

    try {
      otherFd = ifIndexToFd_.at(dstIfIndex);
    } catch (std::out_of_range const& err) {
      LOG(ERROR) << "No sockets bound to " << dstIfName;
      continue;
    }

    // this prevents sending to self
    CHECK(otherFd != sockFd);

    auto& msgQueue = mailboxes_[otherFd];

    // copy the data from iov
    std::string packet(
        reinterpret_cast<const char*>(msg->msg_iov->iov_base),
        msg->msg_iov->iov_len);

    msgQueue.emplace_back(
        dstIfIndex,
        srcAddr,
        std::move(packet),
        std::chrono::milliseconds(latency));

    sent = true;
  }

  // return the length of single vector sent
  if (sent) {
    return msg->msg_iov->iov_len;
  }
  return -1;
}

//
// Simply accept all setsockopts, and build fd to ifName mapping
//
int
MockIoProvider::setsockopt(
    int sockFd,
    int /* level */,
    int optname,
    const void* optval,
    socklen_t /* optlen */) {
  std::lock_guard<std::mutex> lock(mutex_);

  VLOG(4) << "MockIoProvider::setsockopt called";

  // bind fd to the interface name, based on ifIndex
  if (optname == IPV6_JOIN_GROUP) {
    const auto* mreq = static_cast<const struct ipv6_mreq*>(optval);
    auto ifIndex = mreq->ipv6mr_interface;
    std::string ifName;
    try {
      ifName = ifIndexToIfName_.at(ifIndex);
    } catch (std::exception const& err) {
      LOG(ERROR) << "ifIndex " << ifIndex << " is not bound to an ifName";
      errno = ERANGE;
      return -1;
    }
    ifIndexToFd_[ifIndex] = sockFd;
    fdToIfName_[sockFd] = ifName;
  }

  return 0;
}

//
// Store user provided ifName to ifIndex mapping
//
void
MockIoProvider::addIfNameIfIndex(const IfNameAndifIndex& entries) {
  for (const auto& entry : entries) {
    const auto ifName = entry.first;
    const auto ifIndex = entry.second;
    VLOG(4) << "MockIoProvider::addIfNameIfIndex called for " << ifName << " ("
            << ifIndex << ")";

    std::lock_guard<std::mutex> lock(mutex_);

    // we dont care about existing entries..
    // Simply stomp away ...
    ifIndexToIfName_[ifIndex] = ifName;
    ifNameToIfIndex_[ifName] = ifIndex;
  }
}

//
// This is invoked often. It loops through all mailboxes and send a signal
// to spark (via linux pipe) to read the message if there is any active
// message for it.
//
void
MockIoProvider::processMailboxes() {
  VLOG(5) << "MockIoProvider::processMailboxes called";

  const uint8_t buf{1};
  std::vector<int> writeFds;

  // Find all writeFds to signal
  {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& kv : mailboxes_) {
      if (not kv.second.size()) {
        continue; // no messages to read
      }

      auto writeFd = pipeFds_.at(kv.first /* read-fd */);
      auto& ioMessage = kv.second.front();
      if (!ioMessage.clientNotified && ioMessage.isActive()) {
        ioMessage.clientNotified = true;
        writeFds.push_back(writeFd);
      }
    }
  } // release lock

  // Signal on writeFd
  for (auto& writeFd : writeFds) {
    write(writeFd, &buf, sizeof(buf));
  }
}
} // namespace openr
