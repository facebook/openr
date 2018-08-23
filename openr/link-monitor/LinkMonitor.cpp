/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LinkMonitor.h"

#include <syslog.h>
#include <functional>

#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/IpPrefix_types.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/spark/Spark.h>

using apache::thrift::FRAGILE;

namespace {

const std::string kConfigKey{"link-monitor-config"};

/**
 * Transformation function to convert measured rtt (in us) to a metric value
 * to be used. Metric can never be zero.
 */
int32_t
getRttMetric(int64_t rttUs) {
  return std::max((int)(rttUs / 100), (int)1);
}

void
printLinkMonitorConfig(openr::thrift::LinkMonitorConfig const& config) {
  VLOG(1) << "LinkMonitor config .... ";
  VLOG(1) << "\tnodeLabel: " << config.nodeLabel;
  VLOG(1) << "\tisOverloaded: " << (config.isOverloaded ? "true" : "false");
  if (not config.overloadedLinks.empty()) {
    VLOG(1) << "\toverloadedLinks: "
            << folly::join(",", config.overloadedLinks);
  }
  if (not config.linkMetricOverrides.empty()) {
    VLOG(1) << "\tlinkMetricOverrides: ";
    for (auto const& kv : config.linkMetricOverrides) {
      VLOG(1) << "\t\t" << kv.first << ": " << kv.second;
    }
  }
}

} // anonymous namespace

namespace openr {

//
// LinkMonitor code
//
LinkMonitor::LinkMonitor(
    //
    // Immutable state initializers
    //
    fbzmq::Context& zmqContext,
    std::string nodeId,
    int32_t platformThriftPort,
    KvStoreLocalCmdUrl kvStoreLocalCmdUrl,
    KvStoreLocalPubUrl kvStoreLocalPubUrl,
    std::unique_ptr<re2::RE2::Set> includeRegexList,
    std::unique_ptr<re2::RE2::Set> excludeRegexList,
    std::unique_ptr<re2::RE2::Set> redistRegexList,
    std::vector<thrift::IpPrefix> const& staticPrefixes,
    bool useRttMetric,
    bool enableFullMeshReduction,
    bool enablePerfMeasurement,
    bool enableV4,
    bool advertiseInterfaceDb,
    bool enableSegmentRouting,
    AdjacencyDbMarker adjacencyDbMarker,
    InterfaceDbMarker interfaceDbMarker,
    SparkCmdUrl sparkCmdUrl,
    SparkReportUrl sparkReportUrl,
    MonitorSubmitUrl const& monitorSubmitUrl,
    PersistentStoreUrl const& configStoreUrl,
    bool assumeDrained,
    PrefixManagerLocalCmdUrl const& prefixManagerUrl,
    PlatformPublisherUrl const& platformPubUrl,
    LinkMonitorGlobalPubUrl linkMonitorGlobalPubUrl,
    LinkMonitorGlobalCmdUrl linkMonitorGlobalCmdUrl,
    //
    // Mutable/transient state initializers
    //
    std::chrono::seconds adjHoldTime,
    std::chrono::milliseconds flapInitialBackoff,
    std::chrono::milliseconds flapMaxBackoff)
    : nodeId_(nodeId),
      platformThriftPort_(platformThriftPort),
      kvStoreLocalCmdUrl_(kvStoreLocalCmdUrl),
      kvStoreLocalPubUrl_(kvStoreLocalPubUrl),
      includeRegexList_(std::move(includeRegexList)),
      excludeRegexList_(std::move(excludeRegexList)),
      redistRegexList_(std::move(redistRegexList)),
      staticPrefixes_(staticPrefixes),
      useRttMetric_(useRttMetric),
      enableFullMeshReduction_(enableFullMeshReduction),
      enablePerfMeasurement_(enablePerfMeasurement),
      enableV4_(enableV4),
      advertiseInterfaceDb_(advertiseInterfaceDb),
      enableSegmentRouting_(enableSegmentRouting),
      adjacencyDbMarker_(adjacencyDbMarker),
      interfaceDbMarker_(interfaceDbMarker),
      sparkCmdUrl_(sparkCmdUrl),
      sparkReportUrl_(sparkReportUrl),
      platformPubUrl_(platformPubUrl),
      linkMonitorGlobalPubUrl_(linkMonitorGlobalPubUrl),
      linkMonitorGlobalCmdUrl_(linkMonitorGlobalCmdUrl),
      // mutable states
      flapInitialBackoff_(flapInitialBackoff),
      flapMaxBackoff_(flapMaxBackoff),
      linkMonitorPubSock_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      linkMonitorCmdSock_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      sparkCmdSock_(zmqContext),
      sparkReportSock_(zmqContext),
      nlEventSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      expBackoff_(Constants::kInitialBackoff, Constants::kMaxBackoff) {
  // Create throttled adjacency advertiser
  advertiseMyAdjacenciesThrottled_ = std::make_unique<fbzmq::ZmqThrottle>(
      this, Constants::kLinkThrottleTimeout, [this]() noexcept {
        processPendingPeerAddRequests();
      });

  // Hold-time for not advertising partial adjacencies
  adjHoldUntilTimePoint_ = std::chrono::steady_clock::now() + adjHoldTime;

  LOG(INFO) << "Loading link-monitor config";
  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Create config-store client
  configStoreClient_ =
      std::make_unique<PersistentStoreClient>(configStoreUrl, zmqContext);
  scheduleTimeout(std::chrono::seconds(0), [this, assumeDrained]() noexcept {
    auto config = configStoreClient_->loadThriftObj<thrift::LinkMonitorConfig>(
        kConfigKey);
    if (config.hasValue()) {
      LOG(INFO) << "Loaded link-monitor config from disk.";
      config_ = config.value();
      printLinkMonitorConfig(config_);
    } else {
      config_.isOverloaded = assumeDrained;
      LOG(WARNING) << folly::sformat("Failed to load link-monitor config. "
          "Setting node as {}", assumeDrained ? "DRAINED" : "UNDRAINED");
    }
  });

  prefixManagerClient_ =
      std::make_unique<PrefixManagerClient>(prefixManagerUrl, zmqContext);

  //  Create KvStore client
  kvStoreClient_ = std::make_unique<KvStoreClient>(
      zmqContext,
      this,
      nodeId_,
      kvStoreLocalCmdUrl_,
      kvStoreLocalPubUrl_,
      folly::none, /* persist key timer */
      folly::none /* recv timeout */);

  if (enableSegmentRouting) {
    // create range allocator to get unique node labels
    rangeAllocator_ = std::make_unique<RangeAllocator<int32_t>>(
        nodeId_,
        Constants::kNodeLabelRangePrefix.toString(),
        kvStoreClient_.get(),
        [&](folly::Optional<int32_t> newVal) noexcept {
          config_.nodeLabel = newVal ? newVal.value() : 0;
          advertiseMyAdjacencies();
        },
        std::chrono::milliseconds(100),
        std::chrono::seconds(2),
        false /* override owner */);

    // Delay range allocation until we have formed all of our adjcencies
    scheduleTimeoutAt(adjHoldUntilTimePoint_, [this]() {
      folly::Optional<int32_t> initValue;
      if (config_.nodeLabel != 0) {
        initValue = config_.nodeLabel;
      }
      rangeAllocator_->startAllocator(Constants::kSrGlobalRange, initValue);
    });
  }

  // Initialize ZMQ sockets
  prepare();
}

void
LinkMonitor::prepare() noexcept {
  //
  // Prepare all sockets
  //
  // bind out publisher socket
  VLOG(2) << "Link Monitor: Binding pub url '" << linkMonitorGlobalPubUrl_
          << "'";
  const auto lmPub =
      linkMonitorPubSock_.bind(fbzmq::SocketUrl{linkMonitorGlobalPubUrl_});
  if (lmPub.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << linkMonitorGlobalPubUrl_ << "' "
               << lmPub.error();
  }

  // enable handover to new connection for duplicate identities
  const int handover = 1;
  const auto linkMonOpt = linkMonitorCmdSock_.setSockOpt(
      ZMQ_ROUTER_HANDOVER, &handover, sizeof(int));
  if (linkMonOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_ROUTER_HANDOVER to " << handover << " "
               << linkMonOpt.error();
  }

  // bind link monitor command socket
  VLOG(2) << "LinkMonitor: Binding linkMonitorGlobalCmdUrl_: '"
          << linkMonitorGlobalCmdUrl_ << "'";
  const auto lmCmd =
      linkMonitorCmdSock_.bind(fbzmq::SocketUrl{linkMonitorGlobalCmdUrl_});
  if (lmCmd.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << linkMonitorGlobalCmdUrl_ << "' "
               << lmCmd.error();
  }

  VLOG(2) << "Connect to Spark to send commands on " << sparkCmdUrl_;

  // Subscribe to events published by Spark for neighbor state changes
  const auto sparkCmd = sparkCmdSock_.connect(fbzmq::SocketUrl{sparkCmdUrl_});
  if (sparkCmd.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << sparkCmdUrl_ << "' "
               << sparkCmd.error();
  }

  // Subscribe to events published by Spark for neighbor state changes
  LOG(INFO) << "Connect to Spark for neighbor events";
  const auto sparkRep =
      sparkReportSock_.connect(fbzmq::SocketUrl{sparkReportUrl_});
  if (sparkRep.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << sparkReportUrl_ << "' "
               << sparkRep.error();
  }

  // Subscribe to link/addr events published by NetlinkAgent
  VLOG(2) << "Connect to PlatformPublisher to subscribe NetlinkEvent on "
          << platformPubUrl_;
  const auto linkEventType =
      static_cast<uint16_t>(thrift::PlatformEventType::LINK_EVENT);
  const auto addrEventType =
      static_cast<uint16_t>(thrift::PlatformEventType::ADDRESS_EVENT);
  auto nlLinkSubOpt =
      nlEventSub_.setSockOpt(ZMQ_SUBSCRIBE, &linkEventType, sizeof(uint16_t));
  if (nlLinkSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to " << linkEventType << " "
               << nlLinkSubOpt.error();
  }
  auto nlAddrSubOpt =
      nlEventSub_.setSockOpt(ZMQ_SUBSCRIBE, &addrEventType, sizeof(uint16_t));
  if (nlAddrSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to " << addrEventType << " "
               << nlAddrSubOpt.error();
  }
  const auto nlSub = nlEventSub_.connect(fbzmq::SocketUrl{platformPubUrl_});
  if (nlSub.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << platformPubUrl_ << "' "
               << nlSub.error();
  }

  // Listen for messages from spark
  addSocket(
      fbzmq::RawZmqSocketPtr{*sparkReportSock_},
      ZMQ_POLLIN,
      [this](int) noexcept {
        VLOG(1) << "LinkMonitor: Spark message received...";

        auto maybeEvent =
            sparkReportSock_.recvThriftObj<thrift::SparkNeighborEvent>(
                serializer_, Constants::kReadTimeout);
        if (maybeEvent.hasError()) {
          LOG(ERROR) << "Error processing Spark event object: "
                     << maybeEvent.error();
          return;
        }
        auto event = maybeEvent.value();

        auto neighborAddrV4 = event.neighbor.transportAddressV4;
        auto neighborAddrV6 = event.neighbor.transportAddressV6;

        VLOG(1) << "Received neighbor event for " << event.neighbor.nodeName
                << " from " << event.neighbor.ifName << " at " << event.ifName
                << " with addrs " << toString(neighborAddrV6) << " and "
                << (enableV4_ ? toString(neighborAddrV4) : "");

        switch (event.eventType) {
        case thrift::SparkNeighborEventType::NEIGHBOR_UP:
          logEvent(
              "NB_UP",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);
          neighborUpEvent(neighborAddrV4, neighborAddrV6, event);
          break;

        case thrift::SparkNeighborEventType::NEIGHBOR_RESTART:
          logEvent(
              "NB_RESTART",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);
          neighborUpEvent(neighborAddrV4, neighborAddrV6, event);
          break;

        case thrift::SparkNeighborEventType::NEIGHBOR_DOWN:
          logEvent(
              "NB_DOWN",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);
          neighborDownEvent(event.neighbor.nodeName, event.ifName);
          break;

        case thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE: {
          if (!useRttMetric_) {
            break;
          }

          logEvent(
              "NB_RTT_CHANGE",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);

          int32_t newRttMetric = getRttMetric(event.rttUs);
          VLOG(1) << "Metric value changed for neighbor "
                  << event.neighbor.nodeName << " to " << newRttMetric;
          const auto adjId =
              std::make_pair(event.neighbor.nodeName, event.ifName);
          auto it = adjacencies_.find(adjId);
          if (it != adjacencies_.end()) {
            auto& adj = it->second.second;
            adj.metric = newRttMetric;
            adj.rtt = event.rttUs;
          } else {
            // this occurs when a neighbor reports NEIGHBOR_UP but has not been
            // added into adjacencies bcoz of throttling
            auto _it = peerAddRequests_.find(adjId);
            DCHECK(_it != peerAddRequests_.end());
            auto& adj = _it->second.second;
            adj.metric = newRttMetric;
            adj.rtt = event.rttUs;
          }
          advertiseMyAdjacenciesThrottled_->operator()();
          break;
        }

        default:
          LOG(ERROR) << "Unknown event type " << (int32_t)event.eventType;
        }
      }); // sparkReportSock_ callback

  addSocket(
      fbzmq::RawZmqSocketPtr{*nlEventSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(2) << "LinkMonitor: Netlink Platform message received....";
        fbzmq::Message eventHeader, eventData;
        const auto ret = nlEventSub_.recvMultiple(eventHeader, eventData);
        if (ret.hasError()) {
          LOG(ERROR) << "Error processing PlatformPublisher event "
                     << "publication for node: " << nodeId_
                     << ", exception: " << ret.error();
          return;
        }

        auto eventMsg =
            eventData.readThriftObj<thrift::PlatformEvent>(serializer_);
        if (eventMsg.hasError()) {
          LOG(ERROR) << "Error in reading publication eventData";
          return;
        }

        const auto eventType = eventMsg.value().eventType;
        CHECK_EQ(
            static_cast<uint16_t>(eventType),
            eventHeader.read<uint16_t>().value());

        switch (eventType) {
        case thrift::PlatformEventType::LINK_EVENT: {
          VLOG(3) << "Received Link Event from Platform....";
          try {
            const auto linkEvt =
                fbzmq::util::readThriftObjStr<thrift::LinkEntry>(
                    eventMsg.value().eventData, serializer_);
            processLinkEvent(linkEvt);
          } catch (std::exception const& e) {
            LOG(ERROR) << "Error parsing linkEvt. Reason: "
                       << folly::exceptionStr(e);
          }
        } break;

        case thrift::PlatformEventType::ADDRESS_EVENT: {
          VLOG(3) << "Received Address Event from Platform....";
          try {
            const auto addrEvt =
                fbzmq::util::readThriftObjStr<thrift::AddrEntry>(
                    eventMsg.value().eventData, serializer_);
            processAddrEvent(addrEvt);
          } catch (std::exception const& e) {
            LOG(ERROR) << "Error parsing addrEvt. Reason: "
                       << folly::exceptionStr(e);
          }
        } break;

        default:
          LOG(ERROR) << "Wrong eventType received on " << nodeId_
                     << ", eventType: " << static_cast<uint16_t>(eventType);
        }
      });

  // Add callback for processing link-monitor requests on command socket
  addSocket(
      fbzmq::RawZmqSocketPtr{*linkMonitorCmdSock_},
      ZMQ_POLLIN,
      [this](int) noexcept {
        VLOG(2) << "LinkMonitor: processing LinkMonitor command";
        processCommand();
      });

  // Schedule callback to advertise the initial set of adjacencies and prefixes
  scheduleTimeoutAt(adjHoldUntilTimePoint_, [this]() noexcept {
    // Advertise adjacencies if not advertised yet
    if (advertiseAdj_) {
      advertiseMyAdjacencies();
      advertiseAdj_ = false;
    }

    // Advertise addresses
    advertiseRedistAddrs();
  });

  // Schedule periodic timer for monitor submission
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);

  // Schedule periodic timer for InterfaceDb re-sync from Netlink Platform
  interfaceDbSyncTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    auto success = syncInterfaces();
    if (success) {
      VLOG(2) << "InterfaceDb Sync is successful";
      expBackoff_.reportSuccess();
      interfaceDbSyncTimer_->scheduleTimeout(
          Constants::kPlatformSyncInterval, isPeriodic);
    } else {
      tData_.addStatValue(
          "link_monitor.thrift.failure.getAllLinks", 1, fbzmq::SUM);
      // Apply exponential backoff and schedule next run
      expBackoff_.reportError();
      interfaceDbSyncTimer_->scheduleTimeout(
          expBackoff_.getTimeRemainingUntilRetry());
      LOG(ERROR) << "InterfaceDb Sync failed, apply exponential "
                 << "backoff and retry in "
                 << expBackoff_.getTimeRemainingUntilRetry().count() << " ms";
    }
  });
  // schedule immediate with small timeout
  interfaceDbSyncTimer_->scheduleTimeout(std::chrono::milliseconds(100));

  sendIfDbTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { sendIfDbCallback(); });
}

void
LinkMonitor::neighborUpEvent(
    const thrift::BinaryAddress& neighborAddrV4,
    const thrift::BinaryAddress& neighborAddrV6,
    const thrift::SparkNeighborEvent& event) {
  const std::string& ifName = event.ifName;
  const std::string& remoteNodeName = event.neighbor.nodeName;
  const std::string& remoteIfName = event.neighbor.ifName;
  const auto adjId = std::make_pair(remoteNodeName, ifName);
  const int32_t neighborKvStorePubPort = event.neighbor.kvStorePubPort;
  const int32_t neighborKvStoreCmdPort = event.neighbor.kvStoreCmdPort;
  auto rttMetric = getRttMetric(event.rttUs);
  auto now = std::chrono::system_clock::now();
  // current unixtime in s
  int64_t timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  VLOG(1) << "LinkMonitor::neighborUpEvent called for '"
          << toString(neighborAddrV6)
          << "', nodeName: '" << remoteNodeName << "'"
          << ", nodeIfName: '" << remoteIfName << "'";
  syslog(
      LOG_NOTICE,
      "%s",
      folly::sformat(
          "Neighbor {} is up on interface {}.", remoteNodeName, ifName)
          .c_str());

  int64_t weight = 1;
  if (interfaceDb_.count(ifName)) {
    weight = interfaceDb_.at(ifName).getWeight();
  }

  thrift::Adjacency newAdj(
      FRAGILE,
      remoteNodeName /* otherNodeName */,
      ifName,
      neighborAddrV6 /* nextHopV6 */,
      neighborAddrV4 /* nextHopV4 */,
      (useRttMetric_ ? rttMetric : 1) /* metric */,
      enableSegmentRouting_ ? event.label : 0 /* adjacency-label */,
      false /* overload bit */,
      (useRttMetric_ ? event.rttUs : 0),
      timestamp,
      weight,
      remoteIfName /* otherIfName */);

  std::string pubUrl, repUrl;
  if (!mockMode_) {
    // use link local address
    pubUrl = folly::sformat(
        "tcp://[{}%{}]:{}",
        toString(neighborAddrV6),
        ifName,
        neighborKvStorePubPort);
    repUrl = folly::sformat(
        "tcp://[{}%{}]:{}",
        toString(neighborAddrV6),
        ifName,
        neighborKvStoreCmdPort);
  } else {
    // use inproc address
    pubUrl = folly::sformat("inproc://{}-kvstore-pub-global", remoteNodeName);
    repUrl = folly::sformat("inproc://{}-kvstore-cmd-global", remoteNodeName);
  }

  // two cases upon this event:
  // 1) the min interface changes: the previous min interface's connection will
  // be overridden by KvStoreClient, thus no need to explicitly remove it
  // 2) does not change: the existing connection to a neighbor is retained
  peerAddRequests_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(adjId),
      std::forward_as_tuple(std::make_pair(
          thrift::PeerSpec(FRAGILE, pubUrl, repUrl),
          std::move(newAdj))));

  // Advertise new adjancies in a throttled fashion
  advertiseMyAdjacenciesThrottled_->operator()();
}

void
LinkMonitor::neighborDownEvent(
    const std::string& remoteNodeName, const std::string& ifName) {
  const auto adjId = std::make_pair(remoteNodeName, ifName);

  VLOG(1) << "LinkMonitor::neighborDownEvent called for nodeName: '"
          << remoteNodeName << "', interface: '" << ifName << "'";
  syslog(
      LOG_NOTICE,
      "%s",
      folly::sformat(
          "Neighbor {} is down on interface {}.", remoteNodeName, ifName)
          .c_str());

  // we haven't activated this neighbor, skip updates
  if (peerAddRequests_.erase(adjId)) {
    VLOG(2) << "LinkMonitor::sessionDown down for '" << remoteNodeName
            << "' via interface '" << ifName
            << "', but has never been announced, ignoring...";
    return;
  }

  // take a snapshot of current peers
  auto oldPeers = getPeersFromAdjacencies();

  // udpate adjacencies_ and nbIfs_
  VLOG(2) << "Session to '" << remoteNodeName << "' via interface '" << ifName
          << "' down, removing immediately..";
  adjacencies_.erase(adjId);
  advertiseMyAdjacencies();

  auto& ifNames = nbIfs_[remoteNodeName];
  ifNames.erase(ifName);

  // take a snapshot of new required peers
  auto newPeers = getPeersFromAdjacencies();

  handlePeerChanges(oldPeers, newPeers);
}

void
LinkMonitor::processPendingPeerAddRequests() {
  LOG(INFO) << "Link Monitor: throttle timer has expired, accumulated "
            << peerAddRequests_.size() << " peer addition requests";
  // take a snapshot of current peers
  auto oldPeers = getPeersFromAdjacencies();

  // udpate adjacencies_ and nbIfs_
  for (auto const& peerKv : peerAddRequests_) {
    const auto& adjId = peerKv.first;
    adjacencies_[adjId] = peerKv.second;

    // adjId.first is node name, adjId.second is interface
    auto& ifNames = nbIfs_[adjId.first];
    ifNames.insert(adjId.second);
  }

  // take a snapshot of new required peers
  auto newPeers = getPeersFromAdjacencies();

  handlePeerChanges(oldPeers, newPeers);

  // Advertise our adjacencies
  advertiseMyAdjacencies();

  peerAddRequests_.clear();
}

std::unordered_map<std::string, thrift::PeerSpec>
LinkMonitor::getPeersForAllNeighbors() {
  std::unordered_map<std::string, thrift::PeerSpec> peers;

  for (const auto& adjKv : adjacencies_) {
    // adjkv is {<nodename, ifname> : <PeerSepc, Adjacency>}
    const auto& nodeName = adjKv.first.first;
    const auto& ifName = adjKv.first.second;

    const auto& ifNames = nbIfs_[nodeName];
    if (ifName == *ifNames.begin()) {
      // min interface
      peers[nodeName] = adjKv.second.first;
    }
  }
  return peers;
}

std::unordered_map<std::string, thrift::PeerSpec>
LinkMonitor::getPeersFromAdjacencies() {
  if (enableFullMeshReduction_) {
    // full mesh reduction
    std::string leaderName = nodeId_;
    for (const auto& adjKv : adjacencies_) {
      // adjkv is {<nodename, ifname> : <PeerSepc, Adjacency>}
      const auto& nodeName = adjKv.first.first;
      if (nodeName < leaderName) {
        leaderName = nodeName;
      }
    }

    if (leaderName == nodeId_) {
      // I am the leader, peer all my neighbors
      return getPeersForAllNeighbors();
    } else {
      std::unordered_map<std::string, thrift::PeerSpec> peers;
      // peer with leader only
      const auto& ifNames = nbIfs_.at(leaderName);
      const auto adjId = std::make_pair(leaderName, *ifNames.begin());
      peers[leaderName] = adjacencies_.at(adjId).first;
      return peers;
    }
  } else {
    // no full mesh reduction
    return getPeersForAllNeighbors();
  }
}

void
LinkMonitor::getPeerDifference(
    const std::unordered_map<std::string, thrift::PeerSpec>& oldPeers,
    const std::unordered_map<std::string, thrift::PeerSpec>& newPeers,
    std::vector<std::string>& toDelPeers,
    std::unordered_map<std::string, thrift::PeerSpec>& toAddPeers) {
  CHECK(toDelPeers.size() == 0) << "toDelPeers is not empty";
  CHECK(toAddPeers.size() == 0) << "toAddPeers is not empty";

  for (const auto& oldKv : oldPeers) {
    const auto& nodeName = oldKv.first;
    if (newPeers.count(nodeName) == 0) {
      toDelPeers.emplace_back(nodeName);
    }
  }

  for (const auto& newKv : newPeers) {
    const auto& nodeName = newKv.first;
    // Even if nodeName is the same, there is the chance that we are updating
    // session (in parallel link cases). So we have to check PeerSpec to decide
    // whether there's a update needed or not
    if (oldPeers.find(nodeName) == oldPeers.end() or
        oldPeers.at(nodeName) != newKv.second) {
      toAddPeers.emplace(nodeName, newKv.second);
    }
  }
}

void
LinkMonitor::handlePeerChanges(
    const std::unordered_map<std::string, thrift::PeerSpec>& oldPeers,
    const std::unordered_map<std::string, thrift::PeerSpec>& newPeers) {
  std::vector<std::string> toDelPeers;
  std::unordered_map<std::string, thrift::PeerSpec> toAddPeers;
  getPeerDifference(oldPeers, newPeers, toDelPeers, toAddPeers);

  // del peers in kvstore
  if (toDelPeers.size() > 0) {
    const auto ret = kvStoreClient_->delPeers(toDelPeers);
    CHECK(ret) << ret.error();
    logPeerEvent("DEL_PEER", toDelPeers);
  }

  // add peers in kvstore
  if (toAddPeers.size() > 0) {
    const auto peerNames = folly::gen::from(toAddPeers) | folly::gen::get<0>() |
        folly::gen::as<std::vector<std::string>>();
    const auto ret = kvStoreClient_->addPeers(std::move(toAddPeers));
    CHECK(ret) << ret.error();
    logPeerEvent("ADD_PEER", peerNames);
  }
}

void
LinkMonitor::advertiseMyAdjacencies() {
  if (std::chrono::steady_clock::now() < adjHoldUntilTimePoint_) {
    // Too early for advertising my own adjacencies. Try again after sometime.
    advertiseAdj_ = true;
    return;
  }

  // Update KvStore
  auto adjDb = thrift::AdjacencyDatabase();
  adjDb.thisNodeName = nodeId_;
  adjDb.isOverloaded = config_.isOverloaded;
  adjDb.nodeLabel = config_.nodeLabel;
  for (const auto& adjKv : adjacencies_) {
    // 'second.second' is the adj object for this peer
    // NOTE: copy on purpose
    auto adj = adjKv.second.second;

    // Set link overload bit
    adj.isOverloaded = config_.overloadedLinks.count(adj.ifName) > 0;

    // Override metric with link metric if it exists
    adj.metric =
        folly::get_default(config_.linkMetricOverrides, adj.ifName, adj.metric);

    // Override metric with adj metric if it exists
    thrift::AdjKey adjKey;
    adjKey.nodeName = adj.otherNodeName;
    adjKey.ifName = adj.ifName;
    adj.metric =
        folly::get_default(config_.adjMetricOverrides, adjKey, adj.metric);

    adjDb.adjacencies.emplace_back(std::move(adj));
  }

  // Add perf information if enabled
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "ADJ_DB_UPDATED");
    adjDb.perfEvents = perfEvents;
  } else {
    DCHECK(!adjDb.perfEvents.hasValue());
  }

  LOG(INFO) << "Updating adjacency database in KvStore with "
            << adjDb.adjacencies.size() << " entries.";
  const auto keyName = adjacencyDbMarker_ + nodeId_;
  std::string adjDbStr = fbzmq::util::writeThriftObjStr(adjDb, serializer_);
  kvStoreClient_->persistKey(keyName, adjDbStr, Constants::kKvStoreDbTtl);
  tData_.addStatValue("link_monitor.advertise_adjacencies", 1, fbzmq::SUM);

  // Config is most likely to have changed. Update it in `ConfigStore`
  configStoreClient_->storeThriftObj(kConfigKey, config_);
}

thrift::InterfaceDatabase
LinkMonitor::createInterfaceDatabase() {
  auto makeIfThrift =
      [this](const std::pair<std::string, LinkMonitor::InterfaceEntry>& ifState)
      -> std::pair<std::string, thrift::InterfaceInfo> {
    auto pair =
        std::make_pair(ifState.first, ifState.second.getInterfaceInfo());

    if (linkBackoffs_.count(ifState.first)) {
       auto& linkBackoff = linkBackoffs_.at(ifState.first);

       if (linkBackoff.second.canTryNow()) {
         // current timestamp
         auto timestamp = std::chrono::steady_clock::now();
         // clear backoff if interface keeps stable longer than threshold
         if (linkBackoff.first.hasValue() &&
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 timestamp - linkBackoff.first.value()) >= flapMaxBackoff_) {
           linkBackoff.second.reportSuccess();
         }
         linkBackoff.first.assign(timestamp);
       } else {
         // mark unstable interface as DOWN, don't let spark do
         // neighbor discovery
         pair.second.isUp = false;
       }
     }
    return pair;
  };

  thrift::InterfaceDatabase ifDb;
  ifDb.thisNodeName = nodeId_;
  ifDb.interfaces = folly::gen::from(interfaceDb_) |
      folly::gen::map(makeIfThrift) |
      folly::gen::as<std::map<std::string, thrift::InterfaceInfo>>();
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "INTF_DB_UPDATED");
    ifDb.perfEvents = std::move(perfEvents);
  }

  return ifDb;
}

void
LinkMonitor::sendInterfaceDatabase() {

  const auto ifDb = createInterfaceDatabase();

  // advertise interface database, prompting FIB to take immediate action
  // publish entire interface database
  const auto res1 = linkMonitorPubSock_.sendThriftObj(ifDb, serializer_);
  if (res1.hasError()) {
    LOG(ERROR) << "Exception in sending ifDb to linkMonitor for node: "
               << nodeId_ << " exception: " << res1.error();
  }

  // inform spark about interface database change
  const auto res2 = sparkCmdSock_.sendThriftObj(ifDb, serializer_);
  if (res2.hasError()) {
    LOG(ERROR) << "Exception in sending ifDb to Spark for node: " << nodeId_
               << " exception: " << res2.error();
  }
  const auto result =
      sparkCmdSock_.recvThriftObj<thrift::SparkIfDbUpdateResult>(serializer_);
  if (result.hasError()) {
    LOG(ERROR) << "Failed updating interface to Spark " << result.error();
  }

  // Return immediately if we are not configured to advertise interface db
  if (not advertiseInterfaceDb_) {
    return;
  }

  // advertise link database in KvStore
  const auto keyName = interfaceDbMarker_ + nodeId_;
  std::string intfDbStr = fbzmq::util::writeThriftObjStr(ifDb, serializer_);
  kvStoreClient_->persistKey(keyName, intfDbStr, Constants::kKvStoreDbTtl);
  tData_.addStatValue("link_monitor.advertise_links", 1, fbzmq::SUM);
}

void
LinkMonitor::sendIfDbCallback() {
  auto retryTime = getRetryTimeOnUnstableInterfaces();

  VLOG(3) << "<linkBackoffs_>:";
  for (const auto& kv : linkBackoffs_) {
    VLOG(3) << kv.first << ": "
            << kv.second.second.getTimeRemainingUntilRetry().count() << " ms";
  }

  // Send list of currently UP and STABLE interfaces
  sendInterfaceDatabase();

  // We will need to advertise UP but UNSTABLE interfaces once their backoff
  // is clear.
  if (retryTime.count() != 0) {
    sendIfDbTimer_->scheduleTimeout(retryTime);
    VLOG(3) << "sendIfDbTimer_ scheduled in " << retryTime.count() << " ms";
  }
}

void
LinkMonitor::createNetlinkSystemHandlerClient() {
  // Reset client if channel is not good
  if (socket_ && (!socket_->good() || socket_->hangup())) {
    client_.reset();
    socket_.reset();
  }

  // Do not create new client if one exists already
  if (client_) {
    return;
  }

  // Create socket to thrift server and set some connection parameters
  socket_ = apache::thrift::async::TAsyncSocket::newSocket(
      &evb_,
      Constants::kPlatformHost.toString(),
      platformThriftPort_,
      Constants::kPlatformConnTimeout.count());

  // Create channel and set timeout
  auto channel = apache::thrift::HeaderClientChannel::newChannel(socket_);
  channel->setTimeout(Constants::kPlatformProcTimeout.count());

  // Set BinaryProtocol and Framed client type for talkiing with thrift1 server
  channel->setProtocolId(apache::thrift::protocol::T_BINARY_PROTOCOL);
  channel->setClientType(THRIFT_FRAMED_DEPRECATED);

  // Reset client_
  client_ =
      std::make_unique<thrift::SystemServiceAsyncClient>(std::move(channel));
}

std::chrono::milliseconds
LinkMonitor::getRetryTimeOnUnstableInterfaces() {
  bool hasUnstableInterface = false;
  std::chrono::milliseconds minRemainMs = flapMaxBackoff_;
  for (const auto& kv : linkBackoffs_) {
    const auto& backoff = kv.second.second;
    const auto& curRemainMs = backoff.getTimeRemainingUntilRetry();
    if (curRemainMs.count() > 0) {
      minRemainMs = std::min(minRemainMs, curRemainMs);
      hasUnstableInterface = true;
    }
  }

  return hasUnstableInterface ? minRemainMs : std::chrono::milliseconds(0);
}

void
LinkMonitor::processLinkUpdatedEvent(const std::string& ifName, bool isUp) {
  VLOG(3) << "<link> update event on " << ifName;

  // send DOWN event immediately and other events in lazy fashion
  if (!isUp) {
    sendInterfaceDatabase();
  }

  if (!linkBackoffs_.count(ifName)) {
    // add backoff for newly added interface
    linkBackoffs_.emplace(
        ifName,
        std::make_pair(
          std::chrono::steady_clock::time_point(),
          ExponentialBackoff<std::chrono::milliseconds>(
              flapInitialBackoff_, flapMaxBackoff_)));
  }
  linkBackoffs_.at(ifName).second.reportError();

  auto retryTime = getRetryTimeOnUnstableInterfaces();
  sendIfDbTimer_->scheduleTimeout(retryTime);
  VLOG(3) << "sendIfDbTimer_ scheduled in " << retryTime.count() << " ms";
}

bool
LinkMonitor::updateLinkEvent(const thrift::LinkEntry& linkEntry) {
  const std::string& ifName = linkEntry.ifName;
  const auto isUp = linkEntry.isUp;
  const auto ifIndex = linkEntry.ifIndex;
  const auto weight = linkEntry.weight;

  if (!isUp and redistAddrs_.erase(ifName)) {
    advertiseRedistAddrs();
  }

  if (!checkIncludeExcludeRegex(ifName, includeRegexList_, excludeRegexList_)) {
    VLOG(2) << "Interface " << ifName << " does not match iface regexes";
    return false;
  }

  bool isUpdated = false;
  if (interfaceDb_.count(ifName)) {
    VLOG(3) << "Updating " << ifName << " : " << interfaceDb_.at(ifName);
    isUpdated = interfaceDb_.at(ifName).updateEntry(ifIndex, isUp, weight);
    VLOG(3) << (isUpdated ? "Updated " : "No updates to ") << ifName << " : "
            << interfaceDb_.at(ifName);
  } else {
    isUpdated = true;
    interfaceDb_[ifName] = InterfaceEntry(ifIndex, isUp);
    VLOG(3) << "Added " << ifName << " : " << interfaceDb_.at(ifName);
  }

  return isUpdated;
}

bool
LinkMonitor::updateAddrEvent(const thrift::AddrEntry& addrEntry) {
  const std::string& ifName = addrEntry.ifName;

  // Add address if it is supposed to be announced
  if (checkRedistIfNameRegex(ifName)) {
    addDelRedistAddr(ifName, addrEntry.isValid, addrEntry.ipPrefix);
  }

  if (!checkIncludeExcludeRegex(ifName, includeRegexList_, excludeRegexList_)) {
    VLOG(2) << "Interface " << ifName << " does not match iface regexes";
    return false;
  }

  bool isUpdated = false;
  auto ipNetwork = toIPNetwork(addrEntry.ipPrefix, false);
  bool isValid = addrEntry.isValid;
  auto& intf = interfaceDb_.at(ifName);

  VLOG(3) << "<addr> event: " << ipNetwork.first.str()
          << "/" << +ipNetwork.second
          << (isValid ? " add" : " delete")
          << " on " << ifName;
  VLOG(3) << "Updating " << ifName << " : " << intf;
  isUpdated = intf.updateEntry(ipNetwork, isValid) && intf.isUp();
  VLOG(3) << (isUpdated ? "Updated " : "No updates to ") << ifName << " : "
            << intf;

  return isUpdated;
}

void
LinkMonitor::processLinkEvent(const thrift::LinkEntry& linkEntry) {
  const std::string& ifName = linkEntry.ifName;
  const auto isUp = linkEntry.isUp;
  const auto ifIndex = linkEntry.ifIndex;
  const auto weight = linkEntry.weight;

  VLOG(3) << "<link> event: " << (isUp ? "UP" : "DOWN") << " for " << ifName
          << ", ifIndex: " << ifIndex << ", weight: " << weight;

  const auto isUpdated = updateLinkEvent(linkEntry);

  if (isUpdated) {
    syslog(
        LOG_NOTICE,
        "%s",
        folly::sformat("Interface {} is {}.", ifName, (isUp ? "UP" : "DOWN"))
            .c_str());
    logLinkEvent((isUp ? "IFACE_UP" : "IFACE_DOWN"), ifName);
    processLinkUpdatedEvent(ifName, isUp);
  }
}

void
LinkMonitor::processAddrEvent(const thrift::AddrEntry& addrEntry) {
  const std::string& ifName = addrEntry.ifName;

  // There is chance that netlink has not sent interface event yet
  // before an address event
  // If the interface entry doesn't exist, we create one in interfaceDb_ here
  bool invalidLinkInfo = false;
  if (!interfaceDb_.count(ifName)) {
    LOG(WARNING) << "Received address event before interface up/down event for "
                 << ifName << ". Adding...";
    interfaceDb_.emplace(ifName, InterfaceEntry(0 /*ifIndex*/, false /*isUp*/));
    invalidLinkInfo = true;
  }

  const auto isUpdated = updateAddrEvent(addrEntry);

  if (!invalidLinkInfo and isUpdated) {
    VLOG(3) << "<addr> event updated on " << ifName;
    sendInterfaceDatabase();
  }
}

bool
LinkMonitor::syncInterfaces() {
  VLOG(1) << "Syncing Interface DB from Netlink Platform";

  //
  // Retrieve latest link snapshot from SystemService
  //
  std::vector<thrift::Link> links;
  try {
    createNetlinkSystemHandlerClient();
    client_->sync_getAllLinks(links);
  } catch (const std::exception& e) {
    client_.reset();
    LOG(ERROR) << "Failed to sync LinkDb from NetlinkSystemHandler. Error: "
               << folly::exceptionStr(e);
    return false;
  }

  //
  // Process received data. We convert received data to link and addr events
  // and invoke our updateLinkEvent and updateAddrEvent handlers
  //
  bool isUpdated = false;
  for (const auto& link : links) {
    // Process link entry
    const thrift::LinkEntry linkEntry(
        apache::thrift::FRAGILE,
        link.ifName,
        link.ifIndex,
        link.isUp,
        link.weight);
    isUpdated |= updateLinkEvent(linkEntry);

    // Process each addr entry
    for (const auto& network : link.networks) {
      const thrift::AddrEntry addrEntry(
          apache::thrift::FRAGILE,
          link.ifName,
          network,
          true /* is valid */);
      isUpdated |= updateAddrEvent(addrEntry);
    }
  }

  // Send an update only if there is an update
  if (isUpdated) {
    VLOG(1) << "Completed sync of Interface DB from netlink";
    sendInterfaceDatabase();
  }

  return true;
}

void
LinkMonitor::processCommand() {
  // read the request id supplied by router socket
  auto maybeClientIdMessage = linkMonitorCmdSock_.recvOne();
  if (maybeClientIdMessage.hasError()) {
    LOG(ERROR) << maybeClientIdMessage.error();
    return;
  }
  auto clientIdMessage = maybeClientIdMessage.value();

  // read actual request
  const auto maybeReq =
      linkMonitorCmdSock_.recvThriftObj<thrift::LinkMonitorRequest>(
          serializer_);
  if (maybeReq.hasError()) {
    LOG(ERROR) << "Error receiving LinkMonitorRequest: " << maybeReq.error();
    return;
  }

  // NOTE: add commands which set/unset overload bit or metric values will
  // immediately advertise new adjacencies into the KvStore.
  const auto& req = maybeReq.value();
  switch (req.cmd) {
  case thrift::LinkMonitorCommand::SET_OVERLOAD:
    if (config_.isOverloaded) {
      // node already in overloaded state, do nothing
      break;
    }
    LOG(INFO) << "Setting overload bit for node.";
    config_.isOverloaded = true;
    advertiseMyAdjacencies();
    break;

  case thrift::LinkMonitorCommand::UNSET_OVERLOAD:
    if (not config_.isOverloaded) {
      // node is not in overloaded state, do nothing
      break;
    }
    LOG(INFO) << "Unsetting overload bit for node.";
    config_.isOverloaded = false;
    advertiseMyAdjacencies();
    break;

  case thrift::LinkMonitorCommand::SET_LINK_OVERLOAD:
    if (0 == interfaceDb_.count(req.interfaceName)) {
      LOG(ERROR) << "SET_LINK_OVERLOAD requested for unknown interface: "
                 << req.interfaceName;
      break;
    }
    if (config_.overloadedLinks.count(req.interfaceName)) {
      // interface is already overloaded
      break;
    }
    LOG(INFO) << "Setting overload bit for interface " << req.interfaceName;
    config_.overloadedLinks.insert(req.interfaceName);
    advertiseMyAdjacencies();
    break;

  case thrift::LinkMonitorCommand::UNSET_LINK_OVERLOAD:
    if (config_.overloadedLinks.erase(req.interfaceName)) {
      LOG(INFO) << "Unsetting overload bit for interface " << req.interfaceName;
      advertiseMyAdjacencies();
    } else {
      LOG(WARNING) << "Got unset-overload-bit request for unknown link "
                   << req.interfaceName;
    }
    break;

  case thrift::LinkMonitorCommand::SET_LINK_METRIC:
    if (0 == interfaceDb_.count(req.interfaceName)) {
      LOG(ERROR) << "SET_LINK_METRIC requested for unknown interface: "
                 << req.interfaceName;
      break;
    }
    if (req.overrideMetric < 1) {
      LOG(ERROR) << "Minimum allowed metric value for link is 1. Can't set "
                 << "a value smaller than that. Got " << req.overrideMetric;
      break;
    }
    LOG(INFO) << "Overriding metric for interface " << req.interfaceName
              << " to " << req.overrideMetric;
    config_.linkMetricOverrides[req.interfaceName] = req.overrideMetric;
    advertiseMyAdjacencies();
    break;

  case thrift::LinkMonitorCommand::UNSET_LINK_METRIC:
    if (config_.linkMetricOverrides.erase(req.interfaceName)) {
      LOG(INFO) << "Removing metric override for interface "
                << req.interfaceName;
      advertiseMyAdjacencies();
    } else {
      LOG(WARNING) << "Got link-metric-unset request for unknown interface "
                   << req.interfaceName;
    }
    break;

  case thrift::LinkMonitorCommand::DUMP_LINKS: {
    VLOG(2) << "Dump Links requested, replying with " << interfaceDb_.size()
            << " links";

    auto makeIfDetails =
        [this](const std::pair<std::string, LinkMonitor::InterfaceEntry>& intf)
        -> std::pair<std::string, thrift::InterfaceDetails> {
      auto ifDetails = thrift::InterfaceDetails(
          apache::thrift::FRAGILE,
          intf.second.getInterfaceInfo(),
          config_.overloadedLinks.count(intf.first) > 0,
          0 /* custom metric value */,
          0 /* link flap back off time */);

      folly::Optional<int32_t> maybeMetric;
      if (config_.linkMetricOverrides.count(intf.first) > 0) {
        maybeMetric.assign(config_.linkMetricOverrides.at(intf.first));
      }
      ifDetails.metricOverride = maybeMetric;

      if (linkBackoffs_.count(intf.first) != 0) {
        ifDetails.linkFlapBackOffMs = linkBackoffs_.at(intf.first)
                                          .second.getTimeRemainingUntilRetry()
                                          .count();
      }

      return std::make_pair(intf.first, ifDetails);
    };

    // reply with the dump of known interfaces and their states
    thrift::DumpLinksReply reply;
    reply.thisNodeName = nodeId_;
    reply.isOverloaded = config_.isOverloaded;
    reply.interfaceDetails =
        folly::gen::from(interfaceDb_) | folly::gen::map(makeIfDetails) |
        folly::gen::as<
            std::unordered_map<std::string, thrift::InterfaceDetails>>();

    auto ret = linkMonitorCmdSock_.sendMultiple(
        clientIdMessage,
        fbzmq::Message::fromThriftObj(reply, serializer_).value());
    if (ret.hasError()) {
      LOG(ERROR) << "Error sending response. " << ret.error();
    }
    break;
  }

  case thrift::LinkMonitorCommand::SET_ADJ_METRIC: {
    if (req.overrideMetric < 1) {
      LOG(ERROR) << "Minimum allowed metric value for adjacency is 1. Can't set"
                 << " a value smaller than that. Got " << req.overrideMetric;
      break;
    }
    if (req.adjNodeName == folly::none) {
      LOG(ERROR) << "SET_ADJ_METRIC - adjacency node name not provided, "
                 << req.interfaceName;
      break;
    }
    thrift::AdjKey adjKey;
    adjKey.ifName = req.interfaceName;
    adjKey.nodeName = req.adjNodeName.value();
    config_.adjMetricOverrides[adjKey] = req.overrideMetric;

    if (adjacencies_.count(std::make_pair(req.adjNodeName.value(),
                                                req.interfaceName))) {
      LOG(INFO) << "Overriding metric for adjacency "
                << req.adjNodeName.value() << " "
                << req.interfaceName << " to " << req.overrideMetric;
      advertiseMyAdjacencies();

    } else {
      LOG(WARNING) << "SET_ADJ_METRIC - adjacency is not yet formed for: "
                 << req.adjNodeName.value() << " " << req.interfaceName;
    }
    break;
  }

  case thrift::LinkMonitorCommand::UNSET_ADJ_METRIC: {
    if (req.adjNodeName == folly::none) {
      LOG(ERROR) << "UNSET_ADJ_METRIC - adjacency node name not provided, "
                 << req.interfaceName;
      break;
    }
    thrift::AdjKey adjKey;
    adjKey.ifName = req.interfaceName;
    adjKey.nodeName = req.adjNodeName.value();

    if (config_.adjMetricOverrides.erase(adjKey)) {
      LOG(INFO) << "Removing metric override for adjacency "
                << req.adjNodeName.value() << " " << req.interfaceName;

        if (adjacencies_.count(std::make_pair(req.adjNodeName.value(),
                                                req.interfaceName))) {
          advertiseMyAdjacencies();
        }
    } else {
      LOG(WARNING) << "Got adj-metric-unset request for unknown adjacency"
                    << req.adjNodeName.value() << " " << req.interfaceName;
    }
    break;
  }

  case thrift::LinkMonitorCommand::GET_VERSION: {

    thrift::OpenrVersions openrVersion(apache::thrift::FRAGILE,
              Constants::kOpenrVersion, Constants::kOpenrSupportedVersion);

    auto ret = linkMonitorCmdSock_.sendMultiple(
        clientIdMessage,
        fbzmq::Message::fromThriftObj(openrVersion, serializer_).value());
    if (ret.hasError()) {
      LOG(ERROR) << "Error sending version response. " << ret.error();
    }
    break;
  }

  case thrift::LinkMonitorCommand::GET_BUILD_INFO: {
    auto buildInfo = getBuildInfoThrift();
    auto ret = linkMonitorCmdSock_.sendMultiple(
        clientIdMessage,
        fbzmq::Message::fromThriftObj(buildInfo, serializer_).value());
    if (ret.hasError()) {
      LOG(ERROR) << "Error sending version response. " << ret.error();
    }
    break;
  }

  default:
    LOG(ERROR) << "Link Monitor received unknown command: "
               << static_cast<int>(req.cmd);
    break;
  }
}

void
LinkMonitor::addDelRedistAddr(
    const std::string& ifName, bool isValid, const thrift::IpPrefix& prefix) {
  bool isUpdated = false;
  // NOTE: this will mask the address.
  auto const ipNetwork = toIPNetwork(prefix);
  auto const ip = ipNetwork.first;
  // Ignore irrelevant ip addresses.
  if (ip.isLoopback() || ip.isLinkLocal() || ip.isMulticast() ||
      (ip.isV4() && !enableV4_)) {
    return;
  }

  auto const prefixToInsert = toIpPrefix(ipNetwork);
  // If address is invalid then try to remove from list if it exists
  if (!isValid and redistAddrs_.count(ifName)) {
    isUpdated |= redistAddrs_.at(ifName).erase(prefixToInsert) > 0;
    if (!redistAddrs_.at(ifName).size()) {
      redistAddrs_.erase(ifName);
    }
  }

  // If address is valid then add it to list if it doesn't exists
  if (isValid) {
    isUpdated = redistAddrs_[ifName].insert(prefixToInsert).second;
  }

  // Advertise updates if there is any change
  if (isUpdated) {
    advertiseRedistAddrs();
  }
}

void
LinkMonitor::advertiseRedistAddrs() {
  std::vector<thrift::PrefixEntry> prefixes;

  // Add static prefixes
  for (auto const& prefix : staticPrefixes_) {
    prefixes.emplace_back(thrift::PrefixEntry(
        apache::thrift::FRAGILE, prefix, thrift::PrefixType::LOOPBACK, ""));
  }

  // Add redistribute addresses
  for (auto const& kv : redistAddrs_) {
    for (auto const& prefix : kv.second) {
      prefixes.emplace_back(thrift::PrefixEntry(
          apache::thrift::FRAGILE, prefix, thrift::PrefixType::LOOPBACK, ""));
    }
  }

  // Advertise via prefix manager client
  prefixManagerClient_->syncPrefixesByType(
      thrift::PrefixType::LOOPBACK, prefixes);
}

void
LinkMonitor::submitCounters() {
  VLOG(3) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  counters["link_monitor.adjacencies"] = adjacencies_.size();
  for (const auto& kv : adjacencies_) {
    auto& adj = kv.second.second;
    counters["link_monitor.metric." + adj.otherNodeName] = adj.metric;
  }

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

void
LinkMonitor::logLinkEvent(const std::string& event, const std::string& iface) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "LinkMonitor");
  sample.addString("node_name", nodeId_);
  sample.addString("interface", iface);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

void
LinkMonitor::logPeerEvent(
    const std::string& event, const std::vector<std::string>& peers) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "LinkMonitor");
  sample.addString("node_name", nodeId_);
  sample.addStringVector("peers", peers);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

thrift::InterfaceInfo
LinkMonitor::InterfaceEntry::getInterfaceInfo() const {

  std::vector<thrift::IpPrefix> networks;
  for (const auto& network : networks_) {
    networks.emplace_back(toIpPrefix(network));
  }

  return thrift::InterfaceInfo(
      FRAGILE,
      isUp_,
      ifIndex_,
      // TO BE DEPERECATED SOON
      folly::gen::from(getV4Addrs()) |
        folly::gen::map(
          [](const folly::IPAddress& ip) {
            return toBinaryAddress(ip);
          }) |
        folly::gen::as<std::vector>(),
      // TO BE DEPRECATED SOON
      folly::gen::from(getV6LinkLocalAddrs()) |
        folly::gen::map(
          [](const folly::IPAddress& ip) {
            return toBinaryAddress(ip);
          }) |
        folly::gen::as<std::vector>(),
      networks);
}

bool
LinkMonitor::checkRedistIfNameRegex(const std::string& ifName) {
  if (!redistRegexList_) {
    return false;
  }
  std::vector<int> matches;
  return redistRegexList_->Match(ifName, &matches);
}

void
LinkMonitor::logEvent(
    const std::string& event,
    const std::string& neighbor,
    const std::string& iface,
    const std::string& remoteIface) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "LinkMonitor");
  sample.addString("node_name", nodeId_);
  sample.addString("neighbor", neighbor);
  sample.addString("interface", iface);
  sample.addString("remote_interface", remoteIface);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

} // namespace openr
