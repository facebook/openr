/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Spark.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sodium.h>

#include <fcntl.h>
#include <algorithm>
#include <functional>
#include <vector>

#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/MapUtil.h>
#include <folly/ScopeGuard.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/gen/Base.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>

#include "IoProvider.h"

namespace {
//
// The min size of IPv6 packet is 1280 bytes. We use this
// so we don't have to care about MTU size/discovery
//
const int kMinIpv6Mtu = 1280;

//
// The acceptable hop limit, assuming we send packets with this TTL
//
const int kSparkHopLimit = 255;

// number of samples in fast sliding window
const size_t kFastWndSize = 10;

// number of samples in slow sliding window
const size_t kSlowWndSize = 30;

// lower threshold, in percentage
const uint8_t kLoThreshold = 2;

// upper threshold, in percentage
const uint8_t kHiThreshold = 5;

// absolute step threshold, in microseconds
const int64_t kAbsThreshold = 500;

//
// Function to get current timestamp in microseconds using steady clock
// NOTE: we use non-monitonic clock since kernel time-stamps do not support
// monotonic timer :(
//
std::chrono::microseconds
getCurrentTimeInUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch());
}

//
// Subscribe/unsubscribe to a multicast group on given interface
//
bool
toggleMcastGroup(
    int fd,
    folly::IPAddress mcastGroup,
    int ifIndex,
    bool join,
    openr::IoProvider* ioProvider) {
  VLOG(2) << "Subscribing to link local multicast on ifIndex " << ifIndex;

  if (!mcastGroup.isMulticast()) {
    LOG(ERROR) << "IP address " << mcastGroup.str() << " is not multicast";
    return false;
  }

  //
  // Join multicast group on interface
  //
  struct ipv6_mreq mreq;
  mreq.ipv6mr_interface = ifIndex;
  ::memcpy(&mreq.ipv6mr_multiaddr, mcastGroup.bytes(), mcastGroup.byteCount());

  if (join) {
    if (ioProvider->setsockopt(
            fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
      LOG(ERROR) << "setsockopt ipv6_join_group failed "
                 << folly::errnoStr(errno);
      return false;
    }

    LOG(INFO) << "Joined multicast addr " << mcastGroup.str() << " on ifindex "
              << ifIndex;

    return true;
  }

  if (ioProvider->setsockopt(
          fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq)) != 0) {
    LOG(ERROR) << "setsockopt ipv6_join_group failed "
               << folly::errnoStr(errno);
    return false;
  }

  LOG(INFO) << "Left multicast addr " << mcastGroup.str() << " on ifindex "
            << ifIndex;

  return true;
}

//
// Receive a message on fd, and return its size, interface index,
// and the source address
//
std::tuple<
    ssize_t /* size */,
    int /* ifIndex */,
    folly::SocketAddress /* srcAddr */,
    int /* hopLimit */,
    std::chrono::microseconds /* kernel timestamp */>
recvMessage(
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
  std::chrono::microseconds recvTs = getCurrentTimeInUs();

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

//
// Send message on fd via given interface to the address provided
// We supply socket address, which has dst IPv6 and port
//
ssize_t
sendMessage(
    int fd,
    int ifIndex,
    folly::IPAddressV6 srcAddr,
    folly::SocketAddress dstAddr,
    std::string const& packet,
    openr::IoProvider* ioProvider) {
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

} // namespace

using namespace fbzmq;

namespace openr {

Spark::Neighbor::Neighbor(
    thrift::SparkNeighbor const& info,
    uint32_t label,
    uint64_t seqNum,
    std::unique_ptr<fbzmq::ZmqTimeout> holdTimer,
    const std::chrono::milliseconds& samplingPeriod,
    std::function<void(const int64_t&)> rttChangeCb)
    : info(info),
      holdTimer(std::move(holdTimer)),
      label(label),
      seqNum(seqNum),
      stepDetector(
          samplingPeriod /* sampling period */,
          kFastWndSize /* fast window size */,
          kSlowWndSize /* slow window size */,
          kLoThreshold /* lower threshold */,
          kHiThreshold /* upper threshold */,
          kAbsThreshold /* absolute threshold */,
          rttChangeCb /* callback function */) {
  CHECK_NE(this->holdTimer.get(), static_cast<void*>(nullptr));
}

Spark::Spark(
    std::string const& myDomainName,
    std::string const& myNodeName,
    uint16_t const udpMcastPort,
    std::chrono::milliseconds myHoldTime,
    std::chrono::milliseconds myKeepAliveTime,
    std::chrono::milliseconds fastInitKeepAliveTime,
    folly::Optional<int> maybeIpTos,
    bool enableV4,
    bool enableSubnetValidation,
    SparkReportUrl const& reportUrl,
    SparkCmdUrl const& cmdUrl,
    MonitorSubmitUrl const& monitorSubmitUrl,
    KvStorePubPort kvStorePubPort,
    KvStoreCmdPort kvStoreCmdPort,
    std::pair<uint32_t, uint32_t> version,
    fbzmq::Context& zmqContext)
    : myDomainName_(myDomainName),
      myNodeName_(myNodeName),
      udpMcastPort_(udpMcastPort),
      myHoldTime_(myHoldTime),
      myKeepAliveTime_(myKeepAliveTime),
      fastInitKeepAliveTime_(fastInitKeepAliveTime),
      enableV4_(enableV4),
      enableSubnetValidation_(enableSubnetValidation),
      reportUrl_(reportUrl),
      reportSocket_(zmqContext),
      cmdUrl_(cmdUrl),
      cmdSocket_(zmqContext),
      kKvStorePubPort_(kvStorePubPort),
      kKvStoreCmdPort_(kvStoreCmdPort),
      kVersion_(apache::thrift::FRAGILE, version.first, version.second),
      ioProvider_(std::make_shared<IoProvider>()) {
  CHECK(myHoldTime_ >= 3 * myKeepAliveTime)
      << "Keep-alive-time must be less than hold-time.";
  CHECK(myKeepAliveTime > std::chrono::milliseconds(0))
      << "Keep-alive-time can't be 0";
  CHECK(fastInitKeepAliveTime > std::chrono::milliseconds(0))
      << "fast-init-keep-alive-time can't be 0";
  CHECK(fastInitKeepAliveTime <= myKeepAliveTime)
      << "fast-init-keep-alive-time must not bigger than keep-alive-time";

  // Initialize list of BucketedTimeSeries
  const std::chrono::seconds sec{1};
  const int32_t numBuckets = Constants::kMaxAllowedPps / 3;
  for (size_t i = 0; i < Constants::kNumTimeSeries; i++) {
    timeSeriesVector_.emplace_back(
        folly::BucketedTimeSeries<int64_t, std::chrono::steady_clock>(
            numBuckets, sec));
  }

  // Initialize ZMQ sockets
  scheduleTimeout(
      std::chrono::seconds(0), [this, maybeIpTos]() { prepare(maybeIpTos); });

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);
}

void
Spark::prepare(folly::Optional<int> maybeIpTos) noexcept {
  VLOG(1) << "Constructing Spark server for node " << myNodeName_;

  // bind socket for adding/removing interfaces
  const auto cmd = cmdSocket_.bind(fbzmq::SocketUrl{cmdUrl_});
  if (cmd.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << cmdUrl_ << "' " << cmd.error();
  }

  // create the socket to inform downstream consumer
  const auto rep = reportSocket_.bind(fbzmq::SocketUrl{reportUrl_});
  if (rep.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << reportUrl_ << "' "
               << rep.error();
  }

  int fd = ioProvider_->socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  mcastFd_ = fd;

  if (fd < 0) {
    LOG(FATAL) << "Failed creating Spark UDP socket. Error: "
               << folly::errnoStr(errno);
  }

  // make socket non-blocking
  if (ioProvider_->fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
    LOG(FATAL) << "Failed making the socket non-blocking. Error: "
               << folly::errnoStr(errno);
  }

  // make v6 only
  int v6Only = 1;
  if (ioProvider_->setsockopt(
          fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6Only, sizeof(v6Only)) != 0) {
    LOG(FATAL) << "Failed making the socket v6 only. Error: "
               << folly::errnoStr(errno);
  }

  // not really needed, but helps us use same port with other listeners, if any
  int reuseAddr = 1;
  if (ioProvider_->setsockopt(
          fd, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr)) != 0) {
    LOG(FATAL) << "Failed making the socket reuse addr. Error: "
               << folly::errnoStr(errno);
  }

  // request additional packet info, e.g. input iface index and sender address
  int recvPktInfo = 1;
  if (ioProvider_->setsockopt(
          fd,
          IPPROTO_IPV6,
          IPV6_RECVPKTINFO,
          &recvPktInfo,
          sizeof(recvPktInfo)) == -1) {
    LOG(FATAL) << "Failed enabling PKTINFO option. Error: "
               << folly::errnoStr(errno);
  }

  // Set ip-tos
  if (maybeIpTos) {
    const int ipTos = *maybeIpTos;
    if (ioProvider_->setsockopt(
            fd, IPPROTO_IPV6, IPV6_TCLASS, &ipTos, sizeof(int)) != 0) {
      LOG(FATAL) << "Failed setting ip-tos value on socket. Error: "
                 << folly::errnoStr(errno);
    }
  }

  //
  // bind the socket to receive any mcast packet
  //
  {
    VLOG(2) << "Binding UDP socket to receive on any destination address";

    auto mcastSockAddr =
        folly::SocketAddress(folly::IPAddress("::"), udpMcastPort_);

    sockaddr_storage addrStorage;
    mcastSockAddr.getAddress(&addrStorage);
    sockaddr* saddr = reinterpret_cast<sockaddr*>(&addrStorage);

    if (ioProvider_->bind(fd, saddr, mcastSockAddr.getActualSize()) != 0) {
      LOG(FATAL) << "Failed binding the socket. Error: "
                 << folly::errnoStr(errno);
    }
  }

  // set the TTL to maximum, so we can check for spoofed addresses
  int ttl = kSparkHopLimit;
  if (ioProvider_->setsockopt(
          fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(ttl)) != 0) {
    LOG(FATAL) << "Failed setting TTL on socket. Error: "
               << folly::errnoStr(errno);
  }

  // allow reporting the packet TTL to user space
  int recvHopLimit = 1;
  if (ioProvider_->setsockopt(
          fd,
          IPPROTO_IPV6,
          IPV6_RECVHOPLIMIT,
          &recvHopLimit,
          sizeof(recvHopLimit)) != 0) {
    LOG(FATAL) << "Failed enabling TTL receive on socket. Error: "
               << folly::errnoStr(errno);
  }

  // disable looping packets to ourselves
  const int loop = 0;
  if (ioProvider_->setsockopt(
          fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof(loop)) != 0) {
    LOG(FATAL) << "Failed disabling looping on socket. Error: "
               << folly::errnoStr(errno);
  }

  // enable timestamping for this socket
  const int enabled = 1;
  if (ioProvider_->setsockopt(
          fd, SOL_SOCKET, SO_TIMESTAMPNS, &enabled, sizeof(enabled)) != 0) {
    LOG(ERROR) << "Failed to enable kernel timestamping. Measured RTTs are "
               << "likely to have more noise in them. Error: "
               << folly::errnoStr(errno);
  }

  LOG(INFO) << "Spark thread attaching socket/events callbacks...";

  // Schedule periodic timer for monitor submission
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);

  // We received an interface add/remove request
  addSocket(RawZmqSocketPtr{*cmdSocket_}, ZMQ_POLLIN, [this](int) noexcept {
    VLOG(2) << "Spark: interface Db received";
    try {
      processInterfaceDbUpdate();
    } catch (std::exception const& err) {
      LOG(ERROR) << "Error processing interface command "
                 << folly::exceptionStr(err);
    }
  });

  // Listen for incoming messages on multicast FD
  addSocketFd(mcastFd_, ZMQ_POLLIN, [this](int) noexcept {
    try {
      processHelloPacket();
    } catch (std::exception const& err) {
      LOG(ERROR) << "Spark: error processing hello packet "
                 << folly::exceptionStr(err);
    }
  });
}

PacketValidationResult
Spark::validateHelloPacket(
    std::string const& ifName, thrift::SparkHelloPacket const& helloPacket) {
  auto const& originator = helloPacket.payload.originator;
  auto const& neighborName = originator.nodeName;
  uint32_t const& remoteVersion =
                          static_cast<uint32_t>(helloPacket.payload.version);

  // in case our own packet has looped
  if (neighborName == myNodeName_) {
    LOG(ERROR) << "Ignore packet from self (" << myNodeName_ << ")";
    tData_.addStatValue("spark.invalid_keepalive.looped_packet", 1, fbzmq::SUM);
    return PacketValidationResult::FAILURE;
  }
  // domain check
  if (originator.domainName != myDomainName_) {
    LOG(ERROR) << "Ignoring hello packet from node " << originator.nodeName
               << " on interface " << originator.ifName
               << " because it's from different domain "
               << originator.domainName
               << ". My domain is " << myDomainName_;
    tData_.addStatValue(
        "spark.invalid_keepalive.different_domain", 1, fbzmq::SUM);
    return PacketValidationResult::FAILURE;
  }
  // version check
  if (remoteVersion < static_cast<uint32_t>(kVersion_.lowestSupportedVersion)) {
    LOG(ERROR) << "Unsupported version: " << neighborName << " "
               << remoteVersion << ", must be >= "
               << kVersion_.lowestSupportedVersion;
    tData_.addStatValue(
        "spark.invalid_keepalive.invalid_version", 1, fbzmq::SUM);
    return PacketValidationResult::FAILURE;
  }

  // validate v4 address subnet
  if (enableV4_ and enableSubnetValidation_) {
    // make sure v4 address is already specified on neighbor
    auto const& myV4Network = interfaceDb_.at(ifName).v4Network;
    auto const& myV4Addr = myV4Network.first;
    auto const& myV4PrefixLen = myV4Network.second;
    auto const& neighV4Addr = originator.transportAddressV4;
    try {
      toIPAddress(neighV4Addr);
    } catch (const folly::IPAddressFormatException& ex) {
      LOG(ERROR) << "Neighbor V4 address is not known";
      tData_.addStatValue(
        "spark.invalid_keepalive.missing_v4_addr", 1, fbzmq::SUM);
      return PacketValidationResult::FAILURE;
    }

    // validate subnet of v4 address
    auto const& neighCidrNetwork =
        folly::sformat("{}/{}", toString(neighV4Addr), myV4PrefixLen);

    if (!myV4Addr.inSubnet(neighCidrNetwork)) {
      LOG(ERROR) << "Neighbor V4 address " << toString(neighV4Addr)
                 << " is not in the same subnet with local V4 address "
                 << myV4Addr.str() << "/" << +myV4PrefixLen;
      tData_.addStatValue(
          "spark.invalid_keepalive.different_subnet", 1, fbzmq::SUM);
      return PacketValidationResult::FAILURE;
    }
  }

  // get the map of tracked neighbors on this interface
  auto& ifNeighbors = neighbors_.at(ifName);

  // see if we already track this neighbor
  auto it = ifNeighbors.find(neighborName);

  // first time we hear from this guy, add to tracking list
  if (it == ifNeighbors.end()) {
    auto holdTimer =
        fbzmq::ZmqTimeout::make(this, [this, ifName, neighborName]() noexcept {
          processNeighborHoldTimeout(ifName, neighborName);
        });

    // Report RTT change
    // capture ifName & originator by copy
    auto rttChangeCb = [this, ifName, originator](const int64_t& newRtt) {
      processNeighborRttChange(ifName, originator, newRtt);
    };

    ifNeighbors.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(neighborName),
        std::forward_as_tuple( // arguments to construct Neighbor object
            originator,
            getNewLabelForIface(ifName),
            helloPacket.payload.seqNum,
            std::move(holdTimer),
            myKeepAliveTime_,
            std::move(rttChangeCb)));
    return PacketValidationResult::SUCCESS;
  }

  // grab existing neighbor; second.first on iterator is the SparkNeighbor
  auto& neighbor = it->second;
  auto newSeqNum = static_cast<uint64_t>(helloPacket.payload.seqNum);

  // Sender's sequence number received in helloPacket is always increasing. If
  // we receive a packet with lower sequence number from adjacent neighbor, then
  // it means that it has restarted.
  // We accept the new sequence number from the neighbor and mark it as a
  // restarting neighbor.
  if (newSeqNum <= neighbor.seqNum) {
    LOG(INFO) << neighborName << " seems to be restarting as received "
              << "unexpected sequence number " << newSeqNum << " instead of "
              << neighbor.seqNum + 1;
    neighbor.info = originator; // Update stored neighbor with new data
    neighbor.seqNum = newSeqNum; // Update the sequence number
    return PacketValidationResult::NEIGHBOR_RESTART;
  }

  // update the sequence number
  neighbor.seqNum = newSeqNum;

  // consider neighbor restart if v4 address has changed on neighbor's
  // interface (due to duplicate IPv4 detection).
  if (enableV4_) {
    auto const& rcvdV4Addr = originator.transportAddressV4;
    auto const& existingV4Addr = neighbor.info.transportAddressV4;
    if (rcvdV4Addr != existingV4Addr) {
      LOG(INFO) << neighborName << " seems to be have reassigned IPv4 address";
      return PacketValidationResult::NEIGHBOR_RESTART;
    }
  }

  return PacketValidationResult::SUCCESS;
}

void
Spark::processNeighborRttChange(
    std::string const& ifName,
    thrift::SparkNeighbor const& originator,
    int64_t const newRtt) {
  // Neighbor must exist if this callback is fired
  auto& neighbor = neighbors_.at(ifName).at(originator.nodeName);

  // only report RTT change if the neighbor is adjacent
  if (!neighbor.isAdjacent) {
    VLOG(2) << "Neighbor is not adjacent, not reporting";
    return;
  }

  VLOG(2) << "RTT for neighbor " << originator.nodeName << " has changed "
          << "from " << neighbor.rtt.count() / 1000.0 << "ms to "
          << newRtt / 1000.0 << "ms over interface " << ifName;

  neighbor.rtt = std::chrono::microseconds(newRtt);
  thrift::SparkNeighborEvent event(
      apache::thrift::FRAGILE,
      thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE,
      ifName,
      originator,
      neighbor.rtt.count(),
      neighbor.label);
  reportSocket_.sendThriftObj(event, serializer_);
}

void
Spark::processNeighborHoldTimeout(
    std::string const& ifName, std::string const& neighborName) {
  // Neighbor must exist if this hold-timeout callback is executed.
  auto& ifNeighbors = neighbors_.at(ifName);
  auto& neighbor = ifNeighbors.at(neighborName);

  // valid timeout event, remove neighbor from tracked and adjacent
  // lists and report downstream
  LOG(INFO) << "Neighbor " << neighborName << " expired on interface "
            << ifName;

  // remove from tracked neighbor at the end
  SCOPE_EXIT {
    allocatedLabels_.erase(neighbor.label);
    ifNeighbors.erase(neighborName);
  };

  // check if the neighbor was adjacent. if so, report it as DOWN
  if (neighbor.isAdjacent) {
    LOG(INFO) << "Neighbor " << neighborName
              << " was adjacent, reporting as DOWN";
    neighbor.isAdjacent = false;

    thrift::SparkNeighborEvent event(
        apache::thrift::FRAGILE,
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        ifName,
        neighbor.info,
        neighbor.rtt.count(),
        neighbor.label);
    reportSocket_.sendThriftObj(event, serializer_);
  } else {
    VLOG(2) << "Neighbor went down, but was not adjacent, not reporting";
  }
}

bool
Spark::shouldProcessHelloPacket(
    std::string const& ifName, folly::IPAddress const& addr) {
  size_t index = std::hash<std::tuple<std::string, folly::IPAddress>>{}(
                     std::make_tuple(ifName, addr)) %
      Constants::kNumTimeSeries;

  // check our timeseries to see if we want to process anymore right now
  timeSeriesVector_[index].update(std::chrono::steady_clock::now());
  if (timeSeriesVector_[index].count() > Constants::kMaxAllowedPps) {
    // drop the packet
    return false;
  }
  // otherwise, count this packet and process it
  timeSeriesVector_[index].addValue(std::chrono::steady_clock::now(), 1);
  return true;
}

void
Spark::processHelloPacket() {
  // the read buffer
  uint8_t buf[kMinIpv6Mtu];

  ssize_t bytesRead;
  int ifIndex;
  folly::SocketAddress clientAddr;
  int hopLimit;
  std::chrono::microseconds myRecvTime;

  std::tie(bytesRead, ifIndex, clientAddr, hopLimit, myRecvTime) =
      recvMessage(mcastFd_, buf, kMinIpv6Mtu, ioProvider_.get());

  if (hopLimit < kSparkHopLimit) {
    LOG(ERROR) << "Rejecting packet from " << clientAddr.getAddressStr()
               << " due to hop limit being " << hopLimit;
    return;
  }

  auto res = findInterfaceFromIfindex(ifIndex);
  if (!res) {
    LOG(WARNING) << "Received packet from " << clientAddr.getAddressStr()
                 << " on unknown interface with index " << ifIndex
                 << ". Ignoring the packet.";
    return;
  }
  const std::string ifName = *res;

  VLOG(4) << "Received message on " << ifName << " ifindex " << ifIndex
          << " from " << clientAddr.getAddressStr();

  // update counters for packets received, dropped and processed
  tData_.addStatValue("spark.hello_packet_recv", 1, fbzmq::SUM);

  if (!shouldProcessHelloPacket(ifName, clientAddr.getIPAddress())) {
    VLOG(3) << "Spark: dropping hello packet on iface: " << ifName
            << " from addr: " << clientAddr.getAddressStr();
    tData_.addStatValue("spark.hello_packet_dropped", 1, fbzmq::SUM);
    return;
  }

  tData_.addStatValue("spark.hello_packet_processed", 1, fbzmq::SUM);

  if (bytesRead >= 0) {
    VLOG(4) << "Read a total of " << bytesRead << " bytes from fd " << mcastFd_;

    if (static_cast<size_t>(bytesRead) > kMinIpv6Mtu) {
      LOG(ERROR) << "Message from " << clientAddr.getAddressStr()
                 << " has been truncated";
      return;
    }
  } else {
    LOG(ERROR) << "Failed reading from fd " << mcastFd_ << " error "
               << folly::errnoStr(errno);
    return;
  }

  // Copy buffer into string object and parse it into helloPacket.
  std::string readBuf(reinterpret_cast<const char*>(&buf[0]), bytesRead);
  thrift::SparkHelloPacket helloPacket;
  try {
    helloPacket =
        util::readThriftObjStr<thrift::SparkHelloPacket>(readBuf, serializer_);
  } catch (std::exception const& err) {
    LOG(ERROR) << "Failed parsing hello packet " << folly::exceptionStr(err);
    return;
  }

  auto validationResult = validateHelloPacket(ifName, helloPacket);
  if (validationResult == PacketValidationResult::FAILURE) {
    LOG(ERROR) << "Ignoring invalid packet received from "
               << helloPacket.payload.originator.nodeName << " on " << ifName;
    return;
  }

  // the map of adjacent neighbors should have been already created
  auto const& originator = helloPacket.payload.originator;
  auto& neighbor = neighbors_.at(ifName).at(originator.nodeName);
  bool isAdjacent = neighbor.isAdjacent;

  // Update timestamps for received hello packet for neighbor
  auto nbrSentTime = std::chrono::microseconds(helloPacket.payload.timestamp);
  neighbor.neighborTimestamp = nbrSentTime;
  neighbor.localTimestamp = myRecvTime;

  // Try to deduce RTT for this neighbor and update timestamps for recvd hello
  auto it = helloPacket.payload.neighborInfos.find(myNodeName_);
  if (it != helloPacket.payload.neighborInfos.end()) {
    auto& tstamps = it->second;
    auto mySentTime = std::chrono::microseconds(tstamps.lastNbrMsgSentTsInUs);
    auto nbrRecvTime = std::chrono::microseconds(tstamps.lastMyMsgRcvdTsInUs);
    auto myRecvTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(myRecvTime);

    VLOG(4) << "RTT timestamps in order: " << mySentTime.count() << ", "
            << nbrRecvTime.count() << ", " << nbrSentTime.count() << ", "
            << myRecvTime.count();

    // Measure only if neighbor is reflecting our previous hello packet.
    if (mySentTime.count() and nbrRecvTime.count()) {
      bool useRtt = true;
      if (nbrSentTime < nbrRecvTime) {
        useRtt = false;
        LOG(ERROR) << "Time anomaly. nbrSentTime: " << nbrSentTime.count()
                   << " < "
                   << " nbrRecvTime : " << nbrRecvTime.count();
      }
      if (myRecvTime < mySentTime) {
        useRtt = false;
        LOG(ERROR) << "Time anomaly. myRecvTime: " << myRecvTime.count()
                   << " < "
                   << " mySentTime : " << mySentTime.count();
      }
      if (useRtt) {
        auto rtt = (myRecvTime - mySentTime) - (nbrSentTime - nbrRecvTime);
        VLOG(3) << "Measured new RTT for neighbor " << originator.nodeName
                << " from iface " << originator.ifName << " over interface "
                << ifName << " as " << rtt.count() / 1000.0 << "ms.";
        // Mask off to millisecond accuracy!
        // Reason => Relying on microsecond accuracy is too inacurate. For
        // practical scenarios like Backbone network having accuracy upto
        // milliseconds is sufficient. Further load on system can heavily
        // infuence rtt at microseconds but not much at milliseconds and hence
        // when node comes back up measurement will more likely to be the same
        // as the previous one.
        rtt = std::max(rtt / 1000 * 1000, std::chrono::microseconds(1000));

        // It is possible for things to go wrong in RTT calculation because of
        // clock adjustment.
        // Next measurements will correct this wrong measurement.
        if (rtt.count() < 0) {
          LOG(ERROR) << "Time anomaly. Measured negative RTT. "
                     << rtt.count() / 1000.0 << "ms.";
        } else {
          // Add it to step detector
          neighbor.stepDetector.addValue(myRecvTimeMs, rtt.count());

          // Set initial value if empty
          if (!neighbor.rtt.count()) {
            VLOG(2) << "Setting initial value for RTT for neighbor "
                    << originator.nodeName;
            neighbor.rtt = rtt;
          }

          // Update rttLatest
          neighbor.rttLatest = rtt;
        }
      }
    }
  }

  //
  // At this point we have heard from the neighbor, but don't know if
  // the neighbor has heard from us. We check this, and also validate
  // that the seq# the neighbor has heard from us is correct
  //

  bool foundSelf{false};
  auto myIt = helloPacket.payload.neighborInfos.find(myNodeName_);
  if (myIt != helloPacket.payload.neighborInfos.end()) {
    // the seq# neighbor has seen from us could not be higher than ours if it
    // is, this normally means we have restarted, and seeing our previous
    // incarnation and we act like we haven't heard from the neighbor (wait
    // for it to catch with our hello packets).
    uint64_t seqNumSeen = static_cast<uint64_t>(myIt->second.seqNum);
    foundSelf = (seqNumSeen < mySeqNum_);

    if (not foundSelf) {
      VLOG(2) << "Seeing my previous incarnation in neighbor "
              << originator.nodeName
              << " hello packets. Seen Seq#: " << seqNumSeen
              << ", My Seq#: " << mySeqNum_;
    }
  } else {
    VLOG(2) << "Not seeing myself in neighbor hello packets.";
  }

  // if a neighbor is in fast initial state and does not see us yet,
  // then reply to this neighbor in fast frequency
  if (!foundSelf && helloPacket.payload.solicitResponse) {
    scheduleTimeout(std::chrono::milliseconds(0), [this, ifName]() noexcept {
      sendHelloPacket(ifName);
    });
  }

  if (isAdjacent &&
      validationResult == PacketValidationResult::NEIGHBOR_RESTART) {
    LOG(INFO) << "Adjacent neighbor " << originator.nodeName << " from iface "
              << originator.ifName << " on iface " << ifName
              << " is restarting, waiting for it to ack myself.";

    thrift::SparkNeighborEvent event(
        apache::thrift::FRAGILE,
        thrift::SparkNeighborEventType::NEIGHBOR_RESTART,
        ifName,
        originator,
        neighbor.rtt.count(),
        neighbor.label);
    reportSocket_.sendThriftObj(event, serializer_);
    return;
  }

  // NOTE: means we only use the data for neighbor from initial packet.
  // all the other messages serves as confirmation of hold time refersh.
  if (foundSelf && isAdjacent) {
    VLOG(3) << "Already adjacent neighbor " << originator.nodeName
            << " from iface " << originator.ifName << " on iface " << ifName
            << " confirms adjacency";

    // Reset the hold-timer for neighbor as we have received a keep-alive
    // message. Note that we are using hold-time sent by neighbor so neighbor
    // can reset it on the fly.
    neighbor.holdTimer->scheduleTimeout(
        std::chrono::milliseconds(originator.holdTime));

    return;
  }

  // Neighbor has not heard from us yet, and we don't see ourselves
  // in its hello packets
  if (!foundSelf && !isAdjacent) {
    LOG(INFO) << "Neighbor " << originator.nodeName << " on iface " << ifName
              << " from iface " << originator.ifName
              << " has not heard from us yet";
    return;
  }

  // add new adjacency in LinkMonitor once we have measured initial RTT
  if (foundSelf && !isAdjacent) {
    LOG(INFO) << "Added new adjacent neighbor " << originator.nodeName
              << " from iface " << originator.ifName << " on iface " << ifName;

    thrift::SparkNeighborEvent event(
        apache::thrift::FRAGILE,
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        ifName,
        originator,
        neighbor.rtt.count(),
        neighbor.label);
    reportSocket_.sendThriftObj(event, serializer_);
    neighbor.isAdjacent = true;

    // Start hold-timer
    neighbor.holdTimer->scheduleTimeout(
        std::chrono::milliseconds(originator.holdTime));

    return;
  }

  // If don't see ourselves in neighbor's hello then we should remove neighbor
  // if neighbor. This case can arise when adjacent node no longer want to peer
  // with us.
  if (!foundSelf && isAdjacent) {
    LOG(INFO) << "Removed adjacent neighbor " << originator.nodeName
              << " from iface " << originator.ifName << " on iface " << ifName
              << " since it no longer hears us.";

    thrift::SparkNeighborEvent event(
        apache::thrift::FRAGILE,
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        ifName,
        originator,
        neighbor.rtt.count(),
        neighbor.label);
    reportSocket_.sendThriftObj(event, serializer_);
    neighbor.isAdjacent = false;

    // Stop hold-timer
    neighbor.holdTimer->cancelTimeout();

    return;
  }
}

void
Spark::sendHelloPacket(std::string const& ifName, bool inFastInitState) {
  VLOG(3) << "Send hello packet called for " << ifName;

  if (interfaceDb_.count(ifName) == 0) {
    LOG(ERROR) << "Interface " << ifName << " is no longer being tracked";
    return;
  }

  SCOPE_EXIT {
    // increment seq# after packet has been sent (even if it didnt go out)
    ++mySeqNum_;
  };

  SCOPE_FAIL {
    LOG(ERROR) << "Failed sending Hello packet on " << ifName;
  };

  // in some cases, getting link-local address may fail and throw
  // e.g. when iface has not yet auto-configured it, or iface is removed but
  // down event has not arrived yet

  const auto& interfaceEntry = interfaceDb_.at(ifName);
  const auto ifIndex = interfaceEntry.ifIndex;
  const auto v4Addr = interfaceEntry.v4Network.first;
  const auto v6Addr = interfaceEntry.v6LinkLocalNetwork.first;
  thrift::OpenrVersion openrVer(kVersion_.version);

  thrift::SparkNeighbor myself(
      apache::thrift::FRAGILE,
      myDomainName_,
      myNodeName_,
      myHoldTime_.count(),
      "", /* DEPRECATED - public key */
      toBinaryAddress(v6Addr),
      toBinaryAddress(v4Addr),
      kKvStorePubPort_,
      kKvStoreCmdPort_,
      ifName);

  // create the hello packet payload
  auto payload = thrift::SparkPayload(
      apache::thrift::FRAGILE,
      openrVer,
      myself,
      mySeqNum_,
      std::map<std::string, thrift::ReflectedNeighborInfo>{},
      getCurrentTimeInUs().count(),
      inFastInitState);

  // add all neighbors we have heard from on this interface
  for (const auto& kv : neighbors_.at(ifName)) {
    std::string const& neighborName = kv.first;
    auto& neighbor = kv.second;

    // Add sequence number information.
    uint64_t seqNum = kv.second.seqNum;

    // Add timestamp and sequence number from last hello. Will be 0 if we
    // haven't heard before from the neighbor.
    // Refer to thrift for definition of timestampts.
    auto& neighborInfo = payload.neighborInfos[neighborName];
    neighborInfo.seqNum = seqNum;
    neighborInfo.lastNbrMsgSentTsInUs = neighbor.neighborTimestamp.count();
    neighborInfo.lastMyMsgRcvdTsInUs = neighbor.localTimestamp.count();
  }

  // build the hello packet from payload and empty signature
  auto packet = util::writeThriftObjStr(
      thrift::SparkHelloPacket{apache::thrift::FRAGILE, payload, ""},
      serializer_);

  // send the payload
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()),
      udpMcastPort_);

  if (kMinIpv6Mtu < packet.size()) {
    LOG(ERROR) << "Hello packet is too big, cannot sent!";
    return;
  }

  auto bytesSent = sendMessage(
      mcastFd_, ifIndex, v6Addr.asV6(), dstAddr, packet, ioProvider_.get());

  if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
    VLOG(1) << "Sending multicast to " << dstAddr.getAddressStr() << " on "
            << ifName << " failed due to error " << folly::errnoStr(errno);
    return;
  }

  tData_.addStatValue("spark.hello_packet_sent", 1, fbzmq::SUM);
  VLOG(4) << "Sent " << bytesSent << " bytes in hello packet";
}

void
Spark::processInterfaceDbUpdate() {
  SCOPE_SUCCESS {
    thrift::SparkIfDbUpdateResult result;
    result.isSuccess = true;
    cmdSocket_.sendThriftObj(result, serializer_);
  };

  SCOPE_FAIL {
    thrift::SparkIfDbUpdateResult result;
    result.isSuccess = false;
    cmdSocket_.sendThriftObj(result, serializer_);
  };

  auto maybeMsg = cmdSocket_.recvThriftObj<thrift::InterfaceDatabase>(
      serializer_, Constants::kReadTimeout);
  if (maybeMsg.hasError()) {
    LOG(ERROR) << "processInterfaceDbUpdate recv failed: " << maybeMsg.error();
    return;
  }
  auto ifDb = maybeMsg.value();

  decltype(interfaceDb_) newInterfaceDb{};

  CHECK_EQ(ifDb.thisNodeName, myNodeName_)
      << "Node name in ifDb " << ifDb.thisNodeName
      << " does not match my node name " << myNodeName_;

  //
  // To be conisdered a valid interface for Spark to track, it must:
  // - be up
  // - have a v6LinkLocal IP
  // - have an IPv4 addr when v4 is enabled
  //
  for (const auto& kv : ifDb.interfaces) {
    const auto& ifName = kv.first;
    const auto isUp = kv.second.isUp;
    const auto& ifIndex = kv.second.ifIndex;
    const auto& networks = kv.second.networks;

    std::vector<folly::CIDRNetwork> v4Networks;
    std::vector<folly::CIDRNetwork> v6LinkLocalNetworks;
    for (const auto& ntwk : networks) {
      const auto& ipNetwork = toIPNetwork(ntwk, false);
      if (ipNetwork.first.isV4()) {
        v4Networks.emplace_back(ipNetwork);
      }
      else if (ipNetwork.first.isV6() && ipNetwork.first.isLinkLocal()) {
        v6LinkLocalNetworks.emplace_back(ipNetwork);
      }
    }

    if (!isUp) {
      continue;
    }
    if (v6LinkLocalNetworks.empty()) {
      VLOG(2) << "IPv6 link local address not found";
      continue;
    }
    if (enableV4_ && v4Networks.empty()) {
      VLOG(2) << "IPv4 enabled but no IPv4 addresses are configured";
      continue;
    }

    // We have a valid entry
    // Obtain v4 address if enabled, else default
    folly::CIDRNetwork v4Network{folly::IPAddress("0.0.0.0"), 32};
    if (enableV4_) {
      CHECK(v4Networks.size());
      v4Network = v4Networks.front();
    }
    folly::CIDRNetwork v6LinkLocalNetwork = v6LinkLocalNetworks.front();

    newInterfaceDb.emplace(ifName,
      Interface(ifIndex, v4Network, v6LinkLocalNetwork));
  }

  auto newIfaces = folly::gen::from(newInterfaceDb) | folly::gen::get<0>() |
      folly::gen::as<std::set<std::string>>();

  auto existingIfaces = folly::gen::from(interfaceDb_) | folly::gen::get<0>() |
      folly::gen::as<std::set<std::string>>();

  std::set<std::string> toAdd;
  std::set<std::string> toDel;
  std::set<std::string> toUpdate;

  std::set_difference(
      newIfaces.begin(),
      newIfaces.end(),
      existingIfaces.begin(),
      existingIfaces.end(),
      std::inserter(toAdd, toAdd.begin()));

  std::set_difference(
      existingIfaces.begin(),
      existingIfaces.end(),
      newIfaces.begin(),
      newIfaces.end(),
      std::inserter(toDel, toDel.begin()));

  std::set_intersection(
      newIfaces.begin(),
      newIfaces.end(),
      existingIfaces.begin(),
      existingIfaces.end(),
      std::inserter(toUpdate, toUpdate.begin()));
  //
  // remove the interfaces no longer in newdb
  //
  for (const auto& ifName : toDel) {
    LOG(INFO) << "Removing " << ifName << " from Spark. "
              << "It is down, declaring all neighbors down";

    for (const auto& kv : neighbors_.at(ifName)) {
      auto& neighborName = kv.first;
      auto& neighbor = kv.second;

      allocatedLabels_.erase(neighbor.label);
      if (!neighbor.isAdjacent) {
        continue;
      }
      LOG(INFO) << "Neighbor " << neighborName << " removed due to iface "
                << ifName << " down";

      thrift::SparkNeighborEvent event(
          apache::thrift::FRAGILE,
          thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
          ifName,
          neighbor.info,
          neighbor.rtt.count(),
          neighbor.label);
      reportSocket_.sendThriftObj(event, serializer_);
    }

    // unsubscribe the socket from mcast group on this interface
    // On error, log and continue
    if (!toggleMcastGroup(
            mcastFd_,
            folly::IPAddress(Constants::kSparkMcastAddr.toString()),
            interfaceDb_.at(ifName).ifIndex,
            false /* leave */,
            ioProvider_.get())) {
      LOG(ERROR) << folly::sformat(
          "Failed leaving multicast group: {}", folly::errnoStr(errno));
    }
    // cleanup for this interface
    neighbors_.erase(ifName);
    ifNameToHelloTimers_.erase(ifName);
    interfaceDb_.erase(ifName);
  }

  //
  // Adding interfaces
  //
  for (const auto& ifName : toAdd) {
    auto newInterface = newInterfaceDb.at(ifName);
    auto ifIndex = newInterface.ifIndex;
    CHECK_NE(ifIndex, 0) << "Cound not get ifIndex for Iface " << ifName;
    LOG(INFO) << "Adding iface " << ifName << " for tracking with ifindex "
              << ifIndex;

    // subscribe the socket to mcast address on this interface
    // We throw an error on the first one to encounter a problem
    if (!toggleMcastGroup(
            mcastFd_,
            folly::IPAddress(Constants::kSparkMcastAddr.toString()),
            ifIndex,
            true /* join */,
            ioProvider_.get())) {
      throw std::runtime_error(folly::sformat(
          "Failed joining multicast group: {}", folly::errnoStr(errno)));
    }

    {
      auto result = interfaceDb_.emplace(ifName, newInterface);
      CHECK(result.second);
    }

    {
      // create place-holders for newly added interface
      auto result = neighbors_.emplace(
          ifName, std::unordered_map<std::string, Neighbor>{});
      CHECK(result.second);
    }

    auto rollHelper = [](std::chrono::milliseconds timeDuration) {
      auto base = timeDuration.count();
      std::uniform_int_distribution<int> distribution(-0.2 * base, 0.2 * base);
      std::default_random_engine generator;
      return [timeDuration, distribution, generator]() mutable {
        return timeDuration +
            std::chrono::milliseconds(distribution(generator));
      };
    };

    auto roll = rollHelper(myKeepAliveTime_);
    auto rollFast = rollHelper(fastInitKeepAliveTime_);
    auto timePoint = std::chrono::steady_clock::now();

    // NOTE: we do not send hello packet immediately after adding new interface
    // this is due to the fact that it may not have yet configured a link-local
    // address. The hello packet will be sent later and will have good chances
    // of making it out. This is not a great solution, but it work reasonably
    // well, and we don't really care to detect neighbor up too fast
    auto helloTimer = fbzmq::ZmqTimeout::make(
        this, [this, ifName, timePoint, roll, rollFast]() mutable noexcept {
          VLOG(3) << "Sending hello multicast packet on interface " << ifName;
          // turn off my own fast init state after myKeepAliveTime_ from
          // starting yet it is still possible that my neighbor is in fast
          // init state and I need to react to it.
          bool inFastInitState = (std::chrono::steady_clock::now() -
                                  timePoint) <= myKeepAliveTime_;
          sendHelloPacket(ifName, inFastInitState);

          // Schedule next run (add 20% variance)
          // overriding timeoutPeriod if I am in fast initial state
          std::chrono::milliseconds timeoutPeriod =
              inFastInitState ? rollFast() : roll();

          ifNameToHelloTimers_.at(ifName)->scheduleTimeout(timeoutPeriod);
        });

    // should be in fast init state when the node just starts
    helloTimer->scheduleTimeout(fastInitKeepAliveTime_);
    ifNameToHelloTimers_[ifName] = std::move(helloTimer);
  }

  //
  // Updating interface. If ifindex changes, we need to unsubscribe old ifindex
  // from mcast and subscribe new one
  //
  for (const auto& ifName : toUpdate) {
    auto& interface = interfaceDb_.at(ifName);
    auto& newInterface = newInterfaceDb.at(ifName);

    if (interface == newInterface) {
      LOG(INFO) << "No update to iface " << ifName << " in spark tracking";
      continue;
    }

    // in case ifindex changes w/o interface down event followed by up event
    // this can occur if platform/netlink agent is down
    if (newInterface.ifIndex != interface.ifIndex) {
      // unsubscribe the socket from mcast group on the old ifindex
      // On error, log and continue
      if (!toggleMcastGroup(
              mcastFd_,
              folly::IPAddress(Constants::kSparkMcastAddr.toString()),
              interface.ifIndex,
              false /* leave */,
              ioProvider_.get())) {
        LOG(WARNING) << folly::sformat(
            "Failed leaving multicast group: {}", folly::errnoStr(errno));
      }

      // subscribe the socket to mcast address on the new ifindex
      // We throw an error on the first one to encounter a problem
      if (!toggleMcastGroup(
              mcastFd_,
              folly::IPAddress(Constants::kSparkMcastAddr.toString()),
              newInterface.ifIndex,
              true /* join */,
              ioProvider_.get())) {
        throw std::runtime_error(folly::sformat(
            "Failed joining multicast group: {}", folly::errnoStr(errno)));
      }
    }
    LOG(INFO)
        << "Updating iface " << ifName << " in spark tracking from "
        << "(ifindex " << interface.ifIndex << ", addrs "
        << interface.v6LinkLocalNetwork.first << " , "
        << interface.v4Network.first << ") to "
        << "(ifindex " << newInterface.ifIndex << ", addrs "
        << newInterface.v6LinkLocalNetwork.first << " , "
        << newInterface.v4Network.first << ")";

    interface = std::move(newInterface);
  }
}

folly::Optional<std::string>
Spark::findInterfaceFromIfindex(int ifIndex) {
  for (const auto& kv : interfaceDb_) {
    if (kv.second.ifIndex == ifIndex) {
      return kv.first;
    }
  }
  return folly::none;
}

int32_t
Spark::getNewLabelForIface(const std::string& ifName) {
  // interface must exists. We try to first assign label based on ifIndex if
  // not already taken.
  int32_t label =
      Constants::kSrLocalRange.first + interfaceDb_.at(ifName).ifIndex;
  if (allocatedLabels_.insert(label).second) { // new value inserted
    return label;
  }

  // Label already exists let's try to find out a new one from the back
  label = Constants::kSrLocalRange.second; // last possible one
  while (!allocatedLabels_.insert(label).second) { // value already exists
    label--;
  }

  if (label < Constants::kSrLocalRange.first) {
    throw std::runtime_error("Ran out of local label allocation space.");
  }

  return label;
}

void
Spark::submitCounters() {
  VLOG(3) << "Submitting counters...";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  int64_t adjacentNeighborCount{0}, trackedNeighborCount{0};
  for (auto const& ifaceNeighbors : neighbors_) {
    trackedNeighborCount += ifaceNeighbors.second.size();
    for (auto const& kv : ifaceNeighbors.second) {
      auto const& neighbor = kv.second;
      adjacentNeighborCount += neighbor.isAdjacent;
      counters
          ["spark.rtt_us." + neighbor.info.nodeName + "." +
           ifaceNeighbors.first] = neighbor.rtt.count();
      counters["spark.rtt_latest_us." + neighbor.info.nodeName] =
          neighbor.rttLatest.count();
      counters["spark.seq_num." + neighbor.info.nodeName] = neighbor.seqNum;
    }
  }
  counters["spark.num_tracked_interfaces"] = neighbors_.size();
  counters["spark.num_tracked_neighbors"] = trackedNeighborCount;
  counters["spark.num_adjacent_neighbors"] = adjacentNeighborCount;
  counters["spark.my_seq_num"] = mySeqNum_;
  counters["spark.pending_timers"] = getNumPendingTimeouts();

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

} // namespace openr
