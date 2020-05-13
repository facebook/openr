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

#include <fb303/ServiceData.h>
#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/GLog.h>
#include <folly/IPAddress.h>
#include <folly/MapUtil.h>
#include <folly/ScopeGuard.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>

#include "IoProvider.h"

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

// number of samples in fast sliding window
const size_t kFastWndSize = 10;

// number of samples in slow sliding window
const size_t kSlowWndSize = 60;

// lower threshold, in percentage
const uint8_t kLoThreshold = 2;

// upper threshold, in percentage
const uint8_t kHiThreshold = 5;

// absolute step threshold, in microseconds
const int64_t kAbsThreshold = 500;

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
    LOG(ERROR) << "setsockopt ipv6_leave_group failed "
               << folly::errnoStr(errno);
    return false;
  }

  LOG(INFO) << "Left multicast addr " << mcastGroup.str() << " on ifindex "
            << ifIndex;

  return true;
}

} // namespace

using namespace fbzmq;

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

Spark::Neighbor::Neighbor(
    thrift::SparkNeighbor const& info,
    uint32_t label,
    uint64_t seqNum,
    std::unique_ptr<fbzmq::ZmqTimeout> holdTimer,
    const std::chrono::milliseconds& samplingPeriod,
    std::function<void(const int64_t&)> rttChangeCb,
    std::string areaId)
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
          rttChangeCb /* callback function */),
      area(areaId) /* area Id */ {
  CHECK_NE(this->holdTimer.get(), static_cast<void*>(nullptr));
}

Spark::Spark2Neighbor::Spark2Neighbor(
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
          samplingPeriod /* sampling period */,
          kFastWndSize /* fast window size */,
          kSlowWndSize /* slow window size */,
          kLoThreshold /* lower threshold */,
          kHiThreshold /* upper threshold */,
          kAbsThreshold /* absolute threshold */,
          rttChangeCb /* callback function */),
      area(adjArea) {
  CHECK(!(this->domainName.empty()));
  CHECK(!(this->nodeName.empty()));
  CHECK(!(this->remoteIfName.empty()));
}

Spark::Spark(
    std::string const& myDomainName,
    std::string const& myNodeName,
    uint16_t const udpMcastPort,
    std::chrono::milliseconds myHoldTime,
    std::chrono::milliseconds myKeepAliveTime,
    std::chrono::milliseconds fastInitKeepAliveTime,
    std::chrono::milliseconds myHelloTime,
    std::chrono::milliseconds myHelloFastInitTime,
    std::chrono::milliseconds myHandshakeTime,
    std::chrono::milliseconds myHeartbeatTime,
    std::chrono::milliseconds myNegotiateHoldTime,
    std::chrono::milliseconds myHeartbeatHoldTime,
    std::optional<int> maybeIpTos,
    bool enableV4,
    messaging::RQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue,
    messaging::ReplicateQueue<thrift::SparkNeighborEvent>& neighborUpdatesQueue,
    KvStoreCmdPort kvStoreCmdPort,
    OpenrCtrlThriftPort openrCtrlThriftPort,
    std::pair<uint32_t, uint32_t> version,
    std::shared_ptr<IoProvider> ioProvider,
    bool enableFloodOptimization,
    bool enableSpark2,
    bool increaseHelloInterval,
    std::shared_ptr<thrift::OpenrConfig> config)
    : myDomainName_(myDomainName),
      myNodeName_(myNodeName),
      udpMcastPort_(udpMcastPort),
      myHoldTime_(myHoldTime),
      myKeepAliveTime_(myKeepAliveTime),
      fastInitKeepAliveTime_(fastInitKeepAliveTime),
      myHelloTime_(myHelloTime),
      myHelloFastInitTime_(myHelloFastInitTime),
      myHandshakeTime_(myHandshakeTime),
      myHeartbeatTime_(myHeartbeatTime),
      myNegotiateHoldTime_(myNegotiateHoldTime),
      myHeartbeatHoldTime_(myHeartbeatHoldTime),
      enableV4_(enableV4),
      neighborUpdatesQueue_(neighborUpdatesQueue),
      kKvStoreCmdPort_(kvStoreCmdPort),
      kOpenrCtrlThriftPort_(openrCtrlThriftPort),
      kVersion_(apache::thrift::FRAGILE, version.first, version.second),
      enableFloodOptimization_(enableFloodOptimization),
      enableSpark2_(enableSpark2),
      increaseHelloInterval_(increaseHelloInterval),
      ioProvider_(std::move(ioProvider)),
      config_(std::move(config)) {
  CHECK(myHoldTime_ >= 3 * myKeepAliveTime)
      << "Keep-alive-time must be less than hold-time.";
  CHECK(myKeepAliveTime > std::chrono::milliseconds(0))
      << "Keep-alive-time can't be 0";
  CHECK(fastInitKeepAliveTime > std::chrono::milliseconds(0))
      << "fast-init-keep-alive-time can't be 0";
  CHECK(fastInitKeepAliveTime <= myKeepAliveTime)
      << "fast-init-keep-alive-time must not be bigger than keep-alive-time";
  CHECK(ioProvider_) << "Got null IoProvider";

  // Initialize global openr config
  loadConfig();

  // Initialize list of BucketedTimeSeries
  const std::chrono::seconds sec{1};
  const int32_t numBuckets = Constants::kMaxAllowedPps / 3;
  for (size_t i = 0; i < Constants::kNumTimeSeries; i++) {
    timeSeriesVector_.emplace_back(
        folly::BucketedTimeSeries<int64_t, std::chrono::steady_clock>(
            numBuckets, sec));
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
}

// static util function to transform state into str
std::string
Spark::sparkNeighborStateToStr(SparkNeighState state) {
  std::string res = "UNKNOWN";
  switch (state) {
  case SparkNeighState::IDLE:
    res = "IDLE";
    break;
  case SparkNeighState::WARM:
    res = "WARM";
    break;
  case SparkNeighState::NEGOTIATE:
    res = "NEGOTIATE";
    break;
  case SparkNeighState::ESTABLISHED:
    res = "ESTABLISHED";
    break;
  case SparkNeighState::RESTART:
    res = "RESTART";
    break;
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
      sendHelloPacket(
          ifName, false /* inFastInitState */, true /* restarting */);
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

void
Spark::addAreaRegex(
    const std::string& areaId,
    const std::vector<std::string>& neighborRegexes,
    const std::vector<std::string>& interfaceRegexes) {
  CHECK(not(neighborRegexes.empty() and interfaceRegexes.empty()))
      << "Invalid config. At least one non-empty regexes for neighbor or interface";

  re2::RE2::Options regexOpts;
  regexOpts.set_case_sensitive(false);
  std::string regexErr;
  std::unique_ptr<re2::RE2::Set> neighborRegexList{nullptr};
  std::unique_ptr<re2::RE2::Set> interfaceRegexList{nullptr};

  // neighbor regex
  if (not neighborRegexes.empty()) {
    neighborRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);

    for (const auto& regexStr : neighborRegexes) {
      if (-1 == neighborRegexList->Add(regexStr, &regexErr)) {
        LOG(FATAL) << folly::sformat(
            "Failed to add neighbor regex: {} for area: {}. Error: {}",
            regexStr,
            areaId,
            regexErr);
      }
    }
    CHECK(neighborRegexList->Compile()) << "Neighbor regex compilation failed";
  }

  // interface regex
  if (not interfaceRegexes.empty()) {
    interfaceRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);

    for (const auto& regexStr : interfaceRegexes) {
      if (-1 == interfaceRegexList->Add(regexStr, &regexErr)) {
        LOG(FATAL) << folly::sformat(
            "Failed to add interface regex: {} for area: {}. Error: {}",
            regexStr,
            areaId,
            regexErr);
      }
    }
    CHECK(interfaceRegexList->Compile())
        << "Interface regex compilation failed";
  }
  areaIdRegexList_.emplace_back(std::make_tuple(
      areaId, std::move(neighborRegexList), std::move(interfaceRegexList)));
}

// parse openrConfig to initialize:
//  1) areaId => [node_name|interface_name] regex matching;
//  2) etc.
//
void
Spark::loadConfig() {
  if (not config_) {
    // global openrConfig_ NOT supported yet. To make regex backward compatible:
    // defaultArea => anything(".*") for backward compatible
    addAreaRegex(thrift::KvStore_constants::kDefaultArea(), {".*"}, {".*"});
    return;
  }

  for (const auto& areaConfig : config_->areas) {
    addAreaRegex(
        areaConfig.area_id,
        areaConfig.neighbor_regexes,
        areaConfig.interface_regexes);
  }
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
  if (remoteVersion < static_cast<uint32_t>(kVersion_.lowestSupportedVersion)) {
    LOG(ERROR) << "Unsupported version: " << neighborName << " "
               << remoteVersion
               << ", must be >= " << kVersion_.lowestSupportedVersion;
    fb303::fbData->addStatValue(
        "spark.invalid_keepalive.invalid_version", 1, fb303::SUM);
    return PacketValidationResult::FAILURE;
  }
  return PacketValidationResult::SUCCESS;
}

// [Plan to deprecate]
PacketValidationResult
Spark::validateHelloPacket(
    std::string const& ifName, thrift::SparkHelloPacket const& helloPacket) {
  auto const& originator = helloPacket.payload.originator;
  auto const& domainName = originator.domainName;
  auto const& neighborName = originator.nodeName;
  auto const& remoteIfName = originator.ifName;
  uint32_t const& remoteVersion =
      static_cast<uint32_t>(helloPacket.payload.version);

  auto sanityCheckResult = sanityCheckHelloPkt(
      domainName, neighborName, remoteIfName, remoteVersion);
  if (PacketValidationResult::SKIP_LOOPED_SELF == sanityCheckResult) {
    return sanityCheckResult;
  }
  if (PacketValidationResult::FAILURE == sanityCheckResult) {
    LOG(ERROR) << "Sanity check of Hello pkt failed";
    return sanityCheckResult;
  }

  // validate v4 address subnet
  if (enableV4_) {
    if (PacketValidationResult::FAILURE ==
        validateV4AddressSubnet(ifName, originator.transportAddressV4)) {
      return PacketValidationResult::FAILURE;
    }
  }

  // get the map of tracked neighbors on this interface
  auto& ifNeighbors = neighbors_.at(ifName);

  // see if we already track this neighbor
  auto it = ifNeighbors.find(neighborName);

  // first time we hear from this guy, add to tracking list
  if (it == ifNeighbors.end()) {
    auto holdTimer = fbzmq::ZmqTimeout::make(
        getEvb(), [this, ifName, neighborName]() noexcept {
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
  auto event = createSparkNeighborEvent(
      thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE,
      ifName,
      originator,
      neighbor.rtt.count(),
      neighbor.label,
      false /* supportFloodOptimization: doesn't matter in RTT event*/,
      neighbor.area);
  neighborUpdatesQueue_.push(std::move(event));
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

  // check if the neighbor was adjacent. if so, report it as neighbor-down
  if (neighbor.isAdjacent) {
    LOG(INFO) << "Neighbor " << neighborName
              << " was adjacent, reporting as DOWN";
    neighbor.isAdjacent = false;

    auto event = createSparkNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        ifName,
        neighbor.info,
        neighbor.rtt.count(),
        neighbor.label,
        false /* supportFloodOptimization: doesn't matter in GR-expired event*/,
        neighbor.area);
    neighborUpdatesQueue_.push(std::move(event));
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
    pkt =
        util::readThriftObjStr<thrift::SparkHelloPacket>(readBuf, serializer_);
  } catch (std::exception const& err) {
    LOG(ERROR) << "Failed parsing hello packet " << folly::exceptionStr(err);
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
  auto& spark2Neighbor = spark2Neighbors_.at(ifName).at(neighborName);

  // only report RTT change if the neighbor is adjacent
  if (spark2Neighbor.state != SparkNeighState::ESTABLISHED) {
    VLOG(2) << "Neighbor: " << neighborName << " over iface: " << ifName
            << " is in state: " << sparkNeighborStateToStr(spark2Neighbor.state)
            << ". Skip RTT change notification.";
    return;
  }

  LOG(INFO) << "RTT for spark2Neighbor " << neighborName << " has changed "
            << "from " << spark2Neighbor.rtt.count() / 1000.0 << "ms to "
            << newRtt / 1000.0 << "ms over interface " << ifName;

  spark2Neighbor.rtt = std::chrono::microseconds(newRtt);
  notifySparkNeighborEvent(
      thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE,
      ifName,
      spark2Neighbor.toThrift(),
      spark2Neighbor.rtt.count(),
      spark2Neighbor.label,
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

  // to serve both Spark and Spark2 usage, will feed RTT info to
  // stepDetector whenever it is available.
  if (neighbors_.find(ifName) != neighbors_.end()) {
    auto& ifNeighbors = neighbors_.at(ifName);
    auto neighborIt = ifNeighbors.find(neighborName);
    if (neighborIt != ifNeighbors.end()) {
      auto& neighbor = neighborIt->second;

      // Add it to step detector
      neighbor.stepDetector.addValue(
          std::chrono::duration_cast<std::chrono::milliseconds>(myRecvTime),
          rtt.count());
      // Set initial value if empty
      if (!neighbor.rtt.count()) {
        VLOG(2) << "Setting initial value for RTT for neighbor "
                << neighborName;
        neighbor.rtt = rtt;
      }
      // Update rttLatest
      neighbor.rttLatest = rtt;
    }
  }

  // for Spark2 stepDetector usage
  if (spark2Neighbors_.find(ifName) != spark2Neighbors_.end()) {
    auto& spark2IfNeighbors = spark2Neighbors_.at(ifName);
    auto spark2NeighborIt = spark2IfNeighbors.find(neighborName);
    if (spark2NeighborIt != spark2IfNeighbors.end()) {
      auto& spark2Neighbor = spark2NeighborIt->second;

      // Add it to step detector
      spark2Neighbor.stepDetector.addValue(
          std::chrono::duration_cast<std::chrono::milliseconds>(myRecvTime),
          rtt.count());
      // Set initial value if empty
      if (!spark2Neighbor.rtt.count()) {
        VLOG(2) << "Setting initial value for RTT for spark2Neighbor "
                << neighborName;
        spark2Neighbor.rtt = rtt;
      }
      // Update rttLatest
      spark2Neighbor.rttLatest = rtt;
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
  handshakeMsg.nodeName = myNodeName_;
  handshakeMsg.isAdjEstablished = isAdjEstablished;
  handshakeMsg.holdTime = myHeartbeatHoldTime_.count();
  handshakeMsg.gracefulRestartTime = myHoldTime_.count();
  handshakeMsg.transportAddressV6 = toBinaryAddress(v6Addr);
  handshakeMsg.transportAddressV4 = toBinaryAddress(v4Addr);
  handshakeMsg.openrCtrlThriftPort = kOpenrCtrlThriftPort_;
  handshakeMsg.kvStoreCmdPort = kKvStoreCmdPort_;
  handshakeMsg.area = neighborAreaId; // send neighborAreaId deduced locally
  handshakeMsg.neighborNodeName_ref() = neighborName;

  thrift::SparkHelloPacket pkt;
  pkt.handshakeMsg_ref() = std::move(handshakeMsg);

  auto packet = util::writeThriftObjStr(pkt, serializer_);

  // send the pkt
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()), udpMcastPort_);

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
  heartbeatMsg.nodeName = myNodeName_;
  heartbeatMsg.seqNum = mySeqNum_;

  thrift::SparkHelloPacket pkt;
  pkt.heartbeatMsg_ref() = std::move(heartbeatMsg);

  auto packet = util::writeThriftObjStr(pkt, serializer_);

  // send the pkt
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()), udpMcastPort_);

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
  SYSLOG(INFO) << "State change: [" << sparkNeighborStateToStr(oldState)
               << "] -> [" << sparkNeighborStateToStr(newState) << "] "
               << "for neighbor: (" << neighborName << ") on interface: ("
               << ifName << ").";
}

void
Spark::checkNeighborState(
    Spark2Neighbor const& neighbor, SparkNeighState const& state) {
  CHECK(neighbor.state == state)
      << "Neighbor: (" << neighbor.nodeName << "), "
      << "Expected state: [" << sparkNeighborStateToStr(state) << "], "
      << "Actual state: [" << sparkNeighborStateToStr(neighbor.state) << "].";
}

std::optional<SparkNeighState>
Spark::getSparkNeighState(
    std::string const& ifName, std::string const& neighborName) {
  folly::Promise<std::optional<SparkNeighState>> promise;
  auto future = promise.getFuture();

  runInEventBaseThread(
      [this, promise = std::move(promise), &ifName, &neighborName]() mutable {
        if (spark2Neighbors_.find(ifName) == spark2Neighbors_.end()) {
          LOG(ERROR) << "No interface: " << ifName
                     << " in spark2Neighbor collection";
          promise.setValue(std::nullopt);
        } else {
          auto& ifNeighbors = spark2Neighbors_.at(ifName);
          auto neighborIt = ifNeighbors.find(neighborName);
          if (neighborIt == ifNeighbors.end()) {
            LOG(ERROR) << "No neighborName: " << neighborName
                       << " in spark2Neighbor colelction";
            promise.setValue(std::nullopt);
          } else {
            auto& neighbor = neighborIt->second;
            promise.setValue(neighbor.state);
          }
        }
      });
  return std::move(future).get();
}

void
Spark::neighborUpWrapper(
    Spark2Neighbor& neighbor,
    std::string const& ifName,
    std::string const& neighborName) {
  // stop sending out handshake msg, no longer in NEGOTIATE stage
  neighbor.negotiateTimer.reset();

  // remove negotiate hold timer, no longer in NEGOTIATE stage
  neighbor.negotiateHoldTimer.reset();

  // create heartbeat hold timer when promote to "ESTABLISHED"
  neighbor.heartbeatHoldTimer = fbzmq::ZmqTimeout::make(
      getEvb(), [this, ifName, neighborName]() noexcept {
        processHeartbeatTimeout(ifName, neighborName);
      });
  neighbor.heartbeatHoldTimer->scheduleTimeout(neighbor.heartbeatHoldTime);

  // add neighborName to collection
  ifNameToActiveNeighbors_[ifName].emplace(neighborName);

  // TODO: This is purely for backward compatibility.
  // Remove this after fully on Spark2.
  //
  // neighbor is under GR from old spark.
  // Should report NEIGHBOR_RESTARTED to honor GR
  if (neighbors_.find(ifName) != neighbors_.end()) {
    auto& oldIfNeighbors = neighbors_.at(ifName);
    auto oldNeighborIt = oldIfNeighbors.find(neighborName);
    if (oldNeighborIt != oldIfNeighbors.end()) {
      auto& oldNeighbor = oldNeighborIt->second;
      if (oldNeighbor.numRecvRestarting > 0) {
        notifySparkNeighborEvent(
            thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
            ifName,
            neighbor.toThrift(),
            neighbor.rtt.count(),
            neighbor.label,
            true /* support flood-optimization */,
            neighbor.area);
        oldNeighbor.numRecvRestarting = 0;
        return;
      }
    }
  }

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
    Spark2Neighbor const& neighbor,
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
  event.supportFloodOptimization = supportFloodOptimization;
  event.area = area;
  neighborUpdatesQueue_.push(std::move(event));
}

void
Spark::processHeartbeatTimeout(
    std::string const& ifName, std::string const& neighborName) {
  // spark2 neighbor must exist
  auto& ifNeighbors = spark2Neighbors_.at(ifName);
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

  // bring down neighborship and cleanup spark2 neighbor state
  neighborDownWrapper(neighbor, ifName, neighborName);
}

void
Spark::processNegotiateTimeout(
    std::string const& ifName, std::string const& neighborName) {
  // spark2 neighbor must exist if the negotiate hold-time expired
  auto& neighbor = spark2Neighbors_.at(ifName).at(neighborName);

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
  // spark2 neighbor must exist if the negotiate hold-timer call back gets
  // called.
  auto& ifNeighbors = spark2Neighbors_.at(ifName);
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

  // bring down neighborship and cleanup spark2 neighbor state
  neighborDownWrapper(neighbor, ifName, neighborName);
}

void
Spark::processGRMsg(
    std::string const& neighborName,
    std::string const& ifName,
    Spark2Neighbor& neighbor) {
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
  neighbor.gracefulRestartHoldTimer = fbzmq::ZmqTimeout::make(
      getEvb(), [this, ifName, neighborName]() noexcept {
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
  auto const& neighborName = helloMsg.nodeName;
  auto const& domainName = helloMsg.domainName;
  auto const& remoteIfName = helloMsg.ifName;
  auto const& neighborInfos = helloMsg.neighborInfos;
  auto const& remoteVersion = static_cast<uint32_t>(helloMsg.version);
  auto const& remoteSeqNum = static_cast<uint64_t>(helloMsg.seqNum);
  auto const& nbrSentTimeInUs = std::chrono::microseconds(helloMsg.sentTsInUs);

  // interface name check
  if (spark2Neighbors_.find(ifName) == spark2Neighbors_.end()) {
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

  // deduce area for peer
  // TODO: in case area is different from previously calculated one,
  //       trigger area change event
  auto area = getNeighborArea(neighborName, ifName, areaIdRegexList_);
  if (not area.has_value()) {
    return;
  }

  // get (neighborName -> Spark2Neighbor) mapping per ifName
  auto& ifNeighbors = spark2Neighbors_.at(ifName);

  // check if we have already track this neighbor
  auto neighborIt = ifNeighbors.find(neighborName);

  if (neighborIt == ifNeighbors.end()) {
    // Report RTT change
    // capture ifName & originator by copy
    auto rttChangeCb = [this, ifName, neighborName](const int64_t& newRtt) {
      processRttChange(ifName, neighborName, newRtt);
    };

    ifNeighbors.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(neighborName),
        std::forward_as_tuple(
            domainName, // neighborNode domain
            neighborName, // neighborNode name
            remoteIfName, // remote interface on neighborNode
            getNewLabelForIface(ifName), // label for Segment Routing
            remoteSeqNum, // seqNum reported by neighborNode
            myKeepAliveTime_,
            std::move(rttChangeCb),
            area.value()));

    auto& neighbor = ifNeighbors.at(neighborName);
    checkNeighborState(neighbor, SparkNeighState::IDLE);

    // backward compatibility check
    if (neighbors_.find(ifName) != neighbors_.end()) {
      // Let's say we have neighborship between:
      //  spark <=> spark2
      //
      // Since spark is NOT sending spark2Msg, spark2 instance
      // will still hold non-spark2 data structures. Right now
      // when spark is restarting itself to run with
      // `enableSpark2=True`, we must purge away old holdTimer
      // related stuff. Otherwise, neighborHoldTimer will bring
      // neighbor down.
      auto& oldIfNeighbors = neighbors_.at(ifName);
      auto oldNeighborIt = oldIfNeighbors.find(neighborName);
      if (oldNeighborIt != oldIfNeighbors.end()) {
        oldNeighborIt->second.holdTimer->cancelTimeout();
      }
    }
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
        std::chrono::microseconds(ts.lastNbrMsgSentTsInUs),
        // recvTime of my helloPkt recorded by neighbor
        std::chrono::microseconds(ts.lastMyMsgRcvdTsInUs),
        // sentTime of neighbor helloPkt
        nbrSentTimeInUs,
        neighborName,
        remoteIfName,
        ifName);
  }

  VLOG(3) << "Current state for neighbor: (" << neighborName << ") is: ["
          << sparkNeighborStateToStr(neighbor.state) << "]";

  // for neighbor in fast initial state and does not see us yet,
  // reply for quick convergence
  if (helloMsg.solicitResponse) {
    sendHelloPacket(ifName);

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
        static_cast<uint64_t>(neighborInfos.at(myNodeName_).seqNum);
    if (myRemoteSeqNum >= mySeqNum_) {
      VLOG(2) << "Seeing my previous incarnation from neighbor: ("
              << neighborName << "). Seen Seq# from neighbor: ("
              << myRemoteSeqNum << "), my Seq#: (" << mySeqNum_ << ").";
      return;
    }

    // Starts timer to periodically send hankshake msg
    const std::string neighborAreaId = neighbor.area;
    neighbor.negotiateTimer = fbzmq::ZmqTimeout::make(
        getEvb(), [this, ifName, neighborName, neighborAreaId]() noexcept {
          // periodically send out handshake msg
          sendHandshakeMsg(ifName, neighborName, neighborAreaId, false);
        });
    const bool isPeriodic = true;
    neighbor.negotiateTimer->scheduleTimeout(myHandshakeTime_, isPeriodic);

    // Starts negotiate hold-timer
    neighbor.negotiateHoldTimer = fbzmq::ZmqTimeout::make(
        getEvb(), [this, ifName, neighborName]() noexcept {
          // prevent to stucking in NEGOTIATE forever
          processNegotiateTimeout(ifName, neighborName);
        });
    neighbor.negotiateHoldTimer->scheduleTimeout(myNegotiateHoldTime_);

    // Neighbor is aware of us. Promote to NEGOTIATE state
    SparkNeighState oldState = neighbor.state;
    neighbor.state = getNextState(oldState, SparkNeighEvent::HELLO_RCVD_INFO);
    logStateTransition(neighborName, ifName, oldState, neighbor.state);
  } else if (neighbor.state == SparkNeighState::ESTABLISHED) {
    // Update local seqNum maintained for this neighbor
    neighbor.seqNum = remoteSeqNum;

    // Check if neighbor is undergoing 'Graceful-Restart'
    if (helloMsg.restarting) {
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

      // bring down neighborship and cleanup spark2 neighbor state
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
    neighbor.heartbeatHoldTimer = fbzmq::ZmqTimeout::make(
        getEvb(), [this, ifName, neighborName]() noexcept {
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

  auto const& neighborName = handshakeMsg.nodeName;
  auto& ifNeighbors = spark2Neighbors_.at(ifName);
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
  if (not handshakeMsg.isAdjEstablished) {
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
            << sparkNeighborStateToStr(neighbor.state) << "]"
            << ", expected state: [NEGOTIIATE]";
    return;
  }

  // update Spark2 neighborState
  neighbor.kvStoreCmdPort = handshakeMsg.kvStoreCmdPort;
  neighbor.openrCtrlThriftPort = handshakeMsg.openrCtrlThriftPort;
  neighbor.transportAddressV4 = handshakeMsg.transportAddressV4;
  neighbor.transportAddressV6 = handshakeMsg.transportAddressV6;

  // update neighbor holdTime as "NEGOTIATING" process
  neighbor.heartbeatHoldTime = std::max(
      std::chrono::milliseconds(handshakeMsg.holdTime), myHeartbeatHoldTime_);
  neighbor.gracefulRestartHoldTime = std::max(
      std::chrono::milliseconds(handshakeMsg.gracefulRestartTime), myHoldTime_);

  // v4 subnet validation if enabled
  if (enableV4_) {
    if (PacketValidationResult::FAILURE ==
        validateV4AddressSubnet(ifName, handshakeMsg.transportAddressV4)) {
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
      handshakeMsg.area != thrift::KvStore_constants::kDefaultArea()) {
    // For backward compatible consideration, If:
    //  1) neighbor.area == defaulArea: this node doesn't support areaConfig;
    //  2) handshakeMsg.area == defaultArea: peer doesn't support areaConfig;
    if (neighbor.area != handshakeMsg.area) {
      LOG(ERROR)
          << "Inconsistent areaId deduced between local and remote review. "
          << "Neighbor's areaId: [" << neighbor.area << "], "
          << "My areaId from remote: [" << handshakeMsg.area << "].";

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
    // In case it doesn't support AREA negotiation.
    // Override neighbor area deduced previously from helloMsg to defaultArea.
    neighbor.area = thrift::KvStore_constants::kDefaultArea();
  }

  // state transition
  SparkNeighState oldState = neighbor.state;
  neighbor.state = getNextState(oldState, SparkNeighEvent::HANDSHAKE_RCVD);
  logStateTransition(neighborName, ifName, oldState, neighbor.state);

  // bring up neighborship and set corresponding spark2 state
  neighborUpWrapper(neighbor, ifName, neighborName);
}

void
Spark::processHeartbeatMsg(
    thrift::SparkHeartbeatMsg const& heartbeatMsg, std::string const& ifName) {
  auto const& neighborName = heartbeatMsg.nodeName;
  auto& ifNeighbors = spark2Neighbors_.at(ifName);
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
            << sparkNeighborStateToStr(neighbor.state) << "]"
            << ", expected state: [ESTABLISHED]";
    return;
  }

  // Reset the hold-timer for neighbor as we have received a keep-alive msg
  neighbor.heartbeatHoldTimer->scheduleTimeout(neighbor.heartbeatHoldTime);
}

void
Spark::processPacket() {
  // Step 1: receive and parse pkt
  thrift::SparkHelloPacket helloPacket;
  std::string ifName;
  std::chrono::microseconds myRecvTime;

  if (!parsePacket(helloPacket, ifName, myRecvTime)) {
    return;
  }

  // Step 2: Spark2 specific msg processing
  if (enableSpark2_) {
    if (helloPacket.helloMsg_ref().has_value()) {
      processHelloMsg(helloPacket.helloMsg_ref().value(), ifName, myRecvTime);
      return;
    } else if (helloPacket.heartbeatMsg_ref().has_value()) {
      processHeartbeatMsg(helloPacket.heartbeatMsg_ref().value(), ifName);
      return;
    } else if (helloPacket.handshakeMsg_ref().has_value()) {
      processHandshakeMsg(helloPacket.handshakeMsg_ref().value(), ifName);
      return;
    } else {
      VLOG(3) << "No valid Spark2 msg. Fallback to old Spark processing";
    }
  }

  // Step 3: old spark way of processing logic
  auto validationResult = validateHelloPacket(ifName, helloPacket);
  if (PacketValidationResult::SKIP_LOOPED_SELF == validationResult) {
    return;
  }
  if (validationResult == PacketValidationResult::FAILURE ||
      validationResult == PacketValidationResult::INVALID_AREA_CONFIGURATION) {
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

  // check if it's a restarting packet
  if (helloPacket.payload.restarting_ref().has_value() and
      *helloPacket.payload.restarting_ref()) {
    // this neighbor informed us that it's restarting
    neighbor.numRecvRestarting += 1;
    if (neighbor.numRecvRestarting > 1) {
      // duplicate restarting packet, we already known this neighbor is
      // restarting
      return;
    }
    LOG(INFO) << "neighbor " << originator.nodeName << " from iface "
              << originator.ifName << " on iface" << ifName << " is restarting";

    auto event = createSparkNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        ifName,
        originator,
        neighbor.rtt.count(),
        neighbor.label,
        false /* supportDual: doesn't matter in DOWN event*/,
        neighbor.area);
    neighborUpdatesQueue_.push(std::move(event));
    return;
  }

  // Try to deduce RTT for this neighbor and update timestamps for recvd hello
  auto it = helloPacket.payload.neighborInfos.find(myNodeName_);
  if (it != helloPacket.payload.neighborInfos.end()) {
    auto& tstamps = it->second;
    auto mySentTime = std::chrono::microseconds(tstamps.lastNbrMsgSentTsInUs);
    auto nbrRecvTime = std::chrono::microseconds(tstamps.lastMyMsgRcvdTsInUs);
    updateNeighborRtt(
        // recvTime of neighbor helloPkt
        myRecvTime,
        // sentTime of my helloPkt recorded by neighbor
        mySentTime,
        // recvTime of my helloPkt recorded by neighbor
        nbrRecvTime,
        // sentTime of neighbor helloPkt
        nbrSentTime,
        originator.nodeName,
        originator.ifName,
        ifName);
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

  // check if neighbor support flood optimization or not
  const auto& supportFloodOptimization =
      helloPacket.payload.supportFloodOptimization;

  if (isAdjacent &&
      validationResult == PacketValidationResult::NEIGHBOR_RESTART) {
    LOG(INFO) << "Adjacent neighbor " << originator.nodeName << " from iface "
              << originator.ifName << " on iface " << ifName
              << " is restarting, waiting for it to ack myself.";

    auto event = createSparkNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
        ifName,
        originator,
        neighbor.rtt.count(),
        neighbor.label,
        supportFloodOptimization,
        neighbor.area);
    neighbor.numRecvRestarting = 0; // reset counter when neighbor comes up
    neighborUpdatesQueue_.push(std::move(event));
    return;
  }

  // NOTE: means we only use the data for neighbor from initial packet.
  // all the other messages serves as confirmation of hold time refresh.
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

    auto event = createSparkNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        ifName,
        originator,
        neighbor.rtt.count(),
        neighbor.label,
        supportFloodOptimization,
        neighbor.area);
    neighborUpdatesQueue_.push(std::move(event));
    neighbor.numRecvRestarting = 0; // reset counter when neighbor comes up
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

    auto event = createSparkNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        ifName,
        originator,
        neighbor.rtt.count(),
        neighbor.label,
        false /* supportFloodOptimization: doesn't matter in DOWN event*/,
        neighbor.area);
    neighborUpdatesQueue_.push(std::move(event));
    neighbor.isAdjacent = false;
    neighbor.holdTimer->cancelTimeout(); // Stop hold-timer
    return;
  }
}

void
Spark::sendHelloPacket(
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
  thrift::OpenrVersion openrVer(kVersion_.version);

  // build the hello packet from payload and empty signature
  thrift::SparkHelloPacket helloPacket;

  if (enableSpark2_) {
    thrift::SparkHelloMsg helloMsg;
    helloMsg.domainName = myDomainName_;
    helloMsg.nodeName = myNodeName_;
    helloMsg.ifName = ifName;
    helloMsg.seqNum = mySeqNum_;
    helloMsg.neighborInfos =
        std::map<std::string, thrift::ReflectedNeighborInfo>{};
    helloMsg.version = openrVer;
    helloMsg.solicitResponse = inFastInitState;
    helloMsg.restarting = restarting;
    helloMsg.sentTsInUs = getCurrentTimeInUs().count();

    // bake neighborInfo into helloMsg
    for (const auto& kv : spark2Neighbors_.at(ifName)) {
      auto const& neighborName = kv.first;
      auto const& neighbor = kv.second;

      auto& neighborInfo = helloMsg.neighborInfos[neighborName];
      neighborInfo.seqNum = neighbor.seqNum;
      neighborInfo.lastNbrMsgSentTsInUs = neighbor.neighborTimestamp.count();
      neighborInfo.lastMyMsgRcvdTsInUs = neighbor.localTimestamp.count();
    }

    // fill in helloMsg field
    helloPacket.helloMsg_ref() = std::move(helloMsg);
  } else {
    // TODO: deprecate the payload setup once old spark msg
    // no longer is use
    thrift::SparkNeighbor myself = createSparkNeighbor(
        myDomainName_,
        myNodeName_,
        myHoldTime_.count(),
        toBinaryAddress(v4Addr),
        toBinaryAddress(v6Addr),
        kKvStoreCmdPort_,
        ifName);

    // create the hello packet payload
    auto payload = createSparkPayload(
        openrVer,
        myself,
        mySeqNum_,
        std::map<std::string, thrift::ReflectedNeighborInfo>{},
        getCurrentTimeInUs().count(),
        inFastInitState,
        enableFloodOptimization_,
        restarting,
        std::nullopt);

    // add all neighbors we have heard from on this interface
    for (const auto& kv : neighbors_.at(ifName)) {
      std::string const& neighborName = kv.first;
      auto& neighbor = kv.second;

      // Add timestamp and sequence number from last hello. Will be 0 if we
      // haven't heard before from the neighbor.
      // Refer to thrift for definition of timestampts.
      auto& neighborInfo = payload.neighborInfos[neighborName];
      neighborInfo.seqNum = neighbor.seqNum;
      neighborInfo.lastNbrMsgSentTsInUs = neighbor.neighborTimestamp.count();
      neighborInfo.lastMyMsgRcvdTsInUs = neighbor.localTimestamp.count();
    }

    helloPacket.payload = std::move(payload);
    helloPacket.signature = "";
  }

  auto packet = util::writeThriftObjStr(helloPacket, serializer_);

  // send the payload
  folly::SocketAddress dstAddr(
      folly::IPAddress(Constants::kSparkMcastAddr.toString()), udpMcastPort_);

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

    // one neighbor either supports spark2 or NOT.
    // it will show EITHER in spark2Neighbors OR neighbors_. NOT both.
    if (enableSpark2_) {
      for (const auto& kv : spark2Neighbors_.at(ifName)) {
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
      spark2Neighbors_.erase(ifName);
      ifNameToHeartbeatTimers_.erase(ifName);
    }

    for (const auto& kv : neighbors_.at(ifName)) {
      auto& neighborName = kv.first;
      auto& neighbor = kv.second;

      allocatedLabels_.erase(neighbor.label);
      if (!neighbor.isAdjacent) {
        continue;
      }
      LOG(INFO) << "Neighbor " << neighborName << " removed due to iface "
                << ifName << " down";

      auto event = createSparkNeighborEvent(
          thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
          ifName,
          neighbor.info,
          neighbor.rtt.count(),
          neighbor.label,
          false /* supportFloodOptimization: doesn't matter in DOWN event*/,
          neighbor.area);
      neighborUpdatesQueue_.push(std::move(event));
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
      auto result = neighbors_.emplace(
          ifName, std::unordered_map<std::string, Neighbor>{});
      CHECK(result.second);
    }

    if (enableSpark2_) {
      // create place-holders for newly added interface
      auto result = spark2Neighbors_.emplace(
          ifName, std::unordered_map<std::string, Spark2Neighbor>{});
      CHECK(result.second);

      // heartbeatTimers will start as soon as intf is in UP state
      auto heartbeatTimer = fbzmq::ZmqTimeout::make(
          getEvb(), [this, ifName]() noexcept { sendHeartbeatMsg(ifName); });

      const bool isPeriodic = true; /* flag indicating periodic pkt sent-out*/
      ifNameToHeartbeatTimers_.emplace(ifName, std::move(heartbeatTimer));
      ifNameToHeartbeatTimers_.at(ifName)->scheduleTimeout(
          myHeartbeatTime_, isPeriodic);
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

    auto roll = (enableSpark2_ && increaseHelloInterval_)
        ? rollHelper(myHelloTime_)
        : rollHelper(myKeepAliveTime_);
    auto rollFast = (enableSpark2_ && increaseHelloInterval_)
        ? rollHelper(myHelloFastInitTime_)
        : rollHelper(fastInitKeepAliveTime_);
    auto timePoint = std::chrono::steady_clock::now();

    // NOTE: We do not send hello packet immediately after adding new interface
    // this is due to the fact that it may not have yet configured a link-local
    // address. The hello packet will be sent later and will have good chances
    // of making it out if small delay is introduced.
    auto helloTimer = fbzmq::ZmqTimeout::make(
        getEvb(), [this, ifName, timePoint, roll, rollFast]() mutable noexcept {
          VLOG(3) << "Sending hello multicast packet on interface " << ifName;
          bool inFastInitState = false;
          if (enableSpark2_ && increaseHelloInterval_) {
            // Under Spark2 context, hello pkt will be sent in relatively low
            // frequency. However, when node comes up initially or restarting,
            // send multiple helloMsg to promote to 'NEGOTIATE' state ASAP.
            // To form adj, at least 2 helloMsg is needed( i.e. with second
            // hello contain myNodeName_ info ). To give enough margin, send
            // 3 times of necessary packets.
            inFastInitState = (std::chrono::steady_clock::now() - timePoint) <=
                6 * myHelloFastInitTime_;
          } else {
            // We will send atleast 3 and atmost 4 packets in fast mode. Only
            // one packet is enough for discovering neighbors in fast mode,
            // however we send multiple for redundancy to overcome packet drops
            // and compute
            inFastInitState = (std::chrono::steady_clock::now() - timePoint) <=
                3 * fastInitKeepAliveTime_;
          }
          sendHelloPacket(ifName, inFastInitState);

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
  for (auto const& ifaceNeighbors : neighbors_) {
    trackedNeighborCount += ifaceNeighbors.second.size();
    for (auto const& kv : ifaceNeighbors.second) {
      auto const& neighbor = kv.second;
      adjacentNeighborCount += neighbor.isAdjacent;
      fb303::fbData->setCounter(
          "spark.rtt_us." + neighbor.info.nodeName + "." + ifaceNeighbors.first,
          neighbor.rtt.count());
      fb303::fbData->setCounter(
          "spark.rtt_latest_us." + neighbor.info.nodeName,
          neighbor.rttLatest.count());
      fb303::fbData->setCounter(
          "spark.seq_num." + neighbor.info.nodeName, neighbor.seqNum);
    }
  }
  for (auto const& ifaceNeighbors : spark2Neighbors_) {
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
      "spark.num_tracked_interfaces",
      neighbors_.size() ? neighbors_.size() : spark2Neighbors_.size());
  fb303::fbData->setCounter(
      "spark.num_tracked_neighbors", trackedNeighborCount);
  fb303::fbData->setCounter(
      "spark.num_adjacent_neighbors", adjacentNeighborCount);
  fb303::fbData->setCounter("spark.my_seq_num", mySeqNum_);
  fb303::fbData->setCounter("spark.pending_timers", getEvb()->timer().count());
}

// This is a static function
std::optional<std::string>
Spark::getNeighborArea(
    const std::string& peerNodeName,
    const std::string& localIfName,
    const std::vector<std::tuple<
        std::string,
        std::unique_ptr<re2::RE2::Set>,
        std::unique_ptr<re2::RE2::Set>>>& areaIdRegexList) {
  std::vector<std::string> candidateAreas{};

  // looping through areaIdRegexList
  for (const auto& t : areaIdRegexList) {
    const auto& areaId = std::get<0>(t);
    const auto& neighborRegex = std::get<1>(t);
    const auto& interfaceRegex = std::get<2>(t);
    if (neighborRegex and interfaceRegex) {
      if (matchRegexSet(peerNodeName, neighborRegex) and
          matchRegexSet(localIfName, interfaceRegex)) {
        VLOG(4) << folly::sformat(
            "Area: {} found for neighbor: {}, interface: {}",
            areaId,
            peerNodeName,
            localIfName);
        candidateAreas.emplace_back(areaId);
      }
    } else if (neighborRegex and matchRegexSet(peerNodeName, neighborRegex)) {
      VLOG(4) << folly::sformat(
          "Area: {} found for neighbor: {}", areaId, peerNodeName);
      candidateAreas.emplace_back(areaId);
    } else if (interfaceRegex and matchRegexSet(localIfName, interfaceRegex)) {
      VLOG(4) << folly::sformat(
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

} // namespace openr
