/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <fb303/ServiceData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/GLog.h>
#include <folly/IPAddress.h>
#include <folly/MapUtil.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/spark/Spark.h>

namespace fb303 = facebook::fb303;

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

// number of restarting packets to send out per interface before I'm going down
const int kNumRestartingPktSent = 3;

//
// Function to get current timestamp in microseconds using steady clock
// NOTE: we use non-monotonic clock since kernel time-stamps do not support
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

  // Join multicast group on interface
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

  // Leave multicast group on interface
  if (ioProvider->setsockopt(
          fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq)) != 0) {
    LOG(ERROR) << "setsockopt ipv6_leave_group failed "
               << folly::errnoStr(errno);
    return false;
  }

  LOG(INFO) << "Left multicast addr " << mcastGroup.str() << " on ifindex "
            << ifIndex;
  return true;
}

} // namespace

namespace openr {

const std::vector<std::vector<std::optional<SparkNeighState>>>
    Spark::stateMap_ = {
        /*
         * index 0 - IDLE
         * HELLO_RCVD_INFO => WARM; HELLO_RCVD_NO_INFO => WARM
         */
        {SparkNeighState::WARM,
         SparkNeighState::WARM,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt},
        /*
         * index 1 - WARM
         * HELLO_RCVD_INFO => NEGOTIATE;
         */
        {SparkNeighState::NEGOTIATE,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt},
        /*
         * index 2 - NEGOTIATE
         * HANDSHAKE_RCVD => ESTABLISHED; NEGOTIATE_TIMER_EXPIRE => WARM;
         * NEGOTIATION_FAILURE => WARM;
         */
        {std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         SparkNeighState::ESTABLISHED,
         std::nullopt,
         SparkNeighState::WARM,
         std::nullopt,
         SparkNeighState::WARM},
        /*
         * index 3 - ESTABLISHED
         * HELLO_RCVD_NO_INFO => IDLE; HELLO_RCVD_RESTART => RESTART;
         * HEARTBEAT_RCVD => ESTABLISHED; HEARTBEAT_TIMER_EXPIRE => IDLE;
         */
        {std::nullopt,
         SparkNeighState::IDLE,
         SparkNeighState::RESTART,
         SparkNeighState::ESTABLISHED,
         std::nullopt,
         SparkNeighState::IDLE,
         std::nullopt,
         std::nullopt,
         std::nullopt},
        /*
         * index 4 - RESTART
         * HELLO_RCVD_INFO => ESTABLISHED; GR_TIMER_EXPIRE => IDLE
         */
        {SparkNeighState::ESTABLISHED,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         SparkNeighState::IDLE,
         std::nullopt}};

SparkNeighState
Spark::getNextState(
    std::optional<SparkNeighState> const& currState,
    SparkNeighEvent const& event) {
  CHECK(currState.has_value()) << "Current state is 'UNEXPECTED'";

  std::optional<SparkNeighState> nextState =
      stateMap_[static_cast<uint32_t>(currState.value())]
               [static_cast<uint32_t>(event)];

  CHECK(nextState.has_value()) << "Next state is 'UNEXPECTED'";
  return nextState.value();
}

Spark::SparkNeighbor::SparkNeighbor(
    const thrift::StepDetectorConfig& stepDetectorConfig,
    std::string const& domainName,
    std::string const& nodeName,
    std::string const& remoteIfName,
    uint32_t label,
    uint64_t seqNum,
    const std::chrono::milliseconds& samplingPeriod,
    std::function<void(const int64_t&)> rttChangeCb,
    const std::string& adjArea)
    : domainName(domainName),
      nodeName(nodeName),
      remoteIfName(remoteIfName),
      label(label),
      seqNum(seqNum),
      state(SparkNeighState::IDLE),
      stepDetector(
          stepDetectorConfig /* step detector config */,
          samplingPeriod /* sampling period */,
          rttChangeCb /* callback function */),
      area(adjArea) {
  CHECK(!(this->domainName.empty()));
  CHECK(!(this->nodeName.empty()));
  CHECK(!(this->remoteIfName.empty()));
}

Spark::Spark(
    std::optional<int> maybeIpTos,
    messaging::RQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue,
    messaging::ReplicateQueue<thrift::SparkNeighborEvent>& neighborUpdatesQueue,
    KvStoreCmdPort kvStoreCmdPort,
    OpenrCtrlThriftPort openrCtrlThriftPort,
    std::shared_ptr<IoProvider> ioProvider,
    std::shared_ptr<const Config> config,
    std::pair<uint32_t, uint32_t> version,
    std::optional<uint32_t> maybeMaxAllowedPps)
    : myDomainName_(*config->getConfig().domain_ref()),
      myNodeName_(config->getNodeName()),
      neighborDiscoveryPort_(static_cast<uint16_t>(
          *config->getSparkConfig().neighbor_discovery_port_ref())),
      helloTime_(
          std::chrono::seconds(*config->getSparkConfig().hello_time_s_ref())),
      fastInitHelloTime_(std::chrono::milliseconds(
          *config->getSparkConfig().fastinit_hello_time_ms_ref())),
      handshakeTime_(std::chrono::milliseconds(
          *config->getSparkConfig().fastinit_hello_time_ms_ref())),
      keepAliveTime_(std::chrono::seconds(
          *config->getSparkConfig().keepalive_time_s_ref())),
      handshakeHoldTime_(std::chrono::seconds(
          *config->getSparkConfig().keepalive_time_s_ref())),
      holdTime_(
          std::chrono::seconds(*config->getSparkConfig().hold_time_s_ref())),
      gracefulRestartTime_(std::chrono::seconds(
          *config->getSparkConfig().graceful_restart_time_s_ref())),
      enableV4_(config->isV4Enabled()),
      neighborUpdatesQueue_(neighborUpdatesQueue),
      kKvStoreCmdPort_(kvStoreCmdPort),
      kOpenrCtrlThriftPort_(openrCtrlThriftPort),
      kVersion_(apache::thrift::FRAGILE, version.first, version.second),
      enableFloodOptimization_(config->isFloodOptimizationEnabled()),
      ioProvider_(std::move(ioProvider)),
      config_(std::move(config)) {
  CHECK(gracefulRestartTime_ >= 3 * keepAliveTime_)
      << "Keep-alive-time must be less than hold-time.";
  CHECK(keepAliveTime_ > std::chrono::milliseconds(0))
      << "heartbeatMsg interval can't be 0";
  CHECK(helloTime_ > std::chrono::milliseconds(0))
      << "helloMsg interval can't be 0";
  CHECK(fastInitHelloTime_ > std::chrono::milliseconds(0))
      << "fastInit helloMsg interval can't be 0";
  CHECK(fastInitHelloTime_ <= helloTime_)
      << "fastInit helloMsg interval must be smaller than normal interval";
  CHECK(ioProvider_) << "Got null IoProvider";

  // Initialize list of BucketedTimeSeries
  const std::chrono::seconds sec{1};
  if (maybeMaxAllowedPps) {
    maybeMaxAllowedPps_ = maybeMaxAllowedPps;
    const int32_t numBuckets = *maybeMaxAllowedPps_ / 3;
    for (size_t i = 0; i < Constants::kNumTimeSeries; i++) {
      timeSeriesVector_.emplace_back(
          folly::BucketedTimeSeries<int64_t, std::chrono::steady_clock>(
              numBuckets, sec));
    }
  }

  // Fiber to process interface updates from LinkMonitor
  addFiberTask([q = std::move(interfaceUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto interfaceUpdates = q.get(); // perform read
      VLOG(1) << "Received interface updates";
      if (interfaceUpdates.hasError()) {
        LOG(INFO) << "Terminating interface update processing fiber";
        break;
      }

      processInterfaceUpdates(std::move(interfaceUpdates).value());
    }
  });

  // Initialize UDP socket for neighbor discovery
  prepareSocket(maybeIpTos);

  // Initialize some stat keys
  fb303::fbData->addStatExportType(
      "spark.invalid_keepalive.different_domain", fb303::SUM);
  fb303::fbData->addStatExportType(
      "spark.invalid_keepalive.invalid_version", fb303::SUM);
  fb303::fbData->addStatExportType(
      "spark.invalid_keepalive.missing_v4_addr", fb303::SUM);
  fb303::fbData->addStatExportType(
      "spark.invalid_keepalive.different_subnet", fb303::SUM);
  fb303::fbData->addStatExportType(
      "spark.invalid_keepalive.looped_packet", fb303::SUM);
  fb303::fbData->addStatExportType(
      "slo.neighbor_discovery.time_ms", fb303::AVG);
  fb303::fbData->addStatExportType("slo.neighbor_restart.time_ms", fb303::AVG);
}

// static util function to transform state into str
std::string
Spark::toStr(SparkNeighState state) {
  std::string res = "UNKNOWN";
  switch (state) {
  case SparkNeighState::IDLE:
    return "IDLE";
  case SparkNeighState::WARM:
    return "WARM";
  case SparkNeighState::NEGOTIATE:
    return "NEGOTIATE";
  case SparkNeighState::ESTABLISHED:
    return "ESTABLISHED";
  case SparkNeighState::RESTART:
    return "RESTART";
  default:
    LOG(ERROR) << "Unknown type";
  }
  return res;
}

void
Spark::stop() {
  // send out restarting packets for all interfaces before I'm going down
  // here we are sending duplicate restarting packets (3 times per interface)
  // in case some packets get lost
  for (int i = 0; i < kNumRestartingPktSent; ++i) {
    for (const auto& kv : interfaceDb_) {
      const auto& ifName = kv.first;
      sendHelloMsg(ifName, false /* inFastInitState */, true /* restarting */);
    }
  }

  LOG(INFO)
      << "I have sent all restarting packets to my neighbors, ready to go down";
  OpenrEventBase::stop();
}

void
Spark::prepareSocket(std::optional<int> maybeIpTos) noexcept {
  int fd = ioProvider_->socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  mcastFd_ = fd;
  LOG(INFO) << "Creatd UDP socket for neighbor discovery. fd: " << mcastFd_;
  CHECK_GT(fd, 0);

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

  // bind the socket to receive any mcast packet
  {
    VLOG(2) << "Binding UDP socket to receive on any destination address";

    auto mcastSockAddr =
        folly::SocketAddress(folly::IPAddress("::"), neighborDiscoveryPort_);

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

  // Listen for incoming messages on multicast FD
  addSocketFd(mcastFd_, ZMQ_POLLIN, [this](int) noexcept {
    try {
      processPacket();
    } catch (std::exception const& err) {
      LOG(ERROR) << "Spark: error processing hello packet "
                 << folly::exceptionStr(err);
    }
  });

  // update counters every few seconds
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    updateGlobalCounters();
    // Schedule next counters update
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
}

PacketValidationResult
Spark::sanityCheckHelloPkt(
    std::string const& domainName,
    std::string const& neighborName,
    std::string const& remoteIfName,
    uint32_t const& remoteVersion) {
  // check if own packet has looped
  if (neighborName == myNodeName_) {
    VLOG(2) << "Ignore packet from self (" << myNodeName_ << ")";
    fb303::fbData->addStatValue(
        "spark.invalid_keepalive.looped_packet", 1, fb303::SUM);
    return PacketValidationResult::SKIP_LOOPED_SELF;
  }
  // domain check
  if (domainName != myDomainName_) {
    LOG(ERROR) << "Ignoring hello packet from node " << neighborName
               << " on interface " << remoteIfName
               << " because it's from different domain " << domainName
               << ". My domain is " << myDomainName_;
    fb303::fbData->addStatValue(
        "spark.invalid_keepalive.different_domain", 1, fb303::SUM);
    return PacketValidationResult::FAILURE;
  }
  // version check
  if (remoteVersion <
      static_cast<uint32_t>(*kVersion_.lowestSupportedVersion_ref())) {
    LOG(ERROR) << "Unsupported version: " << neighborName << " "
               << remoteVersion
               << ", must be >= " << *kVersion_.lowestSupportedVersion_ref();
    fb303::fbData->addStatValue(
        "spark.invalid_keepalive.invalid_version", 1, fb303::SUM);
    return PacketValidationResult::FAILURE;
  }
  return PacketValidationResult::SUCCESS;
}

bool
Spark::shouldProcessHelloPacket(
    std::string const& ifName, folly::IPAddress const& addr) {
  if (not maybeMaxAllowedPps_.has_value()) {
    return true; // no rate limit
  }

  size_t index = std::hash<std::tuple<std::string, folly::IPAddress>>{}(
                     std::make_tuple(ifName, addr)) %
      Constants::kNumTimeSeries;
  // check our timeseries to see if we want to process anymore right now
  timeSeriesVector_[index].update(std::chrono::steady_clock::now());

  if (timeSeriesVector_[index].count() > *maybeMaxAllowedPps_) {
    // drop the packet
    return false;
  }
  // otherwise, count this packet and process it
  timeSeriesVector_[index].addValue(std::chrono::steady_clock::now(), 1);

  return true;
}

bool
Spark::parsePacket(
    thrift::SparkHelloPacket& pkt,
    std::string& ifName,
    std::chrono::microseconds& recvTime) {
  // the read buffer
  uint8_t buf[kMinIpv6Mtu];

  ssize_t bytesRead;
  int ifIndex;
  folly::SocketAddress clientAddr;
  int hopLimit;

  std::tie(bytesRead, ifIndex, clientAddr, hopLimit, recvTime) =
      IoProvider::recvMessage(mcastFd_, buf, kMinIpv6Mtu, ioProvider_.get());

  if (hopLimit < kSparkHopLimit) {
    LOG(ERROR) << "Rejecting packet from " << clientAddr.getAddressStr()
               << " due to hop limit being " << hopLimit;
    return false;
  }

  auto res = findInterfaceFromIfindex(ifIndex);
  if (!res.has_value()) {
    LOG(ERROR) << "Received packet from " << clientAddr.getAddressStr()
               << " on unknown interface with index " << ifIndex
               << ". Ignoring the packet.";
    return false;
  }

  ifName = res.value();

  VLOG(4) << "Received message on " << ifName << " ifindex " << ifIndex
          << " from " << clientAddr.getAddressStr();

  // update counters for packets received, dropped and processed
  fb303::fbData->addStatValue("spark.hello_packet_recv", 1, fb303::SUM);

  // update counters for total size of packets received
  fb303::fbData->addStatValue(
      "spark.hello_packet_recv_size", bytesRead, fb303::SUM);

  if (!shouldProcessHelloPacket(ifName, clientAddr.getIPAddress())) {
    LOG(ERROR) << "Spark: dropping hello packet due to rate limiting on iface: "
               << ifName << " from addr: " << clientAddr.getAddressStr();
    fb303::fbData->addStatValue("spark.hello_packet_dropped", 1, fb303::SUM);
    return false;
  }

  fb303::fbData->addStatValue("spark.hello_packet_processed", 1, fb303::SUM);

  if (bytesRead >= 0) {
    VLOG(4) << "Read a total of " << bytesRead << " bytes from fd " << mcastFd_;

    if (static_cast<size_t>(bytesRead) > kMinIpv6Mtu) {
      LOG(ERROR) << "Message from " << clientAddr.getAddressStr()
                 << " has been truncated";
      return false;
    }
  } else {
    LOG(ERROR) << "Failed reading from fd " << mcastFd_ << " error "
               << folly::errnoStr(errno);
    return false;
  }

  // Copy buffer into string object and parse it into helloPacket.
  std::string readBuf(reinterpret_cast<const char*>(&buf[0]), bytesRead);
  try {
    pkt = fbzmq::util::readThriftObjStr<thrift::SparkHelloPacket>(
        readBuf, serializer_);
  } catch (std::exception const& err) {
    LOG(ERROR) << "Failed parsing hello packet " << folly::exceptionStr(err);
    if (isThrowParserErrorsOn_) {
      throw;
    }
    return false;
  }
  return true;
}

PacketValidationResult
Spark::validateV4AddressSubnet(
    std::string const& ifName, thrift::BinaryAddress neighV4Addr) {
  // validate v4 address subnet
  // make sure v4 address is already specified on neighbor
  auto const& myV4Network = interfaceDb_.at(ifName).v4Network;
  auto const& myV4Addr = myV4Network.first;
  auto const& myV4PrefixLen = myV4Network.second;

  try {
    toIPAddress(neighV4Addr);
  } catch (const folly::IPAddressFormatException& ex) {
    LOG(ERROR) << "Neighbor V4 address is not known";
    fb303::fbData->addStatValue(
        "spark.invalid_keepalive.missing_v4_addr", 1, fb303::SUM);
    return PacketValidationResult::FAILURE;
  }

  // validate subnet of v4 address
  auto const& neighCidrNetwork =
      folly::sformat("{}/{}", toString(neighV4Addr), myV4PrefixLen);

  if (!myV4Addr.inSubnet(neighCidrNetwork)) {
    LOG(ERROR) << "Neighbor V4 address " << toString(neighV4Addr)
               << " is not in the same subnet with local V4 address "
               << myV4Addr.str() << "/" << +myV4PrefixLen;
    fb303::fbData->addStatValue(
        "spark.invalid_keepalive.different_subnet", 1, fb303::SUM);
    return PacketValidationResult::FAILURE;
  }
  return PacketValidationResult::SUCCESS;
}

void
Spark::processRttChange(
    std::string const& ifName,
    std::string const& neighborName,
    int64_t const newRtt) {
  // Neighbor must exist if this callback is fired
  auto& sparkNeighbor = sparkNeighbors_.at(ifName).at(neighborName);

  // only report RTT change if the neighbor is adjacent
  if (sparkNeighbor.state != SparkNeighState::ESTABLISHED) {
    VLOG(2) << "Neighbor: " << neighborName << " over iface: " << ifName
            << " is in state: " << toStr(sparkNeighbor.state)
            << ". Skip RTT change notification.";
    return;
  }

  LOG(INFO) << "RTT for sparkNeighbor " << neighborName << " has changed "
            << "from " << sparkNeighbor.rtt.count() << "usecs to " << newRtt
            << "usecs over interface " << ifName;

  sparkNeighbor.rtt = std::chrono::microseconds(newRtt);
  notifySparkNeighborEvent(
      thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE,
      ifName,
      sparkNeighbor.toThrift(),
      sparkNeighbor.rtt.count(),
      sparkNeighbor.label,
      false);
}

void
Spark::updateNeighborRtt(
    std::chrono::microseconds const& myRecvTime,
    std::chrono::microseconds const& mySentTime,
    std::chrono::microseconds const& nbrRecvTime,
    std::chrono::microseconds const& nbrSentTime,
    std::string const& neighborName,
    std::string const& remoteIfName,
    std::string const& ifName) {
  VLOG(4) << "RTT timestamps in order: " << mySentTime.count() << ", "
          << nbrRecvTime.count() << ", " << nbrSentTime.count() << ", "
          << myRecvTime.count();

  if (!mySentTime.count() || !nbrRecvTime.count()) {
    LOG(ERROR) << "Missing timestamp to deduce RTT";
    return;
  }

  if (nbrSentTime < nbrRecvTime) {
    LOG(ERROR) << "Time anomaly. nbrSentTime: [" << nbrSentTime.count()
               << "] < nbrRecvTime: [" << nbrRecvTime.count() << "]";
    return;
  }

  if (myRecvTime < mySentTime) {
    LOG(ERROR) << "Time anomaly. myRecvTime: [" << myRecvTime.count()
               << "] < mySentTime: [" << mySentTime.count() << "]";
    return;
  }

  // Measure only if neighbor is reflecting our previous hello packet.
  auto rtt = (myRecvTime - mySentTime) - (nbrSentTime - nbrRecvTime);
  VLOG(3) << "Measured new RTT for neighbor " << neighborName
          << " from remote iface " << remoteIfName << " over interface "
          << ifName << " as " << rtt.count() / 1000.0 << "ms.";
  // Mask off to millisecond accuracy!
  //
  // Reason => Relying on microsecond accuracy is too inaccurate. For
  // practical Wide Area Networks(WAN) scenario. Having accuracy up to
  // milliseconds is sufficient.
  //
  // Further, load on system can heavily influence rtt measurement in
  // microseconds as we do calculation in user-space. Also when Open/R
  // process restarts on neighbor node, measurement will more likely
  // to be the same as previous one.
  rtt = std::max(rtt / 1000 * 1000, std::chrono::microseconds(1000));

  // It is possible for things to go wrong in RTT calculation because of
  // clock adjustment.
  // Next measurements will correct this wrong measurement.
  if (rtt.count() < 0) {
    LOG(ERROR) << "Time anomaly. Measured negative RTT. "
               << rtt.count() / 1000.0 << "ms.";
    return;
  }

  // for Spark stepDetector usage
  if (sparkNeighbors_.find(ifName) != sparkNeighbors_.end()) {
    auto& sparkIfNeighbors = sparkNeighbors_.at(ifName);
    auto sparkNeighborIt = sparkIfNeighbors.find(neighborName);
    if (sparkNeighborIt != sparkIfNeighbors.end()) {
      auto& sparkNeighbor = sparkNeighborIt->second;

      // Add it to step detector
      sparkNeighbor.stepDetector.addValue(
          std::chrono::duration_cast<std::chrono::milliseconds>(myRecvTime),
          rtt.count());
      // Set initial value if empty
      if (!sparkNeighbor.rtt.count()) {
        VLOG(2) << "Setting initial value for RTT for sparkNeighbor "
                << neighborName;
        sparkNeighbor.rtt = rtt;
      }
      // Update rttLatest
      sparkNeighbor.rttLatest = rtt;
    }
  }
}

void
Spark::sendHandshakeMsg(
    std::string const& ifName,
    std::string const& neighborName,
    std::string const& neighborAreaId,
    bool isAdjEstablished) {
  SCOPE_FAIL {
    LOG(ERROR) << "Failed sending Handshake packet on " << ifName;
  };

  // in some cases, getting link-local address may fail and throw
  // e.g. when iface has not yet auto-configured it, or iface is removed but
  // down event has not arrived yet
  const auto& interfaceEntry = interfaceDb_.at(ifName);
  const auto ifIndex = interfaceEntry.ifIndex;
  const auto v4Addr = interfaceEntry.v4Network.first;
  const auto v6Addr = interfaceEntry.v6LinkLocalNetwork.first;

  // build handshake msg
  thrift::SparkHandshakeMsg handshakeMsg;
  *handshakeMsg.nodeName_ref() = myNodeName_;
  handshakeMsg.isAdjEstablished_ref() = isAdjEstablished;
  handshakeMsg.holdTime_ref() = holdTime_.count();
  handshakeMsg.gracefulRestartTime_ref() = gracefulRestartTime_.count();
  *handshakeMsg.transportAddressV6_ref() = toBinaryAddress(v6Addr);
  *handshakeMsg.transportAddressV4_ref() = toBinaryAddress(v4Addr);
  handshakeMsg.openrCtrlThriftPort_ref() = kOpenrCtrlThriftPort_;
  handshakeMsg.kvStoreCmdPort_ref() = kKvStoreCmdPort_;
  *handshakeMsg.area_ref() =
      neighborAreaId; // send neighborAreaId deduced locally
  handshakeMsg.neighborNodeName_ref() = neighborName;

  thrift::SparkHelloPacket pkt;
  pkt.handshakeMsg_ref() = std::move(handshakeMsg);

  auto packet = fbzmq::util::writeThriftObjStr(pkt, serializer_);

  // send the pkt
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()),
      neighborDiscoveryPort_);

  if (kMinIpv6Mtu < packet.size()) {
    LOG(ERROR) << "Handshake packet is too big, can't send it out.";
    return;
  }

  auto bytesSent = IoProvider::sendMessage(
      mcastFd_, ifIndex, v6Addr.asV6(), dstAddr, packet, ioProvider_.get());

  if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
    VLOG(1) << "Sending multicast to " << dstAddr.getAddressStr() << " on "
            << ifName << " failed due to error " << folly::errnoStr(errno);
    return;
  }

  // update counters for number of pkts and total size of pkts sent
  fb303::fbData->addStatValue(
      "spark.handshake.bytes_sent", packet.size(), fb303::SUM);
  fb303::fbData->addStatValue("spark.handshake.packets_sent", 1, fb303::SUM);
}

void
Spark::sendHeartbeatMsg(std::string const& ifName) {
  SCOPE_EXIT {
    // increment seq# after packet has been sent (even if it didnt go out)
    ++mySeqNum_;
  };

  SCOPE_FAIL {
    LOG(ERROR) << "Failed sending Heartbeat packet on " << ifName;
  };

  if (ifNameToActiveNeighbors_.find(ifName) == ifNameToActiveNeighbors_.end()) {
    VLOG(3) << "Interface: " << ifName
            << " hasn't have any active neighbor yet."
            << " Skip sending out heartbeatMsg.";
    return;
  }

  // in some cases, getting link-local address may fail and throw
  // e.g. when iface has not yet auto-configured it, or iface is removed but
  // down event has not arrived yet
  const auto& interfaceEntry = interfaceDb_.at(ifName);
  const auto ifIndex = interfaceEntry.ifIndex;
  const auto v6Addr = interfaceEntry.v6LinkLocalNetwork.first;

  // build heartbeat msg
  thrift::SparkHeartbeatMsg heartbeatMsg;
  *heartbeatMsg.nodeName_ref() = myNodeName_;
  heartbeatMsg.seqNum_ref() = mySeqNum_;

  thrift::SparkHelloPacket pkt;
  pkt.heartbeatMsg_ref() = std::move(heartbeatMsg);

  auto packet = fbzmq::util::writeThriftObjStr(pkt, serializer_);

  // send the pkt
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()),
      neighborDiscoveryPort_);

  if (kMinIpv6Mtu < packet.size()) {
    LOG(ERROR) << "Handshake packet is too big, can't send it out.";
    return;
  }

  auto bytesSent = IoProvider::sendMessage(
      mcastFd_, ifIndex, v6Addr.asV6(), dstAddr, packet, ioProvider_.get());

  if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
    VLOG(1) << "Sending multicast to " << dstAddr.getAddressStr() << " on "
            << ifName << " failed due to error " << folly::errnoStr(errno);
    return;
  }

  // update counters for number of pkts and total size of pkts sent
  fb303::fbData->addStatValue(
      "spark.heartbeat.bytes_sent", packet.size(), fb303::SUM);
  fb303::fbData->addStatValue("spark.heartbeat.packets_sent", 1, fb303::SUM);
}

void
Spark::logStateTransition(
    std::string const& neighborName,
    std::string const& ifName,
    SparkNeighState const& oldState,
    SparkNeighState const& newState) {
  SYSLOG(INFO) << "State change: [" << toStr(oldState) << "] -> ["
               << toStr(newState) << "] "
               << "for neighbor: (" << neighborName << ") on interface: ("
               << ifName << ").";

  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto& neighbor = ifNeighbors.at(neighborName);

  // Track neighbor discovery time
  if (newState == SparkNeighState::ESTABLISHED and
      oldState == SparkNeighState::NEGOTIATE) {
    // compute neighbor discovery time

    const auto elapsedTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() -
            neighbor.idleStateTransitionTime);

    LOG(INFO) << "Neighbor discovery time for neighbor: (" << neighborName
              << ") on interface: (" << ifName << ") " << elapsedTime.count()
              << " ms";

    fb303::fbData->addStatValue(
        "slo.neighbor_discovery_time.time_ms", elapsedTime.count(), fb303::AVG);
  } else if (newState == SparkNeighState::IDLE) {
    // reset neighbor discovery time
    neighbor.idleStateTransitionTime = std::chrono::steady_clock::now();
  }

  // Track how much time has elapsed from RESTART to ESTABLISHED transition.
  if (newState == SparkNeighState::ESTABLISHED and
      oldState == SparkNeighState::RESTART) {
    const auto elapsedTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() -
            neighbor.restartStateTransitionTime);

    LOG(INFO) << "Neighbor restart time for neighbor: (" << neighborName
              << ") on interface: (" << ifName << ") " << elapsedTime.count()
              << " ms";

    fb303::fbData->addStatValue(
        "slo.neighbor_restart.time_ms", elapsedTime.count(), fb303::AVG);
  } else if (newState == SparkNeighState::RESTART) {
    // reset neighbor restart time
    neighbor.restartStateTransitionTime = std::chrono::steady_clock::now();
  }
}

void
Spark::checkNeighborState(
    SparkNeighbor const& neighbor, SparkNeighState const& state) {
  CHECK(neighbor.state == state)
      << "Neighbor: (" << neighbor.nodeName << "), "
      << "Expected state: [" << toStr(state) << "], "
      << "Actual state: [" << toStr(neighbor.state) << "].";
}

folly::SemiFuture<std::optional<SparkNeighState>>
Spark::getSparkNeighState(
    std::string const& ifName, std::string const& neighborName) {
  folly::Promise<std::optional<SparkNeighState>> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread(
      [this, promise = std::move(promise), ifName, neighborName]() mutable {
        if (sparkNeighbors_.find(ifName) == sparkNeighbors_.end()) {
          LOG(ERROR) << "No interface: " << ifName
                     << " in sparkNeighbor collection";
          promise.setValue(std::nullopt);
        } else {
          auto& ifNeighbors = sparkNeighbors_.at(ifName);
          auto neighborIt = ifNeighbors.find(neighborName);
          if (neighborIt == ifNeighbors.end()) {
            LOG(ERROR) << "No neighborName: " << neighborName
                       << " in sparkNeighbor colelction";
            promise.setValue(std::nullopt);
          } else {
            auto& neighbor = neighborIt->second;
            promise.setValue(neighbor.state);
          }
        }
      });
  return sf;
}

void
Spark::neighborUpWrapper(
    SparkNeighbor& neighbor,
    std::string const& ifName,
    std::string const& neighborName) {
  // stop sending out handshake msg, no longer in NEGOTIATE stage
  neighbor.negotiateTimer.reset();

  // remove negotiate hold timer, no longer in NEGOTIATE stage
  neighbor.negotiateHoldTimer.reset();

  // create heartbeat hold timer when promote to "ESTABLISHED"
  neighbor.heartbeatHoldTimer = folly::AsyncTimeout::make(
      *getEvb(), [this, ifName, neighborName]() noexcept {
        processHeartbeatTimeout(ifName, neighborName);
      });
  neighbor.heartbeatHoldTimer->scheduleTimeout(neighbor.heartbeatHoldTime);

  // add neighborName to collection
  ifNameToActiveNeighbors_[ifName].emplace(neighborName);

  // notify LinkMonitor about neighbor UP state
  notifySparkNeighborEvent(
      thrift::SparkNeighborEventType::NEIGHBOR_UP,
      ifName,
      neighbor.toThrift(),
      neighbor.rtt.count(),
      neighbor.label,
      true /* support flood-optimization */,
      neighbor.area);
}

void
Spark::neighborDownWrapper(
    SparkNeighbor const& neighbor,
    std::string const& ifName,
    std::string const& neighborName) {
  // notify LinkMonitor about neighbor DOWN state
  notifySparkNeighborEvent(
      thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
      ifName,
      neighbor.toThrift(),
      neighbor.rtt.count(),
      neighbor.label,
      true /* support flood-optimization */,
      neighbor.area);

  // remove neighborship on this interface
  if (ifNameToActiveNeighbors_.find(ifName) == ifNameToActiveNeighbors_.end()) {
    LOG(WARNING) << "Ignore " << ifName << " as there is NO active neighbors.";
    return;
  }

  ifNameToActiveNeighbors_.at(ifName).erase(neighborName);
  if (ifNameToActiveNeighbors_.at(ifName).empty()) {
    ifNameToActiveNeighbors_.erase(ifName);
  }
}

void
Spark::notifySparkNeighborEvent(
    thrift::SparkNeighborEventType eventType,
    std::string const& ifName,
    thrift::SparkNeighbor const& originator,
    int64_t rttUs,
    int32_t label,
    bool supportFloodOptimization,
    const std::string& area) {
  thrift::SparkNeighborEvent event;
  event.eventType = eventType;
  event.ifName = ifName;
  event.neighbor = originator;
  event.rttUs = rttUs;
  event.label = label;
  event.supportFloodOptimization_ref() = supportFloodOptimization;
  *event.area_ref() = area;
  neighborUpdatesQueue_.push(std::move(event));
}

void
Spark::processHeartbeatTimeout(
    std::string const& ifName, std::string const& neighborName) {
  // spark neighbor must exist
  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto& neighbor = ifNeighbors.at(neighborName);

  // remove from tracked neighbor at the end
  SCOPE_EXIT {
    allocatedLabels_.erase(neighbor.label);
    ifNeighbors.erase(neighborName);
  };

  LOG(INFO) << "Heartbeat timer expired for: " << neighborName
            << " on interface " << ifName;

  // neighbor must in 'ESTABLISHED' state
  checkNeighborState(neighbor, SparkNeighState::ESTABLISHED);

  // state transition
  SparkNeighState oldState = neighbor.state;
  neighbor.state =
      getNextState(oldState, SparkNeighEvent::HEARTBEAT_TIMER_EXPIRE);
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // bring down neighborship and cleanup spark neighbor state
  neighborDownWrapper(neighbor, ifName, neighborName);
}

void
Spark::processNegotiateTimeout(
    std::string const& ifName, std::string const& neighborName) {
  // spark neighbor must exist if the negotiate hold-time expired
  auto& neighbor = sparkNeighbors_.at(ifName).at(neighborName);

  LOG(INFO) << "Negotiate timer expired for: " << neighborName
            << " on interface " << ifName;

  // neighbor must in 'NEGOTIATE' state
  checkNeighborState(neighbor, SparkNeighState::NEGOTIATE);

  // state transition
  SparkNeighState oldState = neighbor.state;
  neighbor.state =
      getNextState(oldState, SparkNeighEvent::NEGOTIATE_TIMER_EXPIRE);
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // stop sending out handshake msg, no longer in NEGOTIATE stage
  neighbor.negotiateTimer.reset();
}

void
Spark::processGRTimeout(
    std::string const& ifName, std::string const& neighborName) {
  // spark neighbor must exist if the negotiate hold-timer call back gets
  // called.
  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto& neighbor = ifNeighbors.at(neighborName);

  // remove from tracked neighbor at the end
  SCOPE_EXIT {
    allocatedLabels_.erase(neighbor.label);
    ifNeighbors.erase(neighborName);
  };

  LOG(INFO) << "Graceful restart timer expired for: " << neighborName
            << " on interface " << ifName;

  // neighbor must in "RESTART" state
  checkNeighborState(neighbor, SparkNeighState::RESTART);

  // state transition
  SparkNeighState oldState = neighbor.state;
  neighbor.state = getNextState(oldState, SparkNeighEvent::GR_TIMER_EXPIRE);
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // bring down neighborship and cleanup spark neighbor state
  neighborDownWrapper(neighbor, ifName, neighborName);
}

void
Spark::processGRMsg(
    std::string const& neighborName,
    std::string const& ifName,
    SparkNeighbor& neighbor) {
  // notify link-monitor for RESTARTING event
  notifySparkNeighborEvent(
      thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
      ifName,
      neighbor.toThrift(),
      neighbor.rtt.count(),
      neighbor.label,
      false /* supportDual: doesn't matter in DOWN event*/,
      neighbor.area);

  // start graceful-restart timer
  neighbor.gracefulRestartHoldTimer = folly::AsyncTimeout::make(
      *getEvb(), [this, ifName, neighborName]() noexcept {
        // change the state back to IDLE
        processGRTimeout(ifName, neighborName);
      });
  neighbor.gracefulRestartHoldTimer->scheduleTimeout(
      neighbor.gracefulRestartHoldTime);

  // state transition
  SparkNeighState oldState = neighbor.state;
  neighbor.state = getNextState(oldState, SparkNeighEvent::HELLO_RCVD_RESTART);
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // neihbor is restarting, shutdown heartbeat hold timer
  neighbor.heartbeatHoldTimer.reset();
}

void
Spark::processHelloMsg(
    thrift::SparkHelloMsg const& helloMsg,
    std::string const& ifName,
    std::chrono::microseconds const& myRecvTimeInUs) {
  auto const& neighborName = *helloMsg.nodeName_ref();
  auto const& domainName = *helloMsg.domainName_ref();
  auto const& remoteIfName = *helloMsg.ifName_ref();
  auto const& neighborInfos = *helloMsg.neighborInfos_ref();
  auto const& remoteVersion = static_cast<uint32_t>(*helloMsg.version_ref());
  auto const& remoteSeqNum = static_cast<uint64_t>(*helloMsg.seqNum_ref());
  auto const& nbrSentTimeInUs =
      std::chrono::microseconds(*helloMsg.sentTsInUs_ref());

  // interface name check
  if (sparkNeighbors_.find(ifName) == sparkNeighbors_.end()) {
    LOG(ERROR) << "Ignoring packet received from: " << neighborName
               << " on unknown interface: " << ifName;
    return;
  }

  auto sanityCheckResult = sanityCheckHelloPkt(
      domainName, neighborName, remoteIfName, remoteVersion);
  if (PacketValidationResult::SKIP_LOOPED_SELF == sanityCheckResult) {
    VLOG(4) << "Received self-looped hello pkt";
    return;
  }

  if (PacketValidationResult::FAILURE == sanityCheckResult) {
    return;
  }

  // get (neighborName -> SparkNeighbor) mapping per ifName
  auto& ifNeighbors = sparkNeighbors_.at(ifName);

  // check if we have already track this neighbor
  auto neighborIt = ifNeighbors.find(neighborName);

  if (neighborIt == ifNeighbors.end()) {
    // deduce area for neighbor
    // TODO: Spark is yet to support area change due to dynamic configuration.
    //       To avoid running area deducing logic for every single helloMsg,
    //       ONLY deduce for unknown neighbors.
    auto area =
        getNeighborArea(neighborName, ifName, config_->getAreaConfiguration());
    if (not area.has_value()) {
      return;
    }

    // Report RTT change
    // capture ifName & originator by copy
    auto rttChangeCb = [this, ifName, neighborName](const int64_t& newRtt) {
      processRttChange(ifName, neighborName, newRtt);
    };

    ifNeighbors.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(neighborName),
        std::forward_as_tuple(
            *config_->getSparkConfig().step_detector_conf_ref(), // step
                                                                 // detector
                                                                 // config
            domainName, // neighborNode domain
            neighborName, // neighborNode name
            remoteIfName, // remote interface on neighborNode
            getNewLabelForIface(ifName), // label for Segment Routing
            remoteSeqNum, // seqNum reported by neighborNode
            keepAliveTime_, // stepDetector sample period
            std::move(rttChangeCb),
            area.value()));

    auto& neighbor = ifNeighbors.at(neighborName);
    checkNeighborState(neighbor, SparkNeighState::IDLE);
  }

  // Up till now, node knows about this neighbor and perform SM check
  auto& neighbor = ifNeighbors.at(neighborName);

  // Update timestamps for received hello packet for neighbor
  neighbor.neighborTimestamp = nbrSentTimeInUs;
  neighbor.localTimestamp = myRecvTimeInUs;

  // Deduce RTT for this neighbor and update timestamps
  auto tsIt = neighborInfos.find(myNodeName_);
  if (tsIt != neighborInfos.end()) {
    auto& ts = tsIt->second;
    updateNeighborRtt(
        // recvTime of neighbor helloPkt
        myRecvTimeInUs,
        // sentTime of my helloPkt recorded by neighbor
        std::chrono::microseconds(*ts.lastNbrMsgSentTsInUs_ref()),
        // recvTime of my helloPkt recorded by neighbor
        std::chrono::microseconds(*ts.lastMyMsgRcvdTsInUs_ref()),
        // sentTime of neighbor helloPkt
        nbrSentTimeInUs,
        neighborName,
        remoteIfName,
        ifName);
  }

  VLOG(3) << "Current state for neighbor: (" << neighborName << ") is: ["
          << toStr(neighbor.state) << "]";

  // for neighbor in fast initial state and does not see us yet,
  // reply for quick convergence
  if (*helloMsg.solicitResponse_ref()) {
    sendHelloMsg(ifName);

    VLOG(3) << "Reply to neighbor's helloMsg since it is under fastInit";
  }

  if (neighbor.state == SparkNeighState::IDLE) {
    // state transition
    SparkNeighState oldState = neighbor.state;
    neighbor.state =
        getNextState(oldState, SparkNeighEvent::HELLO_RCVD_NO_INFO);
    logStateTransition(neighborName, ifName, oldState, neighbor.state);
  } else if (neighbor.state == SparkNeighState::WARM) {
    // Update local seqNum maintained for this neighbor
    neighbor.seqNum = remoteSeqNum;

    if (tsIt == neighborInfos.end()) {
      // Neighbor is NOT aware of us, ignore helloMsg
      return;
    }

    // My node's Seq# seen from neighbor should NOT be higher than ours
    // since it always received helloMsg sent previously. If it is the
    // case, it normally means we have recently restarted ourself.
    //
    // Ignore this helloMsg from my previous incarnation.
    // Wait for neighbor to catch up with the latest Seq#.
    const uint64_t myRemoteSeqNum =
        static_cast<uint64_t>(*neighborInfos.at(myNodeName_).seqNum_ref());
    if (myRemoteSeqNum >= mySeqNum_) {
      VLOG(2) << "Seeing my previous incarnation from neighbor: ("
              << neighborName << "). Seen Seq# from neighbor: ("
              << myRemoteSeqNum << "), my Seq#: (" << mySeqNum_ << ").";
      return;
    }

    // Starts timer to periodically send hankshake msg
    const std::string neighborAreaId = neighbor.area;
    neighbor.negotiateTimer = folly::AsyncTimeout::make(
        *getEvb(), [this, ifName, neighborName, neighborAreaId]() noexcept {
          sendHandshakeMsg(ifName, neighborName, neighborAreaId, false);
          // send out handshake msg periodically to this neighbor
          CHECK(sparkNeighbors_.count(ifName) > 0)
              << folly::sformat("Key NOT found for: {}", ifName);
          CHECK(sparkNeighbors_.at(ifName).count(neighborName) > 0)
              << folly::sformat(
                     "Key NOT found: {} under: {}", neighborName, ifName);
          sparkNeighbors_.at(ifName)
              .at(neighborName)
              .negotiateTimer->scheduleTimeout(handshakeTime_);
        });
    neighbor.negotiateTimer->scheduleTimeout(handshakeTime_);

    // Starts negotiate hold-timer
    neighbor.negotiateHoldTimer = folly::AsyncTimeout::make(
        *getEvb(), [this, ifName, neighborName]() noexcept {
          // prevent to stucking in NEGOTIATE forever
          processNegotiateTimeout(ifName, neighborName);
        });
    neighbor.negotiateHoldTimer->scheduleTimeout(handshakeHoldTime_);

    // Neighbor is aware of us. Promote to NEGOTIATE state
    SparkNeighState oldState = neighbor.state;
    neighbor.state = getNextState(oldState, SparkNeighEvent::HELLO_RCVD_INFO);
    logStateTransition(neighborName, ifName, oldState, neighbor.state);
  } else if (neighbor.state == SparkNeighState::ESTABLISHED) {
    // Update local seqNum maintained for this neighbor
    neighbor.seqNum = remoteSeqNum;

    // Check if neighbor is undergoing 'Graceful-Restart'
    if (*helloMsg.restarting_ref()) {
      LOG(INFO) << "Adjacent neighbor (" << neighborName << "), "
                << "from remote interface: (" << remoteIfName << "), "
                << "on interface: (" << ifName << ") is restarting.";
      processGRMsg(neighborName, ifName, neighbor);
      return;
    }

    if (tsIt == neighborInfos.end()) {
      //
      // Did NOT find our own info in peer's hello msg. Peer doesn't want to
      // form adjacency with us. Drop neighborship.
      //
      SparkNeighState oldState = neighbor.state;
      neighbor.state =
          getNextState(oldState, SparkNeighEvent::HELLO_RCVD_NO_INFO);
      logStateTransition(neighborName, ifName, oldState, neighbor.state);

      // bring down neighborship and cleanup spark neighbor state
      neighborDownWrapper(neighbor, ifName, neighborName);

      // remove from tracked neighbor at the end
      allocatedLabels_.erase(neighbor.label);
      ifNeighbors.erase(neighborName);
    }
  } else if (neighbor.state == SparkNeighState::RESTART) {
    // Neighbor is undergoing restart. Will reply immediately for hello msg for
    // quick adjacency establishment.
    if (tsIt == neighborInfos.end()) {
      // Neighbor is NOT aware of us, ignore helloMsg
      return;
    }

    if (neighbor.seqNum < remoteSeqNum) {
      // By going here, it means this node missed ALL of the helloMsg sent-out
      // after neighbor 'restarting' itself. Will let GR timer to handle it.
      LOG(WARNING) << "Unexpected Seq#:" << remoteSeqNum
                   << " received from neighbor: (" << neighborName
                   << "), local Seq#: (" << neighbor.seqNum << ").";
      return;
    }

    // Neighbor is back from 'restarting' state. Go back to 'ESTABLISHED'
    LOG(INFO) << "Node: (" << neighborName << ") is back from restart. "
              << "Received Seq#: (" << remoteSeqNum << "), local Seq#: ("
              << neighbor.seqNum << ").";

    // Update local seqNum maintained for this neighbor
    neighbor.seqNum = remoteSeqNum;

    notifySparkNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
        ifName,
        neighbor.toThrift(),
        neighbor.rtt.count(),
        neighbor.label,
        true /* support flood-optimization */,
        neighbor.area);

    // start heartbeat timer again to make sure neighbor is alive
    neighbor.heartbeatHoldTimer = folly::AsyncTimeout::make(
        *getEvb(), [this, ifName, neighborName]() noexcept {
          processHeartbeatTimeout(ifName, neighborName);
        });
    neighbor.heartbeatHoldTimer->scheduleTimeout(neighbor.heartbeatHoldTime);

    // stop the graceful-restart hold-timer
    neighbor.gracefulRestartHoldTimer.reset();

    SparkNeighState oldState = neighbor.state;
    neighbor.state = getNextState(oldState, SparkNeighEvent::HELLO_RCVD_INFO);
    logStateTransition(neighborName, ifName, oldState, neighbor.state);
  }
}

void
Spark::processHandshakeMsg(
    thrift::SparkHandshakeMsg const& handshakeMsg, std::string const& ifName) {
  // Ignore handshakeMsg if I am NOT the receiver as AREA negotiation
  // is point-to-point
  if (auto neighborNodeName = handshakeMsg.neighborNodeName_ref()) {
    if (*neighborNodeName != myNodeName_) {
      VLOG(4) << "Ignoring handshakeMsg targeted for node: "
              << *neighborNodeName << ", my node name: " << myNodeName_;
      return;
    }
  }

  auto const& neighborName = *handshakeMsg.nodeName_ref();
  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto neighborIt = ifNeighbors.find(neighborName);

  // under quick flapping of Openr, msg can come out-of-order.
  // handshakeMsg will ONLY be processed when:
  //  1). neighbor is tracked on ifName;
  //  2). neighbor is under NEGOTIATE stage;
  if (neighborIt == ifNeighbors.end()) {
    VLOG(3) << "Neighbor: (" << neighborName
            << "). is NOT found. Ignore handshakeMsg.";
    return;
  }

  auto& neighbor = neighborIt->second;

  // for quick convergence, reply immediately if neighbor
  // hasn't form adjacency with us yet.
  //
  // ATTN: in case of v4 subnet validation fails, neighbor
  //       state will fall back from NEGOTIATE => WARM.
  //       Node should NOT ask for handshakeMsg reply to
  //       avoid infinite loop of pkt between nodes.
  if (not(*handshakeMsg.isAdjEstablished_ref())) {
    sendHandshakeMsg(
        ifName,
        neighborName,
        neighbor.area,
        neighbor.state != SparkNeighState::NEGOTIATE);
    LOG(INFO) << "Neighbor: (" << neighborName
              << ") has NOT forming adj with us yet. "
              << "Reply to handshakeMsg immediately.";
  }

  // After GR from peerNode, peerNode will go through:
  //
  // IDLE => WARM => NEGOTIATE => ESTABLISHED
  //
  // Node can receive handshakeMsg from peerNode although it has already
  // marked peer in ESTABLISHED state. Avoid unnecessary adj drop when
  // handshake is happening by extending heartbeat hold timer.
  if (neighbor.heartbeatHoldTimer) {
    // Reset the hold-timer for neighbor as we have received a keep-alive msg
    LOG(INFO) << "Extend heartbeat timer for neighbor: " << neighborName;
    neighbor.heartbeatHoldTimer->scheduleTimeout(neighbor.heartbeatHoldTime);
  }

  // skip NEGOTIATE step if neighbor is NOT in state. This can happen:
  //  1). negotiate hold timer already expired;
  //  2). v4 validation failed and fall back to WARM;
  if (neighbor.state != SparkNeighState::NEGOTIATE) {
    VLOG(3) << "For neighborNode (" << neighborName << "): current state: ["
            << toStr(neighbor.state) << "]"
            << ", expected state: [NEGOTIIATE]";
    return;
  }

  // update Spark neighborState
  neighbor.kvStoreCmdPort = *handshakeMsg.kvStoreCmdPort_ref();
  neighbor.openrCtrlThriftPort = *handshakeMsg.openrCtrlThriftPort_ref();
  neighbor.transportAddressV4 = *handshakeMsg.transportAddressV4_ref();
  neighbor.transportAddressV6 = *handshakeMsg.transportAddressV6_ref();

  // update neighbor holdTime as "NEGOTIATING" process
  neighbor.heartbeatHoldTime = std::max(
      std::chrono::milliseconds(*handshakeMsg.holdTime_ref()), holdTime_);
  neighbor.gracefulRestartHoldTime = std::max(
      std::chrono::milliseconds(*handshakeMsg.gracefulRestartTime_ref()),
      gracefulRestartTime_);

  // v4 subnet validation if enabled
  if (enableV4_) {
    if (PacketValidationResult::FAILURE ==
        validateV4AddressSubnet(
            ifName, *handshakeMsg.transportAddressV4_ref())) {
      // state transition
      SparkNeighState oldState = neighbor.state;
      neighbor.state =
          getNextState(oldState, SparkNeighEvent::NEGOTIATION_FAILURE);
      logStateTransition(neighborName, ifName, oldState, neighbor.state);

      // stop sending out handshake msg, no longer in NEGOTIATE stage
      neighbor.negotiateTimer.reset();
      // remove negotiate hold timer, no longer in NEGOTIATE stage
      neighbor.negotiateHoldTimer.reset();

      return;
    }
  }

  // area validation. Compare the following:
  //
  //  1) handshakeMsg.area: areaId that neighbor node thinks I should be in;
  //  2) neighbor.area: areaId that I think neighbor node should be in;
  //
  //  ONLY promote to NEGOTIATE state if areaId matches
  if (neighbor.area != thrift::KvStore_constants::kDefaultArea() &&
      *handshakeMsg.area_ref() != thrift::KvStore_constants::kDefaultArea()) {
    // For backward compatible consideration, If:
    //  1) neighbor.area == defaulArea: this node doesn't support areaConfig;
    //  2) handshakeMsg.area == defaultArea: peer doesn't support areaConfig;
    if (neighbor.area != *handshakeMsg.area_ref()) {
      LOG(ERROR)
          << "Inconsistent areaId deduced between local and remote review. "
          << "Neighbor's areaId: [" << neighbor.area << "], "
          << "My areaId from remote: [" << *handshakeMsg.area_ref() << "].";

      // state transition
      SparkNeighState oldState = neighbor.state;
      neighbor.state =
          getNextState(oldState, SparkNeighEvent::NEGOTIATION_FAILURE);
      logStateTransition(neighborName, ifName, oldState, neighbor.state);

      // stop sending out handshake msg, no longer in NEGOTIATE stage
      neighbor.negotiateTimer.reset();
      // remove negotiate hold timer, no longer in NEGOTIATE stage
      neighbor.negotiateHoldTimer.reset();
      return;
    }
  } else {
    // Backward compatibility:
    // In case peer/me doesn't support AREA negotiation.
    // Use local configuration: nerighbor.area. Ignore handshakeMsg.area msg.
  }

  // state transition
  SparkNeighState oldState = neighbor.state;
  neighbor.state = getNextState(oldState, SparkNeighEvent::HANDSHAKE_RCVD);
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // bring up neighborship and set corresponding spark state
  neighborUpWrapper(neighbor, ifName, neighborName);
}

void
Spark::processHeartbeatMsg(
    thrift::SparkHeartbeatMsg const& heartbeatMsg, std::string const& ifName) {
  auto const& neighborName = *heartbeatMsg.nodeName_ref();
  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto neighborIt = ifNeighbors.find(neighborName);

  // under GR case, when node restarts, it will needs several helloMsg to
  // establish neighborship. During this time, heartbeatMsg from peer
  // will NOT be processed.
  if (neighborIt == ifNeighbors.end()) {
    VLOG(3) << "I am NOT aware of neighbor: (" << neighborName
            << "). Ignore it.";
    return;
  }

  auto& neighbor = neighborIt->second;

  // In case receiving heartbeat msg when it is NOT in established state,
  // Just ignore it.
  if (neighbor.state != SparkNeighState::ESTABLISHED) {
    VLOG(3) << "For neighborNode (" << neighborName << "): current state: ["
            << toStr(neighbor.state) << "]"
            << ", expected state: [ESTABLISHED]";
    return;
  }

  // Reset the hold-timer for neighbor as we have received a keep-alive msg
  neighbor.heartbeatHoldTimer->scheduleTimeout(neighbor.heartbeatHoldTime);
}

void
Spark::processPacket() {
  // receive and parse pkt
  thrift::SparkHelloPacket helloPacket;
  std::string ifName;
  std::chrono::microseconds myRecvTime;

  if (!parsePacket(helloPacket, ifName, myRecvTime)) {
    return;
  }

  // Spark specific msg processing
  if (helloPacket.helloMsg_ref().has_value()) {
    processHelloMsg(helloPacket.helloMsg_ref().value(), ifName, myRecvTime);
  } else if (helloPacket.heartbeatMsg_ref().has_value()) {
    processHeartbeatMsg(helloPacket.heartbeatMsg_ref().value(), ifName);
  } else if (helloPacket.handshakeMsg_ref().has_value()) {
    processHandshakeMsg(helloPacket.handshakeMsg_ref().value(), ifName);
  }
}

void
Spark::sendHelloMsg(
    std::string const& ifName, bool inFastInitState, bool restarting) {
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
  thrift::OpenrVersion openrVer(*kVersion_.version_ref());

  // build the helloMsg from scratch
  thrift::SparkHelloMsg helloMsg;
  *helloMsg.domainName_ref() = myDomainName_;
  *helloMsg.nodeName_ref() = myNodeName_;
  *helloMsg.ifName_ref() = ifName;
  helloMsg.seqNum_ref() = mySeqNum_;
  *helloMsg.neighborInfos_ref() =
      std::map<std::string, thrift::ReflectedNeighborInfo>{};
  helloMsg.version_ref() = openrVer;
  helloMsg.solicitResponse_ref() = inFastInitState;
  helloMsg.restarting_ref() = restarting;
  helloMsg.sentTsInUs_ref() = getCurrentTimeInUs().count();

  // bake neighborInfo into helloMsg
  for (const auto& kv : sparkNeighbors_.at(ifName)) {
    auto const& neighborName = kv.first;
    auto const& neighbor = kv.second;

    auto& neighborInfo = helloMsg.neighborInfos_ref()[neighborName];
    neighborInfo.seqNum_ref() = neighbor.seqNum;
    neighborInfo.lastNbrMsgSentTsInUs_ref() =
        neighbor.neighborTimestamp.count();
    neighborInfo.lastMyMsgRcvdTsInUs_ref() = neighbor.localTimestamp.count();
  }

  // fill in helloMsg field
  thrift::SparkHelloPacket helloPacket;
  helloPacket.helloMsg_ref() = std::move(helloMsg);

  // send the payload
  auto packet = fbzmq::util::writeThriftObjStr(helloPacket, serializer_);
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()),
      neighborDiscoveryPort_);

  if (kMinIpv6Mtu < packet.size()) {
    LOG(ERROR) << "Hello packet is too big, cannot sent!";
    return;
  }

  auto bytesSent = IoProvider::sendMessage(
      mcastFd_, ifIndex, v6Addr.asV6(), dstAddr, packet, ioProvider_.get());

  if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
    VLOG(1) << "Sending multicast to " << dstAddr.getAddressStr() << " on "
            << ifName << " failed due to error " << folly::errnoStr(errno);
    return;
  }

  // update counters for number of pkts and total size of pkts sent
  fb303::fbData->addStatValue(
      "spark.hello.bytes_sent", packet.size(), fb303::SUM);
  fb303::fbData->addStatValue("spark.hello.packets_sent", 1, fb303::SUM);

  VLOG(4) << "Sent " << bytesSent << " bytes in hello packet";
}

void
Spark::processInterfaceUpdates(thrift::InterfaceDatabase&& ifDb) {
  decltype(interfaceDb_) newInterfaceDb{};

  CHECK_EQ(*ifDb.thisNodeName_ref(), myNodeName_)
      << "Node name in ifDb " << *ifDb.thisNodeName_ref()
      << " does not match my node name " << myNodeName_;

  //
  // To be conisdered a valid interface for Spark to track, it must:
  // - be up
  // - have a v6LinkLocal IP
  // - have an IPv4 addr when v4 is enabled
  //
  for (const auto& kv : *ifDb.interfaces_ref()) {
    const auto& ifName = kv.first;
    const auto isUp = kv.second.isUp;
    const auto& ifIndex = kv.second.ifIndex;
    const auto& networks = *kv.second.networks_ref();

    // Sort networks and use the lowest one (other node will do similar)
    std::set<folly::CIDRNetwork> v4Networks;
    std::set<folly::CIDRNetwork> v6LinkLocalNetworks;
    for (const auto& ntwk : networks) {
      const auto& ipNetwork = toIPNetwork(ntwk, false);
      if (ipNetwork.first.isV4()) {
        v4Networks.emplace(ipNetwork);
      } else if (ipNetwork.first.isV6() && ipNetwork.first.isLinkLocal()) {
        v6LinkLocalNetworks.emplace(ipNetwork);
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
      v4Network = *v4Networks.begin();
    }
    folly::CIDRNetwork v6LinkLocalNetwork = *v6LinkLocalNetworks.begin();

    newInterfaceDb.emplace(
        ifName, Interface(ifIndex, v4Network, v6LinkLocalNetwork));
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

  // remove the interfaces no longer in newdb
  deleteInterfaceFromDb(toDel);

  // Adding interfaces
  addInterfaceToDb(toAdd, newInterfaceDb);

  // Updating interface. If ifindex changes, we need to unsubscribe old ifindex
  // from mcast and subscribe new one
  updateInterfaceInDb(toUpdate, newInterfaceDb);
}

void
Spark::deleteInterfaceFromDb(const std::set<std::string>& toDel) {
  for (const auto& ifName : toDel) {
    LOG(INFO) << "Removing " << ifName << " from Spark. "
              << "It is down, declaring all neighbors down";

    for (const auto& kv : sparkNeighbors_.at(ifName)) {
      auto& neighborName = kv.first;
      auto& neighbor = kv.second;
      allocatedLabels_.erase(neighbor.label);
      LOG(INFO) << "Neighbor " << neighborName << " removed due to iface "
                << ifName << " down";

      CHECK(not neighbor.nodeName.empty());
      CHECK(not neighbor.remoteIfName.empty());

      // Spark will NOT notify neighbor DOWN event in following cases:
      //    1). v6Addr is empty for this neighbor;
      //    2). v4 enabled and v4Addr is empty for this neighbor;
      if (neighbor.transportAddressV6.addr.empty() ||
          (enableV4_ && neighbor.transportAddressV4.addr.empty())) {
        continue;
      }
      neighborDownWrapper(neighbor, ifName, neighborName);
    }
    sparkNeighbors_.erase(ifName);
    ifNameToHeartbeatTimers_.erase(ifName);

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
    ifNameToHelloTimers_.erase(ifName);
    interfaceDb_.erase(ifName);
  }
}

void
Spark::addInterfaceToDb(
    const std::set<std::string>& toAdd,
    const std::unordered_map<std::string, Interface>& newInterfaceDb) {
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
      auto result = sparkNeighbors_.emplace(
          ifName, std::unordered_map<std::string, SparkNeighbor>{});
      CHECK(result.second);

      // heartbeatTimers will start as soon as intf is in UP state
      auto heartbeatTimer =
          folly::AsyncTimeout::make(*getEvb(), [this, ifName]() noexcept {
            sendHeartbeatMsg(ifName);
            // schedule heartbeatTimers periodically as soon as intf is UP
            ifNameToHeartbeatTimers_.at(ifName)->scheduleTimeout(
                keepAliveTime_);
          });

      ifNameToHeartbeatTimers_.emplace(ifName, std::move(heartbeatTimer));
      ifNameToHeartbeatTimers_.at(ifName)->scheduleTimeout(keepAliveTime_);
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

    auto roll = rollHelper(helloTime_);
    auto rollFast = rollHelper(fastInitHelloTime_);
    auto timePoint = std::chrono::steady_clock::now();

    // NOTE: We do not send hello packet immediately after adding new interface
    // this is due to the fact that it may not have yet configured a link-local
    // address. The hello packet will be sent later and will have good chances
    // of making it out if small delay is introduced.
    auto helloTimer = folly::AsyncTimeout::make(
        *getEvb(),
        [this, ifName, timePoint, roll, rollFast]() mutable noexcept {
          VLOG(3) << "Sending hello multicast packet on interface " << ifName;
          bool inFastInitState = false;
          // Under Spark context, hello pkt will be sent in relatively low
          // frequency. However, when node comes up initially or restarting,
          // send multiple helloMsg to promote to 'NEGOTIATE' state ASAP.
          // To form adj, at least 2 helloMsg is needed( i.e. with second
          // hello contain myNodeName_ info ). To give enough margin, send
          // 3 times of necessary packets.
          inFastInitState = (std::chrono::steady_clock::now() - timePoint) <=
              6 * fastInitHelloTime_;

          sendHelloMsg(ifName, inFastInitState);

          // Schedule next run (add 20% variance)
          // overriding timeoutPeriod if I am in fast initial state
          std::chrono::milliseconds timeoutPeriod =
              inFastInitState ? rollFast() : roll();

          ifNameToHelloTimers_.at(ifName)->scheduleTimeout(timeoutPeriod);
        });

    // should be in fast init state when the node just starts
    helloTimer->scheduleTimeout(rollFast());
    ifNameToHelloTimers_[ifName] = std::move(helloTimer);
  }
}

void
Spark::updateInterfaceInDb(
    const std::set<std::string>& toUpdate,
    const std::unordered_map<std::string, Interface>& newInterfaceDb) {
  for (const auto& ifName : toUpdate) {
    auto& interface = interfaceDb_.at(ifName);
    auto& newInterface = newInterfaceDb.at(ifName);

    if (interface == newInterface) {
      VLOG(3) << "No update to iface " << ifName << " in spark tracking";
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
    LOG(INFO) << "Updating iface " << ifName << " in spark tracking from "
              << "(ifindex " << interface.ifIndex << ", addrs "
              << interface.v6LinkLocalNetwork.first << " , "
              << interface.v4Network.first << ") to "
              << "(ifindex " << newInterface.ifIndex << ", addrs "
              << newInterface.v6LinkLocalNetwork.first << " , "
              << newInterface.v4Network.first << ")";

    interface = std::move(newInterface);
  }
}

std::optional<std::string>
Spark::findInterfaceFromIfindex(int ifIndex) {
  for (const auto& kv : interfaceDb_) {
    if (kv.second.ifIndex == ifIndex) {
      return kv.first;
    }
  }
  return std::nullopt;
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
Spark::updateGlobalCounters() {
  // set some flat counters
  int64_t adjacentNeighborCount{0}, trackedNeighborCount{0};
  for (auto const& ifaceNeighbors : sparkNeighbors_) {
    trackedNeighborCount += ifaceNeighbors.second.size();
    for (auto const& kv : ifaceNeighbors.second) {
      auto const& neighbor = kv.second;
      adjacentNeighborCount += neighbor.state == SparkNeighState::ESTABLISHED;
      fb303::fbData->setCounter(
          "spark.rtt_us." + neighbor.nodeName + "." + ifaceNeighbors.first,
          neighbor.rtt.count());
      fb303::fbData->setCounter(
          "spark.rtt_latest_us." + neighbor.nodeName,
          neighbor.rttLatest.count());
      fb303::fbData->setCounter(
          "spark.seq_num." + neighbor.nodeName, neighbor.seqNum);
    }
  }
  fb303::fbData->setCounter(
      "spark.num_tracked_interfaces", sparkNeighbors_.size());
  fb303::fbData->setCounter(
      "spark.num_tracked_neighbors", trackedNeighborCount);
  fb303::fbData->setCounter(
      "spark.num_adjacent_neighbors", adjacentNeighborCount);
  fb303::fbData->setCounter(
      "spark.tracked_adjacent_neighbors_diff",
      trackedNeighborCount - adjacentNeighborCount);
  fb303::fbData->setCounter("spark.my_seq_num", mySeqNum_);
  fb303::fbData->setCounter("spark.pending_timers", getEvb()->timer().count());
}

// This is a static function
std::optional<std::string>
Spark::getNeighborArea(
    const std::string& peerNodeName,
    const std::string& localIfName,
    const std::unordered_map<std::string /* areaId */, AreaConfiguration>&
        areaConfigs) {
  std::vector<std::string> candidateAreas{};

  // looping through areaIdRegexList
  for (const auto& t : areaConfigs) {
    const auto& areaId = t.first;
    const auto& neighborRegex = t.second.neighborRegexList;
    const auto& interfaceRegex = t.second.interfaceRegexList;

    if (neighborRegex and interfaceRegex) {
      if (matchRegexSet(peerNodeName, neighborRegex) and
          matchRegexSet(localIfName, interfaceRegex)) {
        VLOG(1) << folly::sformat(
            "Area: {} found for neighbor: {}, interface: {}",
            areaId,
            peerNodeName,
            localIfName);
        candidateAreas.emplace_back(areaId);
      }
    } else if (neighborRegex and matchRegexSet(peerNodeName, neighborRegex)) {
      VLOG(1) << folly::sformat(
          "Area: {} found for neighbor: {}", areaId, peerNodeName);
      candidateAreas.emplace_back(areaId);
    } else if (interfaceRegex and matchRegexSet(localIfName, interfaceRegex)) {
      VLOG(1) << folly::sformat(
          "Area: {} found for interface: {}", areaId, localIfName);
      candidateAreas.emplace_back(areaId);
    }
  }

  if (candidateAreas.empty()) {
    LOG(ERROR) << "No matching area found for neighbor: " << peerNodeName;
    fb303::fbData->addStatValue("spark.neighbor_no_area", 1, fb303::COUNT);
    return std::nullopt;
  } else if (candidateAreas.size() > 1) {
    LOG(ERROR) << "Multiple area found for neighbor: " << peerNodeName;
    fb303::fbData->addStatValue(
        "spark.neighbor_multiple_area", 1, fb303::COUNT);
    return std::nullopt;
  }
  return candidateAreas.back();
}

void
Spark::setThrowParserErrors(bool val) {
  isThrowParserErrorsOn_ = val;
}

} // namespace openr
