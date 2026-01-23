/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/logging/xlog.h>

#include <openr/common/Constants.h>
#include <openr/common/EventLogger.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/spark/Spark.h>

#include <thrift/lib/cpp/protocol/TProtocolException.h>

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
// Subscribe/unsubscribe to a multicast group on given interface
//
bool
toggleMcastGroup(
    int fd,
    folly::IPAddress mcastGroup,
    int ifIndex,
    bool join,
    openr::IoProvider* ioProvider) {
  XLOG(DBG1) << fmt::format(
      "Subscribing to link-local multicast on ifIndex: {}", ifIndex);

  if (!mcastGroup.isMulticast()) {
    XLOG(ERR) << fmt::format(
        "IP address {} is not multicast address", mcastGroup.str());
    return false;
  }

  // Join multicast group on interface
  struct ipv6_mreq mreq;
  mreq.ipv6mr_interface = ifIndex;
  ::memcpy(&mreq.ipv6mr_multiaddr, mcastGroup.bytes(), mcastGroup.byteCount());

  if (join) {
    if (ioProvider->setsockopt(
            fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
      XLOG(ERR) << fmt::format(
          "setsockopt ipv6_join_group failed: {}", folly::errnoStr(errno));
      return false;
    }

    XLOG(INFO) << fmt::format(
        "Joined multicast addr: {} on ifIndex: {}", mcastGroup.str(), ifIndex);
    return true;
  }

  // Leave multicast group on interface
  if (ioProvider->setsockopt(
          fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq)) != 0) {
    XLOG(ERR) << fmt::format(
        "setsockopt ipv6_leave_group failed: {}", folly::errnoStr(errno));
    return false;
  }

  XLOG(INFO) << fmt::format(
      "Left multicast addr: {}, on ifIndex: {}", mcastGroup.str(), ifIndex);
  return true;
}

} // namespace

namespace openr {

const std::vector<std::vector<std::optional<thrift::SparkNeighState>>>
    Spark::stateMap_ = {
        /*
         * index 0 - IDLE
         * HELLO_RCVD_INFO => WARM; HELLO_RCVD_NO_INFO => WARM
         */
        {thrift::SparkNeighState::WARM,
         thrift::SparkNeighState::WARM,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt},
        /*
         * index 1 - WARM
         * HELLO_RCVD_INFO => NEGOTIATE; GR_TIMER_EXPIRE => IDLE
         */
        {thrift::SparkNeighState::NEGOTIATE,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         thrift::SparkNeighState::IDLE,
         std::nullopt},
        /*
         * index 2 - NEGOTIATE
         * HANDSHAKE_RCVD => ESTABLISHED; NEGOTIATE_TIMER_EXPIRE => WARM;
         * GR_TIMER_EXPIRE => IDLE; NEGOTIATION_FAILURE => WARM;
         */
        {std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         thrift::SparkNeighState::ESTABLISHED,
         std::nullopt,
         thrift::SparkNeighState::WARM,
         thrift::SparkNeighState::IDLE,
         thrift::SparkNeighState::WARM},
        /*
         * index 3 - ESTABLISHED
         * HELLO_RCVD_NO_INFO => IDLE; HELLO_RCVD_RESTART => RESTART;
         * HEARTBEAT_RCVD => ESTABLISHED; HEARTBEAT_TIMER_EXPIRE => IDLE;
         */
        {std::nullopt,
         thrift::SparkNeighState::IDLE,
         thrift::SparkNeighState::RESTART,
         thrift::SparkNeighState::ESTABLISHED,
         std::nullopt,
         thrift::SparkNeighState::IDLE,
         std::nullopt,
         std::nullopt,
         std::nullopt},
        /*
         * index 4 - RESTART
         * HELLO_RCVD_INFO => NEGOTIATE; GR_TIMER_EXPIRE => IDLE
         */
        {thrift::SparkNeighState::NEGOTIATE,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         thrift::SparkNeighState::IDLE,
         std::nullopt}};

thrift::SparkNeighState
Spark::getNextState(
    std::optional<thrift::SparkNeighState> const& currState,
    thrift::SparkNeighEvent const& event) {
  CHECK(currState.has_value()) << "Current state is 'UNEXPECTED'";

  std::optional<thrift::SparkNeighState> nextState =
      stateMap_[static_cast<uint32_t>(currState.value())]
               [static_cast<uint32_t>(event)];

  CHECK(nextState.has_value()) << "Next state is 'UNEXPECTED'";
  return nextState.value();
}

/*
 * [SparkNeighbor]
 *
 * SparkNeighbor is the struct to hold data-structures of neighbor information
 * from neighbor discovery process. It manages neighbors and report the updates
 * to LinkMonitor.
 */
Spark::SparkNeighbor::SparkNeighbor(
    const thrift::StepDetectorConfig& stepDetectorConfig,
    std::string const& nodeName,
    std::string const& localIfName,
    std::string const& remoteIfName,
    uint64_t seqNum,
    const std::chrono::milliseconds& samplingPeriod,
    std::function<void(const int64_t&)> rttChangeCb,
    const std::string& adjArea)
    : nodeName(nodeName),
      localIfName(localIfName),
      remoteIfName(remoteIfName),
      seqNum(seqNum),
      stepDetector(
          stepDetectorConfig /* step detector config */,
          samplingPeriod /* sampling period */,
          rttChangeCb /* callback function */),
      area(adjArea) {
  CHECK(!this->nodeName.empty());
  CHECK(!this->localIfName.empty());
  CHECK(!this->remoteIfName.empty());
}

thrift::SparkNeighbor
Spark::SparkNeighbor::toThrift() const {
  thrift::SparkNeighbor info;

  // populate basic info
  info.nodeName() = this->nodeName;
  info.state() = apache::thrift::util::enumNameSafe(this->state);
  info.event() = apache::thrift::util::enumNameSafe(this->event);
  info.area() = this->area;

  // populate address/port info for TCP connection
  info.transportAddressV4() = this->transportAddressV4;
  info.transportAddressV6() = this->transportAddressV6;
  info.openrCtrlThriftPort() = this->openrCtrlThriftPort;

  // populate interface info
  info.localIfName() = this->localIfName;
  info.remoteIfName() = this->remoteIfName;

  // populate misc info
  info.rttUs() = this->rtt.count();

  // populate telemetry info
  auto currentTime = getCurrentTime<std::chrono::milliseconds>();
  info.lastHelloMsgSentTimeDelta() =
      currentTime.count() - lastHelloMsgSentAt.count();
  info.lastHandshakeMsgSentTimeDelta() =
      currentTime.count() - lastHandshakeMsgSentAt.count();
  info.lastHeartbeatMsgSentTimeDelta() =
      currentTime.count() - lastHeartbeatMsgSentAt.count();

  return info;
}

/*
 * This is the util function to determine if flag `adjOnlyUsedByOtherNode` will
 * be reset based on received `SparkHeartbeatMsg`.
 */
bool
Spark::SparkNeighbor::shouldResetAdjacency(
    const thrift::SparkHeartbeatMsg& heartbeatMsg) {
  // Skip resetting if adjacency is NOT on hold.
  if (!this->adjOnlyUsedByOtherNode) {
    return false;
  }

  // Honor the `holdAdjacency` flag from `SparkHeartbeatMsg`
  return !*heartbeatMsg.holdAdjacency();
}

Spark::Spark(
    messaging::RQueue<InterfaceDatabase> interfaceUpdatesQueue,
    messaging::RQueue<thrift::InitializationEvent> initializationEventQueue,
    messaging::RQueue<AddressEvent> addrEventQueue,
    messaging::ReplicateQueue<NeighborInitEvent>& neighborUpdatesQueue,
    std::shared_ptr<IoProvider> ioProvider,
    std::shared_ptr<const Config> config,
    std::pair<uint32_t, uint32_t> version,
    std::optional<uint32_t> maybeMaxAllowedPps)
    : myNodeName_(config->getNodeName()),
      neighborDiscoveryPort_(
          static_cast<uint16_t>(
              *config->getSparkConfig().neighbor_discovery_port())),
      helloTime_(
          std::chrono::seconds(*config->getSparkConfig().hello_time_s())),
      fastInitHelloTime_(
          std::chrono::milliseconds(
              *config->getSparkConfig().fastinit_hello_time_ms())),
      handshakeTime_(
          std::chrono::milliseconds(
              *config->getSparkConfig().fastinit_hello_time_ms())),
      minNeighborDiscoveryInterval_(
          std::chrono::seconds(
              *config->getSparkConfig().min_neighbor_discovery_interval_s())),
      maxNeighborDiscoveryInterval_(
          std::chrono::seconds(
              *config->getSparkConfig().max_neighbor_discovery_interval_s())),
      handshakeHoldTime_(
          std::chrono::seconds(*config->getSparkConfig().keepalive_time_s())),
      holdTime_(std::chrono::seconds(*config->getSparkConfig().hold_time_s())),
      gracefulRestartTime_(
          std::chrono::seconds(
              *config->getSparkConfig().graceful_restart_time_s())),
      enableV4_(config->isV4Enabled()),
      v4OverV6Nexthop_(config->isV4OverV6NexthopEnabled()),
      neighborUpdatesQueue_(neighborUpdatesQueue),
      kOpenrCtrlThriftPort_(*config->getThriftServerConfig().openr_ctrl_port()),
      kVersion_(createOpenrVersions(version.first, version.second)),
      ioProvider_(std::move(ioProvider)),
      config_(std::move(config)) {
  CHECK(gracefulRestartTime_ >= holdTime_)
      << "Heartbeat hold-time must be less than GR hold-time.";
  CHECK(helloTime_ > std::chrono::milliseconds(0))
      << "helloMsg interval can't be 0";
  CHECK(fastInitHelloTime_ > std::chrono::milliseconds(0))
      << "fastInit helloMsg interval can't be 0";
  CHECK(fastInitHelloTime_ <= helloTime_)
      << "fastInit helloMsg interval must be smaller than normal interval";
  CHECK(ioProvider_) << "Got null IoProvider";
  CHECK(maxNeighborDiscoveryInterval_ > minNeighborDiscoveryInterval_)
      << "maxNeighborDiscoveryInterval_ must be greater than minNeighborDiscoveryInterval_.";

  // Initialize list of BucketedTimeSeries
  const std::chrono::seconds sec{1};
  if (maybeMaxAllowedPps) {
    maybeMaxAllowedPps_ = maybeMaxAllowedPps;
    const int32_t numBuckets = *maybeMaxAllowedPps_ / 3;
    for (size_t i = 0; i < Constants::kNumTimeSeries; i++) {
      timeSeriesVector_.emplace_back(

          numBuckets, sec);
    }
  }
  // Timer scheduled with lower bound timeout, after which  NEIGHBOR_DISCOVERED
  // initialization signal may be published to LM over neighborUpdatesQueue_.
  minNeighborDiscoveryIntervalTimer_ =
      folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
        XLOG(INFO) << fmt::format(
            "[Initialization] Min neighbor discovery time has elapsed."
            " Active neighbors = {}, total neighbors = {}.",
            numActiveNeighbors_,
            numTotalNeighbors_);

        // If all known neighbors are discovered, send initialization signal.
        if (allNeighborsDiscovered()) {
          maxNeighborDiscoveryIntervalTimer_->cancelTimeout();
          initialNeighborsDiscovered();
        } else {
          // Defer until the pending neighbors become active or the
          // maxNeighborDiscoveryInterval expires.
          XLOG(INFO) << fmt::format(
              "[Initialization] Deferring publishing  NEIGHBOR_DISCOVERED"
              " initialization signal upto {}ms",
              minNeighborDiscoveryInterval_.count());
        }
      });

  // Timer scheduled with upper bound timeout, after which  NEIGHBOR_DISCOVERED
  // initialization signal must be published to LM over neighborUpdatesQueue_.
  maxNeighborDiscoveryIntervalTimer_ =
      folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
        XLOG(INFO) << fmt::format(
            "[Initialization] Max neighbor discovery time has elapsed."
            " Active neighbors = {}, total neighbors = {}.",
            numActiveNeighbors_,
            numTotalNeighbors_);

        // Send initialization signal.
        initialNeighborsDiscovered();
      });

  // Fiber to process interface updates from LinkMonitor
  addFiberTask([q = std::move(interfaceUpdatesQueue), this]() mutable noexcept {
    XLOG(DBG1) << "Starting interface-updates processing task";
    while (true) {
      auto interfaceUpdates = q.get(); // perform read
      if (interfaceUpdates.hasError()) {
        break;
      }

      if (!initialInterfacesReceived_) {
        // Set up min-max interval related timers that control when Spark can
        // publish the NEIGHBOR_DISCOVERED signal to LinkMonitor. The min
        // interval is a lower bound that must elapse before considering
        // publishing this signal while the max interval is an upper bound at
        // which point the signal must be published, if signal has still not
        // been sent.
        minNeighborDiscoveryIntervalTimer_->scheduleTimeout(
            minNeighborDiscoveryInterval_);
        maxNeighborDiscoveryIntervalTimer_->scheduleTimeout(
            maxNeighborDiscoveryInterval_);
        initialInterfacesReceived_ = true;

        XLOG(INFO) << fmt::format(
            "[Initialization] Initial interface update received. Scheduled timer after {}ms",
            minNeighborDiscoveryInterval_.count());
      }
      processInterfaceUpdates(std::move(interfaceUpdates).value());
    }
    XLOG(DBG1) << "[Exit] Interface-updates processing task finished";
  });

  // Fiber to process Open/R initialization event from PrefixManager
  addFiberTask(
      [q = std::move(initializationEventQueue), this]() mutable noexcept {
        XLOG(DBG1) << "Starting initialization event processing task";
        while (true) {
          auto maybeEvent = q.get();
          if (maybeEvent.hasError()) {
            break;
          }
          processInitializationEvent(std::move(maybeEvent).value());
        }
        XLOG(DBG1) << "[Exit] Initialization event processing task finished";
      });

  // Listen to NeighborMonitor if enabled
  if (config_->isNeighborMonitorEnabled()) {
    addFiberTask([q = std::move(addrEventQueue), this]() mutable noexcept {
      XLOG(DBG1) << "Starting arp/ndp entry processing task";
      while (true) {
        auto maybeEvent = q.get();
        if (maybeEvent.hasError()) {
          break;
        }
        processAddressEvent(std::move(maybeEvent).value());
      }
      XLOG(DBG1) << "[Exit] Arp/ndp entry processing task finished";
    });
  }

  // Initialize UDP socket for neighbor discovery
  prepareSocket();

  // Initialize some stat keys
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

/**
 * Signal that Spark has finished discovering all the the known neighbors or the
 * initializationHoldTimer_ has expired.
 */
void
Spark::initialNeighborsDiscovered() {
  neighborUpdatesQueue_.push(thrift::InitializationEvent::NEIGHBOR_DISCOVERED);
  logInitializationEvent(
      "Spark", thrift::InitializationEvent::NEIGHBOR_DISCOVERED);
}

void
Spark::stop() {
  // NOTE: explicitly wait for msg to send out before going down
  XLOG(DBG1) << fmt::format(
                    "[Exit] Send termination signal to stop {} tasks.",
                    getFiberTaskNum())
             << " Notify neighbors for graceful restart shut-down.";
  floodRestartingMsg().get();
  OpenrEventBase::stop();
  XLOG(DBG1) << "[Exit] Successfully stopped Spark eventbase.";
}

void
Spark::prepareSocket() noexcept {
  int fd = ioProvider_->socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  mcastFd_ = fd;

  CHECK_GT(fd, 0);
  if (fd < 0) {
    XLOG(FATAL) << "Failed creating Spark UDP socket. Error: "
                << folly::errnoStr(errno);
  }

  XLOG(INFO) << fmt::format(
      "Created UDP socket for neighbor discovery with fd: {}", mcastFd_);

  // make socket non-blocking
  if (ioProvider_->fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
    XLOG(FATAL) << "Failed making the socket non-blocking. Error: "
                << folly::errnoStr(errno);
  }

  // make v6 only
  int v6Only = 1;
  if (ioProvider_->setsockopt(
          fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6Only, sizeof(v6Only)) != 0) {
    XLOG(FATAL) << "Failed making the socket v6 only. Error: "
                << folly::errnoStr(errno);
  }

  // not really needed, but helps us use same port with other listeners, if any
  int reuseAddr = 1;
  if (ioProvider_->setsockopt(
          fd, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr)) != 0) {
    XLOG(FATAL) << "Failed making the socket reuse addr. Error: "
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
    XLOG(FATAL) << "Failed enabling PKTINFO option. Error: "
                << folly::errnoStr(errno);
  }

  // Set ip-tos
  if (config_->getConfig().ip_tos().has_value()) {
    int ipTos = config_->getConfig().ip_tos().value();
    if (ioProvider_->setsockopt(
            fd, IPPROTO_IPV6, IPV6_TCLASS, &ipTos, sizeof(int)) != 0) {
      XLOG(FATAL) << "Failed setting ip-tos value on socket. Error: "
                  << folly::errnoStr(errno);
    }
  }

  // bind the socket to receive any mcast packet
  {
    auto mcastSockAddr =
        folly::SocketAddress(folly::IPAddress("::"), neighborDiscoveryPort_);

    sockaddr_storage addrStorage;
    mcastSockAddr.getAddress(&addrStorage);
    sockaddr* saddr = reinterpret_cast<sockaddr*>(&addrStorage);

    if (ioProvider_->bind(fd, saddr, mcastSockAddr.getActualSize()) != 0) {
      XLOG(FATAL) << "Failed binding the socket. Error: "
                  << folly::errnoStr(errno);
    }
  }

  // set the TTL to maximum, so we can check for spoofed addresses
  int ttl = kSparkHopLimit;
  if (ioProvider_->setsockopt(
          fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(ttl)) != 0) {
    XLOG(FATAL) << "Failed setting TTL on socket. Error: "
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
    XLOG(FATAL) << "Failed enabling TTL receive on socket. Error: "
                << folly::errnoStr(errno);
  }

  // disable looping packets to ourselves
  const int loop = 0;
  if (ioProvider_->setsockopt(
          fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof(loop)) != 0) {
    XLOG(FATAL) << "Failed disabling looping on socket. Error: "
                << folly::errnoStr(errno);
  }

  // enable timestamping for this socket
  const int enabled = 1;
  if (ioProvider_->setsockopt(
          fd, SOL_SOCKET, SO_TIMESTAMPNS, &enabled, sizeof(enabled)) != 0) {
    XLOG(ERR) << "Failed to enable kernel timestamping. Measured RTTs are "
              << "likely to have more noise in them. Error: "
              << folly::errnoStr(errno);
  }

  // Listen for incoming messages on multicast FD
  addSocketFd(mcastFd_, folly::EventHandler::READ, [this](uint16_t) noexcept {
    try {
      processPacket();
    } catch (std::exception const& err) {
      XLOG(ERR) << "Spark: error processing hello packet "
                << folly::exceptionStr(err);
    }
  });
  XLOG(INFO) << "Attached socket/events callbacks...";

  // update counters every few seconds
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    updateGlobalCounters();
    // Schedule next counters update
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
}

PacketValidationResult
Spark::sanityCheckMsg(
    std::string const& neighborName, std::string const& ifName) {
  // check if own packet has looped
  if (neighborName == myNodeName_) {
    XLOG(DBG3) << fmt::format(
        "[Sanity Check] Ignore packet from self node: {}", myNodeName_);
    fb303::fbData->addStatValue(
        "spark.invalid_keepalive.looped_packet", 1, fb303::SUM);
    return PacketValidationResult::SKIP;
  }

  if (neighborName.empty()) {
    XLOG(DBG3) << "[Sanity Check] Ignore packet from node with no name";
    fb303::fbData->addStatValue(
        "spark.invalid_keepalive.empty_neighbor_name", 1, fb303::SUM);
    return PacketValidationResult::SKIP;
  }

  // interface name check
  if (sparkNeighbors_.find(ifName) == sparkNeighbors_.end()) {
    XLOG(DBG3) << fmt::format(
        "[Sanity Check] Ignoring packet received from {} on unknown interface: {}",
        neighborName,
        ifName);
    return PacketValidationResult::SKIP;
  }
  return PacketValidationResult::SUCCESS;
}

bool
Spark::shouldProcessPacket(
    std::string const& ifName, folly::IPAddress const& addr) {
  if (!maybeMaxAllowedPps_.has_value()) {
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
    XLOG(ERR) << fmt::format(
        "Rejecting packet from {} due to hop-limit: {} being less than: {}",
        clientAddr.getAddressStr(),
        hopLimit,
        kSparkHopLimit);
    return false;
  }

  auto res = findInterfaceFromIfindex(ifIndex);
  if (!res.has_value()) {
    XLOG(ERR) << fmt::format(
        "Received packet from {} with unknown ifIndex: {}. Skip processing.",
        clientAddr.getAddressStr(),
        ifIndex);
    return false;
  }

  // assign value to ifName and pass it back via argument list
  ifName = res.value();

  // update counters for packets received, dropped and processed
  fb303::fbData->addStatValue("spark.packet_recv", 1, fb303::SUM);

  // update counters for total size of packets received
  fb303::fbData->addStatValue("spark.packet_recv_size", bytesRead, fb303::SUM);

  if (!shouldProcessPacket(ifName, clientAddr.getIPAddress())) {
    XLOG(ERR) << fmt::format(
        "Dropping pkt due to rate limiting on iface: {} from addr: {}",
        ifName,
        clientAddr.getAddressStr());

    fb303::fbData->addStatValue("spark.packet_dropped", 1, fb303::SUM);
    return false;
  }

  fb303::fbData->addStatValue("spark.packet_processed", 1, fb303::SUM);

  if (bytesRead >= 0) {
    XLOG(DBG3) << fmt::format(
        "Read a total of {} bytes from fd {}", bytesRead, mcastFd_);

    if (static_cast<size_t>(bytesRead) > kMinIpv6Mtu) {
      XLOG(ERR) << fmt::format(
          "Message from {} has been truncated.", clientAddr.getAddressStr());
      return false;
    }
  } else {
    XLOG(ERR) << fmt::format(
        "Failed reading from fd: {} with error: {}",
        mcastFd_,
        folly::errnoStr(errno));
    return false;
  }

  // Parse buffer into helloPacket.
  try {
    // assign value to pkt and pass it back via argument list
    auto ioBuf = folly::IOBuf::wrapBuffer(buf, bytesRead);
    pkt = readThriftObj<thrift::SparkHelloPacket>(*ioBuf, serializer_);
  } catch (std::out_of_range const& err) {
    XLOG(ERR) << "Malformed Thrift packet: " << folly::exceptionStr(err);
    return false;
  } catch (apache::thrift::protocol::TProtocolException const& err) {
    XLOG(ERR) << "Malformed Thrift packet: " << folly::exceptionStr(err);
    return false;
  } catch (std::exception const& err) {
    XLOG(ERR) << "Failed to parse packet: " << folly::exceptionStr(err);
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
  } catch (const folly::IPAddressFormatException&) {
    XLOG(ERR) << fmt::format(
        "[SparkHandshakeMsg] Invalid ipv4 address from ifName: {}", ifName);
    fb303::fbData->addStatValue(
        "spark.handshake.invalid_v4_addr", 1, fb303::SUM);
    return PacketValidationResult::FAILURE;
  }

  // validate subnet of v4 address
  auto const& neighCidrNetwork =
      fmt::format("{}/{}", toString(neighV4Addr), myV4PrefixLen);

  if (!myV4Addr.inSubnet(neighCidrNetwork)) {
    XLOG(ERR) << "[SparkHandshakeMsg] Neighbor V4 address "
              << toString(neighV4Addr)
              << " is not in the same subnet with local V4 address "
              << myV4Addr.str() << "/" << +myV4PrefixLen;
    fb303::fbData->addStatValue(
        "spark.handshake.different_subnet", 1, fb303::SUM);
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

  // only report RTT change in ESTABLISHED state
  if (sparkNeighbor.state != thrift::SparkNeighState::ESTABLISHED) {
    XLOG(DBG3) << fmt::format(
        "[SparkHelloMsg] Neighbor: {} over iface: {} is in state: {}. "
        "Skip RTT change notification",
        neighborName,
        ifName,
        apache::thrift::util::enumNameSafe(sparkNeighbor.state));
    return;
  }
  // rounding rtt value to milisecond
  auto roundedNewRtt = rttRounding(newRtt);
  // skip if no update
  if (roundedNewRtt == sparkNeighbor.rtt) {
    return;
  }
  // update rtt value
  sparkNeighbor.rtt = roundedNewRtt;

  // notify the rtt changes if use the rtt metric
  if (*config_->getLinkMonitorConfig().use_rtt_metric()) {
    XLOG(DBG1) << fmt::format(
        "[SparkHelloMsg] RTT for neighbor:{} has changed from {}us to {}us over iface: {}",
        neighborName,
        sparkNeighbor.rtt.count(),
        newRtt,
        ifName);
    notifySparkNeighborEvent(
        NeighborEventType::NEIGHBOR_RTT_CHANGE, sparkNeighbor);
  }
}

std::chrono::microseconds
Spark::rttRounding(int64_t const rtt) {
  // Mask off to millisecond accuracy!
  //
  // Reason => For practical Wide Area Networks(WAN) scenario.
  // Having accuracy up to milliseconds is sufficient.
  //
  // Further, load on system can heavily influence rtt measurement in
  // microseconds as we do calculation in user-space. Also when Open/R
  // process restarts on neighbor node, measurement will more likely
  // to be the same as previous one.
  return std::chrono::microseconds(
      std::max(rtt / 1000 * 1000, std::chrono::microseconds(1000).count()));
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
  XLOG(DBG3) << "RTT timestamps in order: " << mySentTime.count() << ", "
             << nbrRecvTime.count() << ", " << nbrSentTime.count() << ", "
             << myRecvTime.count();

  if (!mySentTime.count() || !nbrRecvTime.count()) {
    XLOG(ERR) << "Missing timestamp to deduce RTT";
    return;
  }

  if (nbrSentTime < nbrRecvTime) {
    XLOG(ERR) << "Time anomaly. nbrSentTime: [" << nbrSentTime.count()
              << "] < nbrRecvTime: [" << nbrRecvTime.count() << "]";
    return;
  }

  if (myRecvTime < mySentTime) {
    XLOG(ERR) << "Time anomaly. myRecvTime: [" << myRecvTime.count()
              << "] < mySentTime: [" << mySentTime.count() << "]";
    return;
  }

  // Measure only if neighbor is reflecting our previous hello packet.
  auto rtt = (myRecvTime - mySentTime) - (nbrSentTime - nbrRecvTime);
  XLOG(DBG3) << "Measured new RTT for neighbor " << neighborName
             << " from remote iface " << remoteIfName << " over interface "
             << ifName << " as " << rtt.count() / 1000.0 << "ms.";

  // rounding rtt value to milisecond
  rtt = rttRounding(rtt.count());

  // It is possible for things to go wrong in RTT calculation because of
  // clock adjustment.
  // Next measurements will correct this wrong measurement.
  if (rtt.count() < 0) {
    XLOG(ERR) << "Time anomaly. Measured negative RTT. " << rtt.count() / 1000.0
              << "ms.";
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
        XLOG(DBG2) << "Setting initial value for RTT for sparkNeighbor "
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
    XLOG(ERR) << fmt::format(
        "[SparkHandshakeMsg] Failed sending pkt over: {}", ifName);
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
  handshakeMsg.nodeName() = myNodeName_;
  handshakeMsg.isAdjEstablished() = isAdjEstablished;
  handshakeMsg.holdTime() = holdTime_.count();
  handshakeMsg.gracefulRestartTime() = gracefulRestartTime_.count();
  handshakeMsg.transportAddressV6() = toBinaryAddress(v6Addr);
  handshakeMsg.transportAddressV4() = toBinaryAddress(v4Addr);
  handshakeMsg.openrCtrlThriftPort() = kOpenrCtrlThriftPort_;
  // ATTN: send neighborAreaId deduced locally
  handshakeMsg.area() = neighborAreaId;
  handshakeMsg.neighborNodeName() = neighborName;

  thrift::SparkHelloPacket pkt;
  pkt.handshakeMsg() = std::move(handshakeMsg);

  auto packet = writeThriftObjStr(pkt, serializer_);

  // send the pkt
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()),
      neighborDiscoveryPort_);

  if (kMinIpv6Mtu < packet.size()) {
    XLOG(ERR) << "[SparkHandshakeMsg] Handshake msg is too big. Abort sending.";
    return;
  }

  auto bytesSent = IoProvider::sendMessage(
      mcastFd_, ifIndex, v6Addr.asV6(), dstAddr, packet, ioProvider_.get());

  if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
    XLOG(ERR) << fmt::format(
        "[SparkHandshakeMsg] Failed sending pkt towards: {} over: {} due to error: {}",
        dstAddr.getAddressStr(),
        ifName,
        folly::errnoStr(errno));
    return;
  }

  // update telemetry for SparkHandshakeMsg
  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto& neighbor = ifNeighbors.at(neighborName);
  neighbor.lastHandshakeMsgSentAt = getCurrentTime<std::chrono::milliseconds>();

  fb303::fbData->addStatValue(
      "spark.handshake.bytes_sent", packet.size(), fb303::SUM);
  fb303::fbData->addStatValue("spark.handshake.packet_sent", 1, fb303::SUM);

  XLOG(DBG2) << fmt::format(
      "[SparkHandshakeMsg] Successfully sent {} bytes "
      "over intf: {}, neighbor name: {}, neighbor areaId: {}, "
      "isAdjEstablished: {}",
      bytesSent,
      ifName,
      neighborName,
      neighborAreaId,
      isAdjEstablished);
}

void
Spark::sendHeartbeatMsg(std::string const& ifName) {
  SCOPE_EXIT {
    // increment seq# after packet has been sent (even if it didnt go out)
    ++mySeqNum_;
  };

  SCOPE_FAIL {
    XLOG(ERR) << fmt::format(
        "[SparkHeartbeatMsg] Failed sending pkt over: {}", ifName);
  };

  if (ifNameToActiveNeighbors_.find(ifName) == ifNameToActiveNeighbors_.end()) {
    XLOG(DBG3) << fmt::format(
        "[SparkHeartbeatMsg] Interface: {} does NOT have any active neighbors. Skip sending.",
        ifName);
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
  heartbeatMsg.nodeName() = myNodeName_;
  heartbeatMsg.seqNum() = mySeqNum_;
  heartbeatMsg.holdAdjacency() = false;
  // ATTN: notify peer to set special adjacency flag when node is still within
  // initialization procedure
  heartbeatMsg.holdAdjacency() = (!initialized_);

  thrift::SparkHelloPacket pkt;
  pkt.heartbeatMsg() = std::move(heartbeatMsg);

  auto packet = writeThriftObjStr(pkt, serializer_);

  // send the pkt
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()),
      neighborDiscoveryPort_);

  if (kMinIpv6Mtu < packet.size()) {
    XLOG(ERR) << "[SparkHeartbeatMsg] Heartbeat pkt is too big. Abort sending.";
    return;
  }

  auto bytesSent = IoProvider::sendMessage(
      mcastFd_, ifIndex, v6Addr.asV6(), dstAddr, packet, ioProvider_.get());

  if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
    XLOG(ERR) << fmt::format(
        "[SparkHeartbeatMsg] Failed sending pkt towards: {} over: {} due to error: {}",
        dstAddr.getAddressStr(),
        ifName,
        folly::errnoStr(errno));
    return;
  }

  // update telemetry for SparkHeartbeatMsg
  for (auto& [_, neighbor] : sparkNeighbors_.at(ifName)) {
    neighbor.lastHeartbeatMsgSentAt =
        getCurrentTime<std::chrono::milliseconds>();
  }

  fb303::fbData->addStatValue(
      "spark.heartbeat.bytes_sent", packet.size(), fb303::SUM);
  fb303::fbData->addStatValue("spark.heartbeat.packet_sent", 1, fb303::SUM);

  XLOG(DBG2) << "[SparkHeartbeatMsg] Successfully sent " << bytesSent
             << " bytes over intf: " << ifName
             << ", with sequenceId: " << mySeqNum_;
}

void
Spark::logStateTransition(
    std::string const& neighborName,
    std::string const& ifName,
    thrift::SparkNeighState const& oldState,
    thrift::SparkNeighState const& newState) {
  SYSLOG(INFO)
      << EventTag()
      << fmt::format(
             "State change: [{}] -> [{}] for neighbor: {} on interface: {}",
             apache::thrift::util::enumNameSafe(oldState),
             apache::thrift::util::enumNameSafe(newState),
             neighborName,
             ifName);
}

void
Spark::checkNeighborState(
    SparkNeighbor const& neighbor,
    std::string const& ifName,
    thrift::SparkNeighState const& state) {
  CHECK(neighbor.state == state) << fmt::format(
      "Neighbor: {} on interface {}, expected state: [{}], actual state: [{}]",
      neighbor.nodeName,
      ifName,
      apache::thrift::util::enumNameSafe(state),
      apache::thrift::util::enumNameSafe(neighbor.state));
}

folly::SemiFuture<std::optional<thrift::SparkNeighState>>
Spark::getSparkNeighState(
    std::string const& ifName, std::string const& neighborName) {
  folly::Promise<std::optional<thrift::SparkNeighState>> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread(
      [this, promise = std::move(promise), ifName, neighborName]() mutable {
        if (sparkNeighbors_.find(ifName) == sparkNeighbors_.end()) {
          XLOG(ERR) << "No interface: " << ifName
                    << " in sparkNeighbor collection";
          promise.setValue(std::nullopt);
        } else {
          auto& ifNeighbors = sparkNeighbors_.at(ifName);
          auto neighborIt = ifNeighbors.find(neighborName);
          if (neighborIt == ifNeighbors.end()) {
            XLOG(ERR) << "No neighborName: " << neighborName
                      << " in sparkNeighbor collection";
            promise.setValue(std::nullopt);
          } else {
            auto& neighbor = neighborIt->second;
            promise.setValue(neighbor.state);
          }
        }
      });
  return sf;
}

folly::SemiFuture<folly::Unit>
Spark::floodRestartingMsg() {
  folly::Promise<folly::Unit> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread([this, p = std::move(promise)]() mutable {
    // send out restarting packets for all interfaces before I'm going down
    // here we are sending duplicate restarting packets (kNumRestartingPktSent
    // times per interface) in case some packets get lost
    for (int i = 0; i < kNumRestartingPktSent; ++i) {
      for (const auto& [ifName, _] : interfaceDb_) {
        sendHelloMsg(
            ifName, false /* inFastInitState */, true /* restarting */);
      }
    }
    XLOG(DBG1) << fmt::format(
        "[Exit] Successfully sent restarting message to: {} neighbors.",
        interfaceDb_.size());
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::SparkNeighbor>>>
Spark::getNeighbors() {
  folly::Promise<std::unique_ptr<std::vector<thrift::SparkNeighbor>>> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread([this, p = std::move(promise)]() mutable {
    std::vector<thrift::SparkNeighbor> res;
    for (auto const& [ifName, neighbors] : sparkNeighbors_) {
      for (auto const& [_, neighbor] : neighbors) {
        res.emplace_back(neighbor.toThrift());
      }
    }
    p.setValue(
        std::make_unique<std::vector<thrift::SparkNeighbor>>(std::move(res)));
  });
  return sf;
}

/**
 * Determine if Spark has finished discovering all the the known neighbors.
 * We will assume neighbora are not all discovered if there's no active
 * Neighbors.
 *
 * Returns:
 *  bool: true if discovery is complete, false otherwise.
 */
bool
Spark::allNeighborsDiscovered() {
  return (numActiveNeighbors_ == numTotalNeighbors_) &&
      (numActiveNeighbors_ > 0);
}

/**
 * Add a neighbor as active for an interface.
 *
 * Args:
 *  ifName: Reference to interface name to which neighbor belongs.
 *  neighborName: Name of the neighbor.
 */
void
Spark::addToActiveNeighbors(
    std::string const& ifName, std::string const& neighborName) {
  auto result = ifNameToActiveNeighbors_[ifName].emplace(neighborName);
  if (result.second) {
    numActiveNeighbors_++;
  }
}

/**
 * Remove a neighbor as active for an interface.
 *
 * Args:
 *  ifName: Reference to interface name to which neighbor belongs.
 *  neighborName: Name of the neighbor.
 */
void
Spark::remFromActiveNeighbors(
    std::string const& ifName, std::string const& neighborName) {
  uint8_t erasedCount = ifNameToActiveNeighbors_.at(ifName).erase(neighborName);
  numActiveNeighbors_ -= erasedCount;
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
  addToActiveNeighbors(ifName, neighborName);

  // Notify LinkMonitor about neighbor UP event.
  // ATTN: both WARM_BOOT(GR) and COLD_BOOT shared the SAME:
  // negotiation -> established state transiion.
  // Differentiate them by reporting different types of events.
  if (neighbor.gracefulRestartHoldTimer) {
    // stop the graceful-restart hold-timer
    neighbor.gracefulRestartHoldTimer.reset();

    notifySparkNeighborEvent(NeighborEventType::NEIGHBOR_RESTARTED, neighbor);
  } else {
    /**
     * adjOnlyUsedByOtherNode is used to indicate if neighbor node is coming
     * up for the first time or cold booting and yet not complete with its
     * own initialization. We want to avoid sending traffic to such a neighbor
     * until neighbor has sent heartbeat message to reset this flag.
     *
     * NeighborUp event can also be seen first tiem for the neighbor when
     * local node is restarting. It should not be mistaken to neighbor node
     * coming up and so should not be manipulating adjOnlyUsedByOtherNode
     * flag in such a case. During local node's restart, all adjacencies
     * need to be learned and notified for decision module to build routes
     */
    if (initialized_) {
      // ATTN: expect adjacency attribute to be removed later with heartbeatMsg
      neighbor.adjOnlyUsedByOtherNode = true;

      LOG(INFO) << fmt::format(
          "[Initialization] Mark neighbor: {} only used by other node in adj population",
          neighborName);
    }
    notifySparkNeighborEvent(NeighborEventType::NEIGHBOR_UP, neighbor);
  }
  if (isInitialNeighborDiscoveryComplete()) {
    XLOG(INFO) << fmt::format(
        "[Initialization] All known neighbors ({}) are now discovered.",
        numTotalNeighbors_);
    initialNeighborsDiscovered();
    maxNeighborDiscoveryIntervalTimer_->cancelTimeout();
  }
}

// clang-format off
/**
 * Determine if the initialization process related neighbor discovery is
 * complete. The status of intial discovery is as follows -
 *  -----------------------------------------------------------------------------
 * |         State                                   |        Discovery status   |
 *  -----------------------------------------------------------------------------
 * | before min discovery timer elapses              |  discovering              |
 *  -----------------------------------------------------------------------------
 * | after min discovery timer elapses but before    |  complete if all known    |
 * | max delivery timer elapses                      |  neighbors are discovered |
 *  -----------------------------------------------------------------------------
 * | after max delivery timer elapses                |   complete                |
 *  ------------------------------------------------------------------------------
 *
 * Returns:
 *  bool: true if discovery is complete, false otherwise.
 */
bool
Spark::isInitialNeighborDiscoveryComplete() {
  return maxNeighborDiscoveryIntervalTimer_->isScheduled() &&
      ! minNeighborDiscoveryIntervalTimer_->isScheduled() &&
      allNeighborsDiscovered();
}
// clang-format on

void
Spark::neighborDownWrapper(
    SparkNeighbor const& neighbor,
    std::string const& ifName,
    std::string const& neighborName) {
  // Remove neighborship on this interface.
  if (ifNameToActiveNeighbors_.find(ifName) == ifNameToActiveNeighbors_.end()) {
    XLOG(WARNING) << "Ignore " << ifName << " as there is NO active neighbors.";
    return;
  }

  remFromActiveNeighbors(ifName, neighborName);

  if (ifNameToActiveNeighbors_.at(ifName).empty()) {
    ifNameToActiveNeighbors_.erase(ifName);
  }

  // Notify LinkMonitor about neighbor DOWN state
  notifySparkNeighborEvent(NeighborEventType::NEIGHBOR_DOWN, neighbor);
}

void
Spark::notifySparkNeighborEvent(
    NeighborEventType eventType, SparkNeighbor const& neighbor) {
  neighborUpdatesQueue_.push(NeighborEvents({NeighborEvent(
      eventType,
      neighbor.nodeName,
      neighbor.transportAddressV4,
      neighbor.transportAddressV6,
      neighbor.localIfName,
      neighbor.remoteIfName,
      neighbor.area,
      neighbor.openrCtrlThriftPort,
      neighbor.rtt.count(),
      neighbor.adjOnlyUsedByOtherNode)}));
}

void
Spark::processHeartbeatTimeout(
    std::string const& ifName, std::string const& neighborName) {
  // spark neighbor must exist
  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto& neighbor = ifNeighbors.at(neighborName);

  // remove from tracked neighbor at the end
  SCOPE_EXIT {
    eraseSparkNeighbor(ifNeighbors, neighborName);
  };

  XLOG(INFO) << "Heartbeat timer expired for: " << neighborName
             << " on interface " << ifName;

  // neighbor must in 'ESTABLISHED' state
  checkNeighborState(neighbor, ifName, thrift::SparkNeighState::ESTABLISHED);

  // state transition
  thrift::SparkNeighState oldState = neighbor.state;
  neighbor.state =
      getNextState(oldState, thrift::SparkNeighEvent::HEARTBEAT_TIMER_EXPIRE);
  neighbor.event = thrift::SparkNeighEvent::HEARTBEAT_TIMER_EXPIRE;
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // bring down neighborship and cleanup spark neighbor state
  neighborDownWrapper(neighbor, ifName, neighborName);
}

void
Spark::processNegotiation(
    std::string const& ifName,
    std::string const& neighborName,
    SparkNeighbor& neighbor) {
  // Starts timer to periodically send hankshake msg
  const std::string neighborAreaId = neighbor.area;
  neighbor.negotiateTimer = folly::AsyncTimeout::make(
      *getEvb(), [this, ifName, neighborName, neighborAreaId]() noexcept {
        sendHandshakeMsg(ifName, neighborName, neighborAreaId, false);
        // send out handshake msg periodically to this neighbor
        auto ifNameIt = sparkNeighbors_.find(ifName);
        CHECK(ifNameIt != sparkNeighbors_.end())
            << fmt::format("Key NOT found for: {}", ifName);
        auto neighborNameIt = ifNameIt->second.find(neighborName);
        CHECK(neighborNameIt != ifNameIt->second.end())
            << fmt::format("Key NOT found: {} under: {}", neighborName, ifName);
        neighborNameIt->second.negotiateTimer->scheduleTimeout(handshakeTime_);
      });
  neighbor.negotiateTimer->scheduleTimeout(handshakeTime_);

  // Starts negotiate hold-timer
  neighbor.negotiateHoldTimer = folly::AsyncTimeout::make(
      *getEvb(), [this, ifName, neighborName]() noexcept {
        // prevent to stucking in NEGOTIATE forever
        processNegotiateTimeout(ifName, neighborName);
      });
  neighbor.negotiateHoldTimer->scheduleTimeout(handshakeHoldTime_);
}

void
Spark::processNegotiateTimeout(
    std::string const& ifName, std::string const& neighborName) {
  // spark neighbor must exist if the negotiate hold-time expired
  auto& neighbor = sparkNeighbors_.at(ifName).at(neighborName);

  XLOG(INFO) << fmt::format(
      "[SparkHandshakeMsg] Negotiate timer expired for: {} over intf: {}",
      neighborName,
      ifName);

  // neighbor must in 'NEGOTIATE' state
  checkNeighborState(neighbor, ifName, thrift::SparkNeighState::NEGOTIATE);

  // state transition
  thrift::SparkNeighState oldState = neighbor.state;
  neighbor.state =
      getNextState(oldState, thrift::SparkNeighEvent::NEGOTIATE_TIMER_EXPIRE);
  neighbor.event = thrift::SparkNeighEvent::NEGOTIATE_TIMER_EXPIRE;
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
    eraseSparkNeighbor(ifNeighbors, neighborName);
  };

  XLOG(INFO) << fmt::format(
      "[SparkHelloMsg] Graceful restart timer expired for: {} over intf: {}",
      neighborName,
      ifName);

  // state transition
  thrift::SparkNeighState oldState = neighbor.state;
  neighbor.state =
      getNextState(oldState, thrift::SparkNeighEvent::GR_TIMER_EXPIRE);
  neighbor.event = thrift::SparkNeighEvent::GR_TIMER_EXPIRE;
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
  notifySparkNeighborEvent(NeighborEventType::NEIGHBOR_RESTARTING, neighbor);

  // start graceful-restart timer
  neighbor.gracefulRestartHoldTimer = folly::AsyncTimeout::make(
      *getEvb(), [this, ifName, neighborName]() noexcept {
        // change the state back to IDLE
        processGRTimeout(ifName, neighborName);
      });
  neighbor.gracefulRestartHoldTimer->scheduleTimeout(
      neighbor.gracefulRestartHoldTime);

  // state transition
  thrift::SparkNeighState oldState = neighbor.state;
  neighbor.state =
      getNextState(oldState, thrift::SparkNeighEvent::HELLO_RCVD_RESTART);
  neighbor.event = thrift::SparkNeighEvent::HELLO_RCVD_RESTART;
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // neihbor is restarting, shutdown heartbeat hold timer
  neighbor.heartbeatHoldTimer.reset();
}

/**
 * Remove a neighbor known on an interface.
 *
 * Args:
 *  ifNeighbors: Container storing known SparkNeighbors on an interface (keyed
 *               by neighborName)
 *  neighborName: Name of the neighbor.
 */
void
Spark::eraseSparkNeighbor(
    std::unordered_map<std::string, SparkNeighbor>& ifNeighbors,
    std::string const& neighborName) {
  uint8_t erasedNeighborCount = ifNeighbors.erase(neighborName);
  numTotalNeighbors_ -= erasedNeighborCount;
}

/**
 * Get the count of all active neighbors.
 *
 * Returns:
 *  uint64_t: Count of all active neighbors.
 */
uint64_t
Spark::getTotalNeighborCount() {
  return numTotalNeighbors_;
}

/**
 * Get the count of all active neighbors.
 *
 * Returns:
 *  uint64_t: Count of all active neighbors.
 */
uint64_t
Spark::getActiveNeighborCount() {
  return numActiveNeighbors_;
}

void
Spark::updateKeepAliveTimer(
    std::chrono::milliseconds updatedHoldTime, const std::string& ifName) {
  // heartBeatTimer was started when intf came up
  auto heartbeatTimer = folly::AsyncTimeout::make(
      *getEvb(), [this, ifName, updatedHoldTime]() noexcept {
        sendHeartbeatMsg(ifName);
        // schedule heartbeatTimers periodically as soon as intf is UP
        ifNameToHeartbeatTimers_.at(ifName)->scheduleTimeout(
            addJitter<std::chrono::milliseconds>(updatedHoldTime / 3));
      });

  // update the heartbeat timer
  ifNameToHeartbeatTimers_.insert_or_assign(ifName, std::move(heartbeatTimer));
  ifNameToHeartbeatTimers_.at(ifName)->scheduleTimeout(
      addJitter<std::chrono::milliseconds>(updatedHoldTime / 3));
}

void
Spark::processHelloMsg(
    thrift::SparkHelloMsg const& helloMsg,
    std::string const& ifName,
    std::chrono::microseconds const& myRecvTimeInUs) {
  auto const& neighborName = *helloMsg.nodeName();
  auto const& remoteIfName = *helloMsg.ifName();
  auto const& neighborInfos = *helloMsg.neighborInfos();
  auto const& remoteVersion = static_cast<uint32_t>(*helloMsg.version());
  auto const& remoteSeqNum = static_cast<uint64_t>(*helloMsg.seqNum());
  auto const& nbrSentTimeInUs =
      std::chrono::microseconds(*helloMsg.sentTsInUs());
  auto const& solicitResponse = *helloMsg.solicitResponse();
  auto const& restarting = *helloMsg.restarting();
  auto const& addressChange = helloMsg.addressChange().value_or(false);

  XLOG(DBG2) << "[SparkHelloMsg] Received pkt over intf: " << ifName
             << ", solicitResponse: " << std::boolalpha << solicitResponse
             << ", restarting flag: " << std::boolalpha << restarting
             << ", remote sequenceId: " << remoteSeqNum;

  // sanity check for SparkHelloMsg
  auto sanityCheckResult = sanityCheckMsg(neighborName, ifName);
  if (PacketValidationResult::SUCCESS != sanityCheckResult) {
    return;
  }
  if (remoteIfName.empty()) {
    XLOG(DBG2) << "[SparkHelloMsg] Ignore packet from " << ifName
               << " : empty remoteIfName";
    return;
  }

  // version check
  if (remoteVersion <
      static_cast<uint32_t>(*kVersion_.lowestSupportedVersion())) {
    XLOG(ERR) << "[SparkHelloMsg] Unsupported version: " << remoteVersion
              << " from: " << neighborName
              << ", must be >= " << *kVersion_.lowestSupportedVersion();
    fb303::fbData->addStatValue("spark.hello.invalid_version", 1, fb303::SUM);
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
    auto areaId = getNeighborArea(neighborName, ifName, config_->getAreas());
    if (!areaId.has_value()) {
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
            *config_->getSparkConfig().step_detector_conf(),
            neighborName, // neighborNode name
            ifName, // interface name which neighbor is discovered on
            remoteIfName, // remote interface on neighborNode
            remoteSeqNum, // seqNum reported by neighborNode
            holdTime_ / 3, // stepDetector sample period
            std::move(rttChangeCb),
            areaId.value()));
    numTotalNeighbors_++;

    auto& neighbor = ifNeighbors.at(neighborName);
    checkNeighborState(neighbor, ifName, thrift::SparkNeighState::IDLE);
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
        std::chrono::microseconds(*ts.lastNbrMsgSentTsInUs()),
        // recvTime of my helloPkt recorded by neighbor
        std::chrono::microseconds(*ts.lastMyMsgRcvdTsInUs()),
        // sentTime of neighbor helloPkt
        nbrSentTimeInUs,
        neighborName,
        remoteIfName,
        ifName);
  }

  XLOG(DBG3) << fmt::format(
      "[SparkHelloMsg] Current state for neighbor: {} is: [{}]",
      neighborName,
      apache::thrift::util::enumNameSafe(neighbor.state));

  // for neighbor in fast initial state and does not see us yet,
  // reply for quick convergence
  if (*helloMsg.solicitResponse()) {
    XLOG(DBG2) << fmt::format(
        "[SparkHelloMsg] Neighbor: {} is soliciting response. Reply immediately.",
        neighborName);
    sendHelloMsg(ifName);
  }

  if (neighbor.state == thrift::SparkNeighState::IDLE) {
    // state transition
    thrift::SparkNeighState oldState = neighbor.state;
    neighbor.state =
        getNextState(oldState, thrift::SparkNeighEvent::HELLO_RCVD_NO_INFO);
    neighbor.event = thrift::SparkNeighEvent::HELLO_RCVD_NO_INFO;
    logStateTransition(neighborName, ifName, oldState, neighbor.state);
  } else if (neighbor.state == thrift::SparkNeighState::WARM) {
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
        static_cast<uint64_t>(*neighborInfos.at(myNodeName_).seqNum());
    if (myRemoteSeqNum >= mySeqNum_) {
      XLOG(DBG2)
          << "[SparkHelloMsg] Seeing my previous incarnation from neighbor: "
          << neighborName << ". Seen Seq# from neighbor: " << myRemoteSeqNum
          << ", my Seq#: " << mySeqNum_;
      return;
    }

    // Start the negotiation process to exchange attributes with neighbor
    processNegotiation(ifName, neighborName, neighbor);

    // Neighbor is aware of us. Promote to NEGOTIATE state
    thrift::SparkNeighState oldState = neighbor.state;
    neighbor.state =
        getNextState(oldState, thrift::SparkNeighEvent::HELLO_RCVD_INFO);
    neighbor.event = thrift::SparkNeighEvent::HELLO_RCVD_INFO;
    logStateTransition(neighborName, ifName, oldState, neighbor.state);
  } else if (neighbor.state == thrift::SparkNeighState::ESTABLISHED) {
    const auto curSeqNum = neighbor.seqNum;
    // Update local seqNum maintained for this neighbor
    neighbor.seqNum = remoteSeqNum;

    // Check if neighbor is undergoing 'Graceful-Restart'
    if (restarting) {
      XLOG(INFO) << "[SparkHelloMsg] Adjacent neighbor: " << neighborName
                 << ", from remote interface: " << remoteIfName
                 << ", on interface: " << ifName << " is restarting.";
      processGRMsg(neighborName, ifName, neighbor);
      return;
    }

    /*
     * Open/R will try to address one of the possible partial-adjacency case,
     * aka, when there is uni-directional link between node A and B showed as
     * the following:

     *      ---X-->
     * A                B
     *      <------

     * A will still recognize B as neighbor, whereas B loses neighbor-ship from
     * A.
     * Hence A will received B's helloMsg, which does not contain A as neighbor.
     * A knows B may have troubles receiving A's keepalive. A will tear down B's
     * neighbor-ship to keep both side in sync.

     * To avoid false alarm under graceful-restart(GR) circumstance, aka, A
     * didn't receive B's restarting signal and keep B in ESTABLISHED state
     * before hold timer expiration, Open/R will double confirm the
     * sequence number received from peer.

     * - Case1: currSeqNum < remoteSeqNum
     *       => peer is not restarting since seqNum monotonically increasing.
     *       [Result]   Tear down.
     * - Case2: currSeqNum >= remoteSeqNum
     *       => highly likely peer is restarted/unexpected restarting but
     *          missed restarting signal.
     *       [Result]   Keep peer in ESTABLISHED state and let hold timer
     *                  takes care of neighbor state.
     */
    if ((curSeqNum < remoteSeqNum) &&
        (tsIt == neighborInfos.end() || addressChange)) {
      thrift::SparkNeighState oldState = neighbor.state;
      neighbor.state =
          getNextState(oldState, thrift::SparkNeighEvent::HELLO_RCVD_NO_INFO);
      neighbor.event = thrift::SparkNeighEvent::HELLO_RCVD_NO_INFO;
      logStateTransition(neighborName, ifName, oldState, neighbor.state);

      // bring down neighborship and cleanup spark neighbor state
      neighborDownWrapper(neighbor, ifName, neighborName);

      // remove from tracked neighbor at the end
      eraseSparkNeighbor(ifNeighbors, neighborName);

      if (addressChange) {
        // create hello timer over this interface with fast neighbor discovery
        setupFastDiscoveryHelloTimer(ifName);
      }
    }
  } else if (neighbor.state == thrift::SparkNeighState::RESTART) {
    // Neighbor is undergoing restart. Will reply immediately for hello msg for
    // quick adjacency establishment.
    if (tsIt == neighborInfos.end()) {
      // Neighbor is NOT aware of us, ignore helloMsg
      return;
    }

    if (neighbor.seqNum < remoteSeqNum) {
      // By going here, it means this node missed ALL of the helloMsg sent-out
      // after neighbor 'restarting' itself. Will let GR timer to handle it.
      XLOG(WARNING) << "[SparkHelloMsg] Unexpected Seq#:" << remoteSeqNum
                    << " received from neighbor: " << neighborName
                    << ", local Seq#: " << neighbor.seqNum;
      return;
    }

    // Neighbor is back from 'restarting' state. Go back to 'ESTABLISHED'
    XLOG(DBG1) << "[SparkHelloMsg] Node: " << neighborName
               << " is back from restart. " << "Received Seq#: " << remoteSeqNum
               << ", local Seq#: " << neighbor.seqNum;

    // Update local seqNum maintained for this neighbor
    neighbor.seqNum = remoteSeqNum;

    // Start the negotiation process to exchange attributes with neighbor
    processNegotiation(ifName, neighborName, neighbor);

    thrift::SparkNeighState oldState = neighbor.state;
    neighbor.state =
        getNextState(oldState, thrift::SparkNeighEvent::HELLO_RCVD_INFO);
    neighbor.event = thrift::SparkNeighEvent::HELLO_RCVD_INFO;
    logStateTransition(neighborName, ifName, oldState, neighbor.state);
  }
}

void
Spark::processHandshakeMsg(
    thrift::SparkHandshakeMsg const& handshakeMsg, std::string const& ifName) {
  // sanity check for SparkHandshakeMsg
  auto const& neighborName = *handshakeMsg.nodeName();
  auto sanityCheckResult = sanityCheckMsg(neighborName, ifName);
  if (PacketValidationResult::SUCCESS != sanityCheckResult) {
    return;
  }

  // Ignore handshakeMsg if I am NOT the receiver as AREA negotiation
  // is point-to-point
  if (auto neighborNodeName = handshakeMsg.neighborNodeName()) {
    if (*neighborNodeName != myNodeName_) {
      XLOG(DBG4) << fmt::format(
          "[SparkHandshakeMsg] Ignoring msg targeted for node: {}, my node name: {}",
          *neighborNodeName,
          myNodeName_);
      return;
    }
  }

  // under quick flapping of Openr, msg can come out-of-order.
  // handshakeMsg will ONLY be processed when:
  //  1). neighbor is tracked on ifName;
  //  2). neighbor is under NEGOTIATE stage;
  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto neighborIt = ifNeighbors.find(neighborName);
  if (neighborIt == ifNeighbors.end()) {
    XLOG(DBG3) << "[SparkHandshakeMsg] Neighbor: " << neighborName
               << " is NOT found.";
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
  if (!(*handshakeMsg.isAdjEstablished())) {
    sendHandshakeMsg(
        ifName,
        neighborName,
        neighbor.area,
        neighbor.state != thrift::SparkNeighState::NEGOTIATE);
  }

  // skip NEGOTIATE step if neighbor is NOT in state. This can happen:
  //  1). negotiate hold timer already expired;
  //  2). v4 validation failed and fall back to WARM;
  if (neighbor.state != thrift::SparkNeighState::NEGOTIATE) {
    XLOG(DBG3) << fmt::format(
        "[SparkHandshakeMsg] Current state of neighbor: {} is [{}], expected state: [NEGOTIIATE]",
        neighborName,
        apache::thrift::util::enumNameSafe(neighbor.state));
    return;
  }

  // update Spark neighborState
  neighbor.openrCtrlThriftPort = *handshakeMsg.openrCtrlThriftPort();
  neighbor.transportAddressV4 = *handshakeMsg.transportAddressV4();
  neighbor.transportAddressV6 = *handshakeMsg.transportAddressV6();

  // update neighbor holdTime as "NEGOTIATING" process
  neighbor.heartbeatHoldTime =
      std::min(std::chrono::milliseconds(*handshakeMsg.holdTime()), holdTime_);
  neighbor.gracefulRestartHoldTime = std::min(
      std::chrono::milliseconds(*handshakeMsg.gracefulRestartTime()),
      gracefulRestartTime_);

  // peer has a lower hold time value. We will reconfigure the heartbeat timer
  // to honor a higher frequency of sending heartbeat.
  // TODO: we can consider using fiber task/corotine to manage the keepalive.
  if (neighbor.heartbeatHoldTime < holdTime_) {
    updateKeepAliveTimer(neighbor.heartbeatHoldTime, ifName);
  }

  // v4 subnet validation if v4 is enabled. If we're using v4-over-v6 we no
  // longer need to validate the address reported by neighbor node
  if (enableV4_ && !v4OverV6Nexthop_) {
    if (PacketValidationResult::FAILURE ==
        validateV4AddressSubnet(ifName, *handshakeMsg.transportAddressV4())) {
      // state transition
      thrift::SparkNeighState oldState = neighbor.state;
      neighbor.state =
          getNextState(oldState, thrift::SparkNeighEvent::NEGOTIATION_FAILURE);
      neighbor.event = thrift::SparkNeighEvent::NEGOTIATION_FAILURE;
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
  if (neighbor.area != *handshakeMsg.area()) {
    if (*handshakeMsg.area() == Constants::kDefaultArea.toString() ||
        neighbor.area == Constants::kDefaultArea.toString()) {
      // for backward compatibility: if the peer is still advertising
      // default area (0), it would be considered a match
      fb303::fbData->addStatValue(
          "spark.hello.default_area_rcvd", 1, fb303::SUM);
    } else {
      XLOG(ERR) << fmt::format(
          "[SparkHandshakeMsg] Inconsistent areaId deduced. "
          "Neighbor's areaId is {} and my areaId from remote is {}.",
          *handshakeMsg.area(),
          neighbor.area);

      // state transition
      thrift::SparkNeighState oldState = neighbor.state;
      neighbor.state =
          getNextState(oldState, thrift::SparkNeighEvent::NEGOTIATION_FAILURE);
      neighbor.event = thrift::SparkNeighEvent::NEGOTIATION_FAILURE;
      logStateTransition(neighborName, ifName, oldState, neighbor.state);

      // stop sending out handshake msg, no longer in NEGOTIATE stage
      neighbor.negotiateTimer.reset();
      // remove negotiate hold timer, no longer in NEGOTIATE stage
      neighbor.negotiateHoldTimer.reset();
      return;
    }
  }

  // state transition
  XLOG(DBG1) << fmt::format(
      "[SparkHandshakeMsg] Successfully negotiated with peer: {} with "
      "TCP port: {}, holdTime: {}ms, gracefulRestartHoldTime: {}ms",
      neighborName,
      neighbor.openrCtrlThriftPort,
      neighbor.heartbeatHoldTime.count(),
      neighbor.gracefulRestartHoldTime.count());

  thrift::SparkNeighState oldState = neighbor.state;
  neighbor.state =
      getNextState(oldState, thrift::SparkNeighEvent::HANDSHAKE_RCVD);
  neighbor.event = thrift::SparkNeighEvent::HANDSHAKE_RCVD;
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // bring up neighborship and set corresponding spark state
  neighborUpWrapper(neighbor, ifName, neighborName);
}

void
Spark::processHeartbeatMsg(
    thrift::SparkHeartbeatMsg const& heartbeatMsg, std::string const& ifName) {
  auto const& remoteSeqNum = *heartbeatMsg.seqNum();

  XLOG(DBG3) << "[SparkHeartbeatMsg] Received SparkHeartbeatMsg over intf: "
             << ifName << ", remote sequenceId: " << remoteSeqNum;

  // sanity check for SparkHandshakeMsg
  auto const& neighborName = *heartbeatMsg.nodeName();
  auto sanityCheckResult = sanityCheckMsg(neighborName, ifName);
  if (PacketValidationResult::SUCCESS != sanityCheckResult) {
    return;
  }

  // under GR case, when node restarts, it will needs several helloMsg to
  // establish neighborship. During this time, heartbeatMsg from peer
  // will NOT be processed.
  auto& ifNeighbors = sparkNeighbors_.at(ifName);
  auto neighborIt = ifNeighbors.find(neighborName);
  if (neighborIt == ifNeighbors.end()) {
    XLOG(DBG3) << "[SparkHeartbeatMsg] I am NOT aware of neighbor: "
               << neighborName << ". Ignore it.";
    return;
  }

  auto& neighbor = neighborIt->second;

  // In case receiving heartbeat msg when it is NOT in established state,
  // Just ignore it.
  if (neighbor.state != thrift::SparkNeighState::ESTABLISHED) {
    XLOG(DBG3) << fmt::format(
        "[SparkHeartbeatMsg] Received heartbeat. Expected state: [ESTABLISHED], current state of neighbor: {} is: [{}]",
        neighborName,
        apache::thrift::util::enumNameSafe(neighbor.state));
    // Only if the neighbor is in Warm state, we need a helloMsg to unblock
    // ourselve to transition this neighbor to established quickly
    if (neighbor.state == thrift::SparkNeighState::WARM) {
      XLOG(DBG3)
          << "[SparkHelloMsg] Sending HelloMsg to solicit a response immediately";
      sendHelloMsg(
          ifName,
          true /* fast init (solicit response) */,
          false /* restarting */);
    }
    return;
  }

  // Reset the hold-timer for neighbor as we have received a keep-alive msg
  neighbor.heartbeatHoldTimer->scheduleTimeout(neighbor.heartbeatHoldTime);

  // Check adjOnlyUsedByOtherNode bit to report to LinkMonitor
  if (neighbor.shouldResetAdjacency(heartbeatMsg)) {
    neighbor.adjOnlyUsedByOtherNode = false;
    notifySparkNeighborEvent(NeighborEventType::NEIGHBOR_ADJ_SYNCED, neighbor);

    LOG(INFO) << fmt::format(
        "[Initialization] Reset flag for neighbor: {} to mark adj to be used globally",
        neighborName);
  }
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
  if (helloPacket.helloMsg().has_value()) {
    processHelloMsg(helloPacket.helloMsg().value(), ifName, myRecvTime);
  } else if (helloPacket.heartbeatMsg().has_value()) {
    processHeartbeatMsg(helloPacket.heartbeatMsg().value(), ifName);
  } else if (helloPacket.handshakeMsg().has_value()) {
    processHandshakeMsg(helloPacket.handshakeMsg().value(), ifName);
  }
}

void
Spark::sendHelloMsg(
    std::string const& ifName,
    bool inFastInitState,
    bool restarting,
    bool addressChange) {
  if (!interfaceDb_.count(ifName)) {
    XLOG(ERR) << fmt::format(
        "[SparkHelloMsg] Interface: {} is no longer being tracked.", ifName);
    return;
  }

  SCOPE_EXIT {
    // increment seq# after packet has been sent (even if it didnt go out)
    ++mySeqNum_;
  };

  SCOPE_FAIL {
    XLOG(ERR)
        << fmt::format("[SparkHelloMsg] Failed sending pkt over: {}", ifName);
  };

  // in some cases, getting link-local address may fail and throw
  // e.g. when iface has not yet auto-configured it, or iface is removed but
  // down event has not arrived yet
  const auto& interfaceEntry = interfaceDb_.at(ifName);
  const auto ifIndex = interfaceEntry.ifIndex;
  const auto v6Addr = interfaceEntry.v6LinkLocalNetwork.first;
  thrift::OpenrVersion openrVer(*kVersion_.version());

  // build the helloMsg from scratch
  thrift::SparkHelloMsg helloMsg;
  helloMsg.nodeName() = myNodeName_;
  helloMsg.ifName() = ifName;
  helloMsg.seqNum() = mySeqNum_;
  helloMsg.neighborInfos() =
      std::map<std::string, thrift::ReflectedNeighborInfo>{};
  helloMsg.version() = openrVer;
  helloMsg.solicitResponse() = inFastInitState;
  helloMsg.restarting() = restarting;
  helloMsg.addressChange() = addressChange;
  helloMsg.sentTsInUs() = getCurrentTime<std::chrono::microseconds>().count();

  // bake neighborInfo into helloMsg
  for (auto& [neighborName, neighbor] : sparkNeighbors_.at(ifName)) {
    auto& neighborInfo = helloMsg.neighborInfos()[neighborName];
    neighborInfo.seqNum() = neighbor.seqNum;
    neighborInfo.lastNbrMsgSentTsInUs() = neighbor.neighborTimestamp.count();
    neighborInfo.lastMyMsgRcvdTsInUs() = neighbor.localTimestamp.count();

    // update telemetry for SparkHelloMsg
    neighbor.lastHelloMsgSentAt = getCurrentTime<std::chrono::milliseconds>();
  }

  // fill in helloMsg field
  thrift::SparkHelloPacket helloPacket;
  helloPacket.helloMsg() = std::move(helloMsg);

  // send the payload
  auto packet = writeThriftObjStr(helloPacket, serializer_);
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()),
      neighborDiscoveryPort_);

  if (kMinIpv6Mtu < packet.size()) {
    XLOG(ERR) << "[SparkHelloMsg] Hello msg is too big. Abort sending.";
    return;
  }

  auto bytesSent = IoProvider::sendMessage(
      mcastFd_, ifIndex, v6Addr.asV6(), dstAddr, packet, ioProvider_.get());

  if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
    XLOG(ERR) << fmt::format(
        "[SparkHelloMsg] Failed sending pkt towards: {} over: {} due to error: {}",
        dstAddr.getAddressStr(),
        ifName,
        folly::errnoStr(errno));
    return;
  }

  // update telemetry for SparkHelloMsg
  fb303::fbData->addStatValue(
      "spark.hello.bytes_sent", packet.size(), fb303::SUM);
  fb303::fbData->addStatValue("spark.hello.packet_sent", 1, fb303::SUM);

  XLOG(DBG2) << "[SparkHelloMsg] Successfully sent " << bytesSent
             << " bytes over intf: " << ifName
             << ", fast-init mode: " << std::boolalpha << inFastInitState
             << ", restarting: " << std::boolalpha << restarting
             << ", sequenceId: " << mySeqNum_;
}

void
Spark::setupFastDiscoveryHelloTimer(std::string const& ifName) {
  auto timePoint = std::chrono::steady_clock::now();

  // NOTE: We do not send hello packet immediately after adding new interface
  // this is due to the fact that it may not have yet configured a link-local
  // address. The hello packet will be sent later and will have good chances
  // of making it out if small delay is introduced.
  auto helloTimer = folly::AsyncTimeout::make(
      *getEvb(), [this, ifName, timePoint]() mutable noexcept {
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
            inFastInitState ? fastInitHelloTime_ : helloTime_;

        ifNameToHelloTimers_.at(ifName)->scheduleTimeout(
            addJitter<std::chrono::milliseconds>(timeoutPeriod));
      });

  // should be in fast init state when the node just starts
  helloTimer->scheduleTimeout(
      addJitter<std::chrono::milliseconds>(fastInitHelloTime_));
  ifNameToHelloTimers_[ifName] = std::move(helloTimer);
}

void
Spark::processInitializationEvent(thrift::InitializationEvent&& event) {
  CHECK(event == thrift::InitializationEvent::PREFIX_DB_SYNCED) << fmt::format(
      "Unexpected initialization event: {}",
      apache::thrift::util::enumNameSafe(event));

  // ATTN: must toggle this flag before sending SparkHeartbeatMsg
  initialized_ = true;

  // force to send SparkHeartbeatMsg immediately to notify peers.
  // NOTE: it is ok for this pkt to be lost as we will continuously send it as
  // the name suggests.
  for (const auto& [ifName, _] : interfaceDb_) {
    sendHeartbeatMsg(ifName);
  }

  // logging for initialization stage duration computation
  logInitializationEvent("Spark", thrift::InitializationEvent::INITIALIZED);
}

void
Spark::processAddressEvent(AddressEvent&& event) {
  // Process Address Event reported by neighborMonitor(FSDB)
  const auto& v6Addr = event.v6Addr;
  const auto& ifName = event.ifName;

  // DOWN -> UP. Perform fast neighbor discovery.
  if (event.resolvable) {
    XLOG(INFO) << fmt::format(
        "Bringing up neighbor over {} with reachability resolved", ifName);
    setupFastDiscoveryHelloTimer(ifName);
    return;
  }

  // UP -> DOWN. Find the unresolvable neighbor by comparing ifName and address
  auto it = sparkNeighbors_.find(ifName);
  if (it == sparkNeighbors_.end()) {
    // Skip processing since this is unknown neighbor
    return;
  }

  auto& ifNeighbors = it->second;
  for (const auto& [name, neighbor] : ifNeighbors) {
    if (neighbor.transportAddressV6 == v6Addr) {
      XLOG(INFO) << fmt::format(
          "Bringing down neighbor {} over {} due to unreachability",
          name,
          ifName);
      neighborDownWrapper(neighbor, ifName, name);
      eraseSparkNeighbor(ifNeighbors, name);
      return;
    }
  }
}

void
Spark::processInterfaceUpdates(InterfaceDatabase&& ifDb) {
  decltype(interfaceDb_) newInterfaceDb{};

  //
  // To be conisdered a valid interface for Spark to track, it must:
  // - be up
  // - have a v6LinkLocal IP
  // - have an IPv4 addr when v4 is enabled
  //
  for (const auto& info : ifDb) {
    // ATTN: multiple networks can be associated with one ifName.
    //  - Retrieve networks in sorted order;
    //  - Use the lowest one (other node will do similar)
    const auto v4Networks = info.getSortedV4Addrs();
    const auto v6LinkLocalNetworks = info.getSortedV6LinkLocalAddrs();

    if (!info.isUp) {
      continue;
    }
    if (v6LinkLocalNetworks.empty()) {
      XLOG(DBG2) << "IPv6 link local address not found";
      continue;
    }
    if (enableV4_ && v4Networks.empty()) {
      XLOG(DBG2) << "IPv4 enabled but no IPv4 addresses are configured";
      continue;
    }

    folly::CIDRNetwork v4Network = enableV4_
        ? *v4Networks.begin()
        : folly::IPAddress::createNetwork("0.0.0.0/32");
    folly::CIDRNetwork v6LinkLocalNetwork = *v6LinkLocalNetworks.begin();

    newInterfaceDb.emplace(
        info.ifName, Interface(info.ifIndex, v4Network, v6LinkLocalNetwork));
  }

  std::vector<std::string> toAdd{};
  std::vector<std::string> toDel{};
  std::vector<std::string> toUpdate{};

  // iterate old and new interfaceDb to catch difference
  for (const auto& [oldIfName, oldInterface] : interfaceDb_) {
    auto it = newInterfaceDb.find(oldIfName);
    if (it != newInterfaceDb.end()) {
      if (it->second != oldInterface) {
        // interface info has changed!
        toUpdate.emplace_back(oldIfName);
      }
    } else {
      // interface being removed!
      toDel.emplace_back(oldIfName);
    }
  }

  for (const auto& [newIfName, _] : newInterfaceDb) {
    auto it = interfaceDb_.find(newIfName);
    if (it == interfaceDb_.end()) {
      // interface being added!
      toAdd.emplace_back(newIfName);
    }
    // ATTN: intersection part has been processed already
  }

  // remove the interfaces no longer in newdb
  deleteInterface(toDel);

  // Adding interfaces
  addInterface(toAdd, newInterfaceDb);

  // Updating interface. If ifindex changes, we need to unsubscribe old ifindex
  // from mcast and subscribe new one
  updateInterface(toUpdate, newInterfaceDb);
}

void
Spark::deleteInterface(const std::vector<std::string>& toDel) {
  for (const auto& ifName : toDel) {
    XLOG(INFO) << "Removing " << ifName << " from Spark. "
               << "It is down, declaring all neighbors down";

    for (const auto& [neighborName, neighbor] : sparkNeighbors_.at(ifName)) {
      XLOG(INFO) << "Neighbor " << neighborName << " removed due to iface "
                 << ifName << " down";

      CHECK(!neighbor.nodeName.empty());
      CHECK(!neighbor.remoteIfName.empty());

      // Spark will NOT notify neighbor DOWN event in following cases:
      //    1). v6Addr is empty for this neighbor;
      //    2). v4 enabled and v4Addr is empty for this neighbor;
      if (neighbor.transportAddressV6.addr()->empty() ||
          ((enableV4_ && !v4OverV6Nexthop_) &&
           neighbor.transportAddressV4.addr()->empty())) {
        continue;
      }
      neighborDownWrapper(neighbor, ifName, neighborName);
    }
    numTotalNeighbors_ -= sparkNeighbors_.at(ifName).size();
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
      XLOG(ERR) << fmt::format(
          "Failed leaving multicast group: {}", folly::errnoStr(errno));
    }
    // cleanup for this interface
    ifNameToHelloTimers_.erase(ifName);
    interfaceDb_.erase(ifName);
  }
}

void
Spark::addInterface(
    const std::vector<std::string>& toAdd,
    const std::unordered_map<std::string, Interface>& newInterfaceDb) {
  for (const auto& ifName : toAdd) {
    auto newInterface = newInterfaceDb.at(ifName);
    auto ifIndex = newInterface.ifIndex;
    CHECK_NE(ifIndex, 0) << "Could not get ifIndex for Iface " << ifName;
    XLOG(INFO) << "Adding iface " << ifName << " for tracking with ifindex "
               << ifIndex;

    // subscribe the socket to mcast address on this interface
    // We throw an error on the first one to encounter a problem
    if (!toggleMcastGroup(
            mcastFd_,
            folly::IPAddress(Constants::kSparkMcastAddr.toString()),
            ifIndex,
            true /* join */,
            ioProvider_.get())) {
      throw std::runtime_error(
          fmt::format(
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
                addJitter<std::chrono::milliseconds>(holdTime_ / 3));
          });

      ifNameToHeartbeatTimers_.emplace(ifName, std::move(heartbeatTimer));
      ifNameToHeartbeatTimers_.at(ifName)->scheduleTimeout(
          addJitter<std::chrono::milliseconds>(holdTime_ / 3));
    }

    // create hello timer over this interface with fast neighbor discovery
    setupFastDiscoveryHelloTimer(ifName);
  }
}

void
Spark::updateInterface(
    const std::vector<std::string>& toUpdate,
    const std::unordered_map<std::string, Interface>& newInterfaceDb) {
  std::vector<std::string> addressChangedInterfaces{};
  for (const auto& ifName : toUpdate) {
    auto& interface = interfaceDb_.at(ifName);
    auto& newInterface = newInterfaceDb.at(ifName);

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
        XLOG(WARNING) << fmt::format(
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
        throw std::runtime_error(
            fmt::format(
                "Failed joining multicast group: {}", folly::errnoStr(errno)));
      }
    }
    XLOG(INFO) << "Updating iface " << ifName << " in spark tracking from "
               << "(ifindex " << interface.ifIndex << ", addrs "
               << interface.v6LinkLocalNetwork.first << " , "
               << interface.v4Network.first << ") to " << "(ifindex "
               << newInterface.ifIndex << ", addrs "
               << newInterface.v6LinkLocalNetwork.first << " , "
               << newInterface.v4Network.first << ")";

    bool v4Changed = interface.v4Network != newInterface.v4Network;
    bool v6Changed =
        interface.v6LinkLocalNetwork != newInterface.v6LinkLocalNetwork;
    interface = std::move(newInterface);
    if (v4Changed || v6Changed) {
      XLOG(INFO) << "Address change detected on ifIndex: " << interface.ifIndex
                 << "for iface: " << ifName;
      addressChangedInterfaces.emplace_back(ifName);
    }
  }
  if (!addressChangedInterfaces.empty()) {
    XLOG(INFO) << "Deleting/Readding " << addressChangedInterfaces.size()
               << " interfaces with updated address.";
    deleteInterface(addressChangedInterfaces);
    addInterface(addressChangedInterfaces, newInterfaceDb);
  }
}

std::optional<std::string>
Spark::findInterfaceFromIfindex(int ifIndex) {
  for (const auto& [ifName, interface] : interfaceDb_) {
    if (interface.ifIndex == ifIndex) {
      return ifName;
    }
  }
  return std::nullopt;
}

void
Spark::updateGlobalCounters() {
  // set some flat counters
  uint64_t adjacentNeighborCount{0}, trackedNeighborCount{0};
  for (auto const& ifaceNeighbors : sparkNeighbors_) {
    trackedNeighborCount += ifaceNeighbors.second.size();
    for (auto const& [_, neighbor] : ifaceNeighbors.second) {
      adjacentNeighborCount +=
          neighbor.state == thrift::SparkNeighState::ESTABLISHED;
      fb303::fbData->setCounter(
          "spark.rtt_latest_us." + neighbor.nodeName,
          neighbor.rttLatest.count());
    }
  }
  fb303::fbData->setCounter(
      "spark.num_tracked_interfaces", sparkNeighbors_.size());

  fb303::fbData->setCounter(
      "spark.num_tracked_neighbors", trackedNeighborCount);
  CHECK_EQ(trackedNeighborCount, numTotalNeighbors_);
  fb303::fbData->setCounter(
      "spark.num_adjacent_neighbors", adjacentNeighborCount);
  // Established neighbor could should be less than active neighbor
  // count (the latter include restarting neighbors too.)
  CHECK_LE(adjacentNeighborCount, numActiveNeighbors_);
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
  // IMPT: ordered set. Function yeilds lowest areaId in case of multiple
  // candidate areas
  std::set<std::string> candidateAreas{};

  // looping through areaIdRegexList
  for (const auto& [areaId, areaConfig] : areaConfigs) {
    if (areaConfig.shouldDiscoverOnIface(localIfName) &&
        areaConfig.shouldPeerWithNeighbor(peerNodeName)) {
      XLOG(DBG1) << fmt::format(
          "Area: {} found for neighbor: {} on interface: {}",
          areaId,
          peerNodeName,
          localIfName);
      candidateAreas.insert(areaId);
    }
  }

  if (candidateAreas.empty()) {
    XLOG(ERR) << "No matching area found for neighbor: " << peerNodeName;
    fb303::fbData->addStatValue("spark.neighbor_no_area", 1, fb303::COUNT);
    return std::nullopt;
  } else if (candidateAreas.size() > 1) {
    XLOG(ERR)
        << "Multiple area found for neighbor: " << peerNodeName
        << ". Will use lowest candidate area: " << *candidateAreas.begin();
    fb303::fbData->addStatValue(
        "spark.neighbor_multiple_area", 1, fb303::COUNT);
  }
  return *candidateAreas.begin();
}

void
Spark::setThrowParserErrors(bool val) {
  isThrowParserErrorsOn_ = val;
}

} // namespace openr
