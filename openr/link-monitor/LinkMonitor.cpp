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

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
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
    bool enablePerfMeasurement,
    bool enableV4,
    bool enableSegmentRouting,
    bool forwardingTypeMpls,
    AdjacencyDbMarker adjacencyDbMarker,
    SparkCmdUrl sparkCmdUrl,
    SparkReportUrl sparkReportUrl,
    MonitorSubmitUrl const& monitorSubmitUrl,
    PersistentStoreUrl const& configStoreUrl,
    bool assumeDrained,
    PrefixManagerLocalCmdUrl const& prefixManagerUrl,
    PlatformPublisherUrl const& platformPubUrl,
    LinkMonitorGlobalPubUrl linkMonitorGlobalPubUrl,
    folly::Optional<std::string> linkMonitorGlobalCmdUrl,
    std::chrono::seconds adjHoldTime,
    std::chrono::milliseconds flapInitialBackoff,
    std::chrono::milliseconds flapMaxBackoff,
    std::chrono::milliseconds ttlKeyInKvStore)
    : OpenrEventLoop(
          nodeId,
          thrift::OpenrModuleType::LINK_MONITOR,
          zmqContext,
          linkMonitorGlobalCmdUrl),
      nodeId_(nodeId),
      platformThriftPort_(platformThriftPort),
      kvStoreLocalCmdUrl_(kvStoreLocalCmdUrl),
      kvStoreLocalPubUrl_(kvStoreLocalPubUrl),
      includeRegexList_(std::move(includeRegexList)),
      excludeRegexList_(std::move(excludeRegexList)),
      redistRegexList_(std::move(redistRegexList)),
      staticPrefixes_(staticPrefixes),
      useRttMetric_(useRttMetric),
      enablePerfMeasurement_(enablePerfMeasurement),
      enableV4_(enableV4),
      enableSegmentRouting_(enableSegmentRouting),
      forwardingTypeMpls_(forwardingTypeMpls),
      adjacencyDbMarker_(adjacencyDbMarker),
      sparkCmdUrl_(sparkCmdUrl),
      sparkReportUrl_(sparkReportUrl),
      platformPubUrl_(platformPubUrl),
      linkMonitorGlobalPubUrl_(linkMonitorGlobalPubUrl),
      flapInitialBackoff_(flapInitialBackoff),
      flapMaxBackoff_(flapMaxBackoff),
      ttlKeyInKvStore_(ttlKeyInKvStore),
      adjHoldUntilTimePoint_(std::chrono::steady_clock::now() + adjHoldTime),
      // mutable states
      linkMonitorPubSock_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      sparkCmdSock_(zmqContext),
      sparkReportSock_(
          zmqContext,
          fbzmq::IdentityString{Constants::kSparkReportClientId.toString()},
          folly::none,
          fbzmq::NonblockingFlag{true}),
      nlEventSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      expBackoff_(Constants::kInitialBackoff, Constants::kMaxBackoff) {
  // Create throttled adjacency advertiser
  advertiseAdjacenciesThrottled_ = std::make_unique<fbzmq::ZmqThrottle>(
      this, Constants::kLinkThrottleTimeout, [this]() noexcept {
        advertiseKvStorePeers();
        advertiseAdjacencies();
      });

  // Create throttled interfaces and addresses advertiser
  advertiseIfaceAddrThrottled_ = std::make_unique<fbzmq::ZmqThrottle>(
      this, Constants::kLinkThrottleTimeout, [this]() noexcept {
        advertiseIfaceAddr();
      });
  // Create timer. Timer is used for immediate or delayed executions.
  advertiseIfaceAddrTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() noexcept { advertiseIfaceAddr(); });

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
      LOG(WARNING) << folly::sformat(
          "Failed to load link-monitor config. "
          "Setting node as {}",
          assumeDrained ? "DRAINED" : "UNDRAINED");
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
          advertiseAdjacencies();
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

  VLOG(2) << "Connect to Spark to send commands on " << sparkCmdUrl_;

  // Subscribe to events published by Spark for neighbor state changes
  const auto sparkCmd = sparkCmdSock_.connect(fbzmq::SocketUrl{sparkCmdUrl_});
  if (sparkCmd.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << sparkCmdUrl_ << "' "
               << sparkCmd.error();
  }
  const int relaxed = 1;
  const auto sparkCmdOptRelaxed =
      sparkCmdSock_.setSockOpt(ZMQ_REQ_RELAXED, &relaxed, sizeof(int));
  if (sparkCmdOptRelaxed.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_REQ_RELAXED option";
  }

  const int correlate = 1;
  const auto sparkCmdOptCorrelate =
      sparkCmdSock_.setSockOpt(ZMQ_REQ_CORRELATE, &correlate, sizeof(int));
  if (sparkCmdOptCorrelate.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_REQ_CORRELATE option";
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

        fbzmq::Message requestIdMsg, delimMsg, thriftMsg;
        const auto ret =
            sparkReportSock_.recvMultiple(requestIdMsg, delimMsg, thriftMsg);

        if (ret.hasError()) {
          LOG(ERROR) << "sparkReportSock: Error receiving command: "
                     << ret.error();
          return;
        }

        const auto requestId = requestIdMsg.read<std::string>().value();
        const auto delim = delimMsg.read<std::string>().value();
        if (not delimMsg.empty()) {
          LOG(ERROR) << "sparkReportSock: Non-empty delimiter: " << delim;
          return;
        }

        VLOG(3) << "sparkReportSock, got id: `"
                << folly::backslashify(requestId) << "` and delim: `"
                << folly::backslashify(delim) << "`";

        const auto maybeEvent =
            thriftMsg.readThriftObj<thrift::SparkNeighborEvent>(serializer_);

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
        case thrift::SparkNeighborEventType::NEIGHBOR_UP: {
          logNeighborEvent(
              "NB_UP",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);
          neighborUpEvent(neighborAddrV4, neighborAddrV6, event);
          break;
        }

        case thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING: {
          logNeighborEvent(
              "NB_RESTARTING",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);
          neighborRestartingEvent(event.neighbor.nodeName, event.ifName);
          break;
        }

        case thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED: {
          logNeighborEvent(
              "NB_RESTARTED",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);
          neighborUpEvent(neighborAddrV4, neighborAddrV6, event);
          break;
        }

        case thrift::SparkNeighborEventType::NEIGHBOR_DOWN: {
          logNeighborEvent(
              "NB_DOWN",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);
          neighborDownEvent(event.neighbor.nodeName, event.ifName);
          break;
        }

        case thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE: {
          if (!useRttMetric_) {
            break;
          }

          logNeighborEvent(
              "NB_RTT_CHANGE",
              event.neighbor.nodeName,
              event.ifName,
              event.neighbor.ifName);

          int32_t newRttMetric = getRttMetric(event.rttUs);
          VLOG(1) << "Metric value changed for neighbor "
                  << event.neighbor.nodeName << " to " << newRttMetric;
          auto it = adjacencies_.find({event.neighbor.nodeName, event.ifName});
          if (it != adjacencies_.end()) {
            auto& adj = it->second.adjacency;
            adj.metric = newRttMetric;
            adj.rtt = event.rttUs;
            advertiseAdjacenciesThrottled_->operator()();
          }
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
            auto interfaceEntry = getOrCreateInterfaceEntry(linkEvt.ifName);
            if (interfaceEntry) {
              const bool wasUp = interfaceEntry->isUp();
              interfaceEntry->updateAttrs(
                  linkEvt.ifIndex, linkEvt.isUp, linkEvt.weight);
              logLinkEvent(
                  interfaceEntry->getIfName(),
                  wasUp,
                  interfaceEntry->isUp(),
                  interfaceEntry->getBackoffDuration());
            }
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
            auto interfaceEntry = getOrCreateInterfaceEntry(addrEvt.ifName);
            if (interfaceEntry) {
              interfaceEntry->updateAddr(
                  toIPNetwork(addrEvt.ipPrefix, false /* no masking */),
                  addrEvt.isValid);
            }
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

  // Schedule callback to advertise the initial set of adjacencies and prefixes
  scheduleTimeoutAt(adjHoldUntilTimePoint_, [this]() noexcept {
    LOG(INFO) << "Hold time expired. Advertising adjacencies and addresses";
    // Advertise adjacencies and addresses after hold-timeout
    advertiseAdjacencies();
    advertiseRedistAddrs();

    // Cancel throttle as we are publishing latest state
    if (advertiseAdjacenciesThrottled_->isActive()) {
      advertiseAdjacenciesThrottled_->cancel();
    }
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
          << toString(neighborAddrV6) << "%" << ifName << "', nodeName: '"
          << remoteNodeName << "'"
          << ", nodeIfName: '" << remoteIfName << "'";
  syslog(
      LOG_NOTICE,
      "%s",
      folly::sformat(
          "Neighbor {} is up on interface {}.", remoteNodeName, ifName)
          .c_str());

  int64_t weight = 1;
  if (interfaces_.count(ifName)) {
    weight = interfaces_.at(ifName).getWeight();
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
  adjacencies_[adjId] = AdjacencyValue(
      thrift::PeerSpec(FRAGILE, pubUrl, repUrl, event.supportFloodOptimization),
      std::move(newAdj));

  // Advertise KvStore peers immediately
  advertiseKvStorePeers();

  // Advertise new adjancies in a throttled fashion
  advertiseAdjacenciesThrottled_->operator()();
}

void
LinkMonitor::neighborDownEvent(
    const std::string& remoteNodeName, const std::string& ifName) {
  const auto adjId = std::make_pair(remoteNodeName, ifName);

  VLOG(1) << "LinkMonitor::neighborDownEvent called for nodeName: '"
          << remoteNodeName << "', interface: '" << ifName << "'.";
  syslog(
      LOG_NOTICE,
      "%s",
      folly::sformat(
          "Neighbor {} is down on interface {}.", remoteNodeName, ifName)
          .c_str());

  // remove such adjacencies
  adjacencies_.erase(adjId);

  // advertise both peers and adjacencies
  advertiseKvStorePeers();
  advertiseAdjacencies();
}

void
LinkMonitor::neighborRestartingEvent(
    const std::string& remoteNodeName, const std::string& ifName) {
  const auto adjId = std::make_pair(remoteNodeName, ifName);

  VLOG(1) << "LinkMonitor::neighborRestartingEvent called for nodeName: '"
          << remoteNodeName << "', interface: '" << ifName << "'";
  syslog(
      LOG_NOTICE,
      "%s",
      folly::sformat(
          "Neighbor {} is restarting on interface {}.", remoteNodeName, ifName)
          .c_str());

  // update adjacencies_ restarting-bit and advertise peers
  if (adjacencies_.count(adjId)) {
    adjacencies_.at(adjId).isRestarting = true;
  }
  advertiseKvStorePeers();
}

std::unordered_map<std::string, thrift::PeerSpec>
LinkMonitor::getPeersFromAdjacencies(
    const std::unordered_map<AdjacencyKey, AdjacencyValue>& adjacencies) {
  std::unordered_map<std::string, std::string> neighborToIface;
  for (const auto& adjKv : adjacencies) {
    if (adjKv.second.isRestarting) {
      // ignore restarting adj
      continue;
    }
    const auto& nodeName = adjKv.first.first;
    const auto& iface = adjKv.first.second;

    // Look up for node
    auto it = neighborToIface.find(nodeName);
    if (it == neighborToIface.end()) {
      // Add nbr-iface if not found
      neighborToIface.emplace(nodeName, iface);
    } else if (it->second > iface) {
      // Update iface if it is smaller (minimum interface)
      it->second = iface;
    }
  }

  std::unordered_map<std::string, thrift::PeerSpec> peers;
  for (const auto& kv : neighborToIface) {
    peers.emplace(kv.first, adjacencies.at(kv).peerSpec);
  }
  return peers;
}

void
LinkMonitor::advertiseKvStorePeers() {
  // Get old and new peer list. Also update local state
  const auto oldPeers = std::move(peers_);
  peers_ = getPeersFromAdjacencies(adjacencies_);
  const auto& newPeers = peers_;

  // Get list of peers to delete
  std::vector<std::string> toDelPeers;
  for (const auto& oldKv : oldPeers) {
    const auto& nodeName = oldKv.first;
    if (newPeers.count(nodeName) == 0) {
      toDelPeers.emplace_back(nodeName);
      logPeerEvent("DEL_PEER", oldKv.first, oldKv.second);
    }
  }

  // Delete old peers
  if (toDelPeers.size() > 0) {
    const auto ret = kvStoreClient_->delPeers(toDelPeers);
    CHECK(ret) << ret.error();
  }

  // Get list of peers to add
  std::unordered_map<std::string, thrift::PeerSpec> toAddPeers;
  for (const auto& newKv : newPeers) {
    const auto& nodeName = newKv.first;
    // Even if nodeName is the same, there is the chance that we are updating
    // session (in parallel link cases). So we have to check PeerSpec to decide
    // whether there's a update needed or not
    if (oldPeers.find(nodeName) == oldPeers.end() or
        oldPeers.at(nodeName) != newKv.second) {
      toAddPeers.emplace(nodeName, newKv.second);
      logPeerEvent("ADD_PEER", newKv.first, newKv.second);
    }
  }

  // Add new peers
  if (toAddPeers.size() > 0) {
    const auto ret = kvStoreClient_->addPeers(std::move(toAddPeers));
    CHECK(ret) << ret.error();
  }
}

void
LinkMonitor::advertiseAdjacencies() {
  if (std::chrono::steady_clock::now() < adjHoldUntilTimePoint_) {
    // Too early for advertising my own adjacencies. Let timeout advertise it
    // and skip here.
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
    auto adj = adjKv.second.adjacency;

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
  kvStoreClient_->persistKey(keyName, adjDbStr, ttlKeyInKvStore_);
  tData_.addStatValue("link_monitor.advertise_adjacencies", 1, fbzmq::SUM);

  // Config is most likely to have changed. Update it in `ConfigStore`
  configStoreClient_->storeThriftObj(kConfigKey, config_);

  // Cancel throttle timeout if scheduled
  if (advertiseAdjacenciesThrottled_->isActive()) {
    advertiseAdjacenciesThrottled_->cancel();
  }
}

void
LinkMonitor::advertiseIfaceAddr() {
  auto retryTime = getRetryTimeOnUnstableInterfaces();

  advertiseInterfaces();
  advertiseRedistAddrs();

  // Cancel throttle timeout if scheduled
  if (advertiseIfaceAddrThrottled_->isActive()) {
    advertiseIfaceAddrThrottled_->cancel();
  }

  // Schedule new timeout if needed to advertise UP but UNSTABLE interfaces
  // once their backoff is clear.
  if (retryTime.count() != 0) {
    advertiseIfaceAddrTimer_->scheduleTimeout(retryTime);
    VLOG(2) << "advertiseIfaceAddr timer scheduled in " << retryTime.count()
            << " ms";
  }
}

void
LinkMonitor::advertiseInterfaces() {
  tData_.addStatValue("link_monitor.advertise_links", 1, fbzmq::SUM);

  // Create interface database
  thrift::InterfaceDatabase ifDb;
  ifDb.thisNodeName = nodeId_;
  for (auto& kv : interfaces_) {
    auto& ifName = kv.first;
    auto& interface = kv.second;
    // Perform regex match
    if (not checkIncludeExcludeRegex(
            ifName, includeRegexList_, excludeRegexList_)) {
      continue;
    }
    // Get interface info and override active status
    auto interfaceInfo = interface.getInterfaceInfo();
    interfaceInfo.isUp = interface.isActive();
    ifDb.interfaces.emplace(ifName, std::move(interfaceInfo));
  }

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
      sparkCmdSock_.recvThriftObj<thrift::SparkIfDbUpdateResult>(
          serializer_, Constants::kReadTimeout);
  if (result.hasError()) {
    LOG(ERROR) << "Failed updating interface to Spark " << result.error();
  }
}

void
LinkMonitor::advertiseRedistAddrs() {
  if (std::chrono::steady_clock::now() < adjHoldUntilTimePoint_) {
    // Too early for advertising my own prefixes. Let timeout advertise it
    // and skip here.
    return;
  }

  std::vector<thrift::PrefixEntry> prefixes;

  // Add static prefixes
  for (auto const& prefix : staticPrefixes_) {
    auto prefixEntry = openr::thrift::PrefixEntry();
    prefixEntry.prefix = prefix;
    prefixEntry.type = thrift::PrefixType::LOOPBACK;
    prefixEntry.data = "";
    prefixEntry.forwardingType = forwardingTypeMpls_
        ? thrift::PrefixForwardingType::SR_MPLS
        : thrift::PrefixForwardingType::IP;
    prefixEntry.ephemeral = folly::none;
    prefixes.push_back(prefixEntry);
  }

  // Add redistribute addresses
  for (auto& kv : interfaces_) {
    auto& interface = kv.second;
    // Ignore in-active interfaces
    if (not interface.isActive()) {
      continue;
    }
    // Perform regex match
    if (not matchRegexSet(interface.getIfName(), redistRegexList_)) {
      continue;
    }
    // Add all prefixes of this interface
    for (auto& prefix : interface.getGlobalUnicastNetworks(enableV4_)) {
      prefix.forwardingType = forwardingTypeMpls_
          ? thrift::PrefixForwardingType::SR_MPLS
          : thrift::PrefixForwardingType::IP;
      prefixes.emplace_back(std::move(prefix));
    }
  }

  // Advertise via prefix manager client
  prefixManagerClient_->syncPrefixesByType(
      thrift::PrefixType::LOOPBACK, prefixes);
}

std::chrono::milliseconds
LinkMonitor::getRetryTimeOnUnstableInterfaces() {
  bool hasUnstableInterface = false;
  std::chrono::milliseconds minRemainMs = flapMaxBackoff_;
  for (auto& kv : interfaces_) {
    auto& interface = kv.second;
    if (interface.isActive()) {
      continue;
    }

    const auto& curRemainMs = interface.getBackoffDuration();
    if (curRemainMs.count() > 0) {
      VLOG(2) << "Interface " << interface.getIfName()
              << " is in backoff state for " << curRemainMs.count() << "ms";
      minRemainMs = std::min(minRemainMs, curRemainMs);
      hasUnstableInterface = true;
    }
  }

  return hasUnstableInterface ? minRemainMs : std::chrono::milliseconds(0);
}

InterfaceEntry* FOLLY_NULLABLE
LinkMonitor::getOrCreateInterfaceEntry(const std::string& ifName) {
  // Return null if ifName doesn't quality regex match criteria
  if (not checkIncludeExcludeRegex(
          ifName, includeRegexList_, excludeRegexList_) and
      not matchRegexSet(ifName, redistRegexList_)) {
    return nullptr;
  }

  // Return existing element if any
  auto it = interfaces_.find(ifName);
  if (it != interfaces_.end()) {
    return &(it->second);
  }

  // Create one and return it's reference
  auto res = interfaces_.emplace(
      ifName,
      InterfaceEntry(
          ifName,
          flapInitialBackoff_,
          flapMaxBackoff_,
          *advertiseIfaceAddrThrottled_,
          *advertiseIfaceAddrTimer_));

  return &(res.first->second);
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
  // Process received data and make updates in InterfaceEntry objects
  //
  for (const auto& link : links) {
    // Get interface entry
    auto interfaceEntry = getOrCreateInterfaceEntry(link.ifName);
    if (not interfaceEntry) {
      continue;
    }

    std::unordered_set<folly::CIDRNetwork> newNetworks;
    for (const auto& network : link.networks) {
      newNetworks.emplace(toIPNetwork(network, false /* no masking */));
    }
    const auto& oldNetworks = interfaceEntry->getNetworks();

    // Update link attributes
    const bool wasUp = interfaceEntry->isUp();
    interfaceEntry->updateAttrs(link.ifIndex, link.isUp, link.weight);
    logLinkEvent(
        interfaceEntry->getIfName(),
        wasUp,
        interfaceEntry->isUp(),
        interfaceEntry->getBackoffDuration());

    // Remove old addresses if they are not in new
    for (auto const& oldNetwork : oldNetworks) {
      if (newNetworks.count(oldNetwork) == 0) {
        interfaceEntry->updateAddr(oldNetwork, false);
      }
    }

    // Add new addresses if they are not in old
    for (auto const& newNetwork : newNetworks) {
      if (oldNetworks.count(newNetwork) == 0) {
        interfaceEntry->updateAddr(newNetwork, true);
      }
    }
  }

  return true;
}

folly::Expected<fbzmq::Message, fbzmq::Error>
LinkMonitor::processRequestMsg(fbzmq::Message&& request) {
  const auto maybeReq =
      request.readThriftObj<thrift::LinkMonitorRequest>(serializer_);
  if (maybeReq.hasError()) {
    LOG(ERROR) << "Error receiving LinkMonitorRequest: " << maybeReq.error();
    return folly::makeUnexpected(fbzmq::Error());
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
    advertiseAdjacencies(); // TODO: Use throttle here
    break;

  case thrift::LinkMonitorCommand::UNSET_OVERLOAD:
    if (not config_.isOverloaded) {
      // node is not in overloaded state, do nothing
      break;
    }
    LOG(INFO) << "Unsetting overload bit for node.";
    config_.isOverloaded = false;
    advertiseAdjacencies(); // TODO: Use throttle here
    break;

  case thrift::LinkMonitorCommand::SET_LINK_OVERLOAD:
    if (0 == interfaces_.count(req.interfaceName)) {
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
    advertiseAdjacencies(); // TODO: Use throttle here
    break;

  case thrift::LinkMonitorCommand::UNSET_LINK_OVERLOAD:
    if (config_.overloadedLinks.erase(req.interfaceName)) {
      LOG(INFO) << "Unsetting overload bit for interface " << req.interfaceName;
      advertiseAdjacencies(); // TODO: Use throttle here
    } else {
      LOG(WARNING) << "Got unset-overload-bit request for unknown link "
                   << req.interfaceName;
    }
    break;

  case thrift::LinkMonitorCommand::SET_LINK_METRIC:
    if (0 == interfaces_.count(req.interfaceName)) {
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
    advertiseAdjacencies(); // TODO: Use throttle here
    break;

  case thrift::LinkMonitorCommand::UNSET_LINK_METRIC:
    if (config_.linkMetricOverrides.erase(req.interfaceName)) {
      LOG(INFO) << "Removing metric override for interface "
                << req.interfaceName;
      advertiseAdjacencies(); // TODO: Use throttle here
    } else {
      LOG(WARNING) << "Got link-metric-unset request for unknown interface "
                   << req.interfaceName;
    }
    break;

  case thrift::LinkMonitorCommand::DUMP_LINKS: {
    VLOG(2) << "Dump Links requested, replying with " << interfaces_.size()
            << " links";

    // reply with the dump of known interfaces and their states
    thrift::DumpLinksReply reply;
    reply.thisNodeName = nodeId_;
    reply.isOverloaded = config_.isOverloaded;

    // Fill interface details
    for (auto& kv : interfaces_) {
      auto& ifName = kv.first;
      auto& interface = kv.second;
      auto ifDetails = thrift::InterfaceDetails(
          apache::thrift::FRAGILE,
          interface.getInterfaceInfo(),
          config_.overloadedLinks.count(ifName) > 0,
          0 /* custom metric value */,
          0 /* link flap back off time */);

      // Add metric override if any
      folly::Optional<int32_t> maybeMetric;
      if (config_.linkMetricOverrides.count(ifName) > 0) {
        maybeMetric.assign(config_.linkMetricOverrides.at(ifName));
      }
      ifDetails.metricOverride = maybeMetric;

      // Add link-backoff
      auto backoffMs = interface.getBackoffDuration();
      if (backoffMs.count() != 0) {
        ifDetails.linkFlapBackOffMs = backoffMs.count();
      } else {
        ifDetails.linkFlapBackOffMs = folly::none;
      }

      reply.interfaceDetails.emplace(ifName, std::move(ifDetails));
    }

    return fbzmq::Message::fromThriftObj(reply, serializer_);
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

    if (adjacencies_.count(
            std::make_pair(req.adjNodeName.value(), req.interfaceName))) {
      LOG(INFO) << "Overriding metric for adjacency " << req.adjNodeName.value()
                << " " << req.interfaceName << " to " << req.overrideMetric;
      advertiseAdjacencies(); // TODO: Use throttle here

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

      if (adjacencies_.count(
              std::make_pair(req.adjNodeName.value(), req.interfaceName))) {
        advertiseAdjacencies(); // TODO: Use throttle here
      }
    } else {
      LOG(WARNING) << "Got adj-metric-unset request for unknown adjacency"
                   << req.adjNodeName.value() << " " << req.interfaceName;
    }
    break;
  }

  case thrift::LinkMonitorCommand::GET_VERSION: {
    thrift::OpenrVersions openrVersion(
        apache::thrift::FRAGILE,
        Constants::kOpenrVersion,
        Constants::kOpenrSupportedVersion);

    return fbzmq::Message::fromThriftObj(openrVersion, serializer_);
  }

  case thrift::LinkMonitorCommand::GET_BUILD_INFO: {
    auto buildInfo = getBuildInfoThrift();
    return fbzmq::Message::fromThriftObj(buildInfo, serializer_);
  }

  default:
    LOG(ERROR) << "Link Monitor received unknown command: "
               << static_cast<int>(req.cmd);
    return folly::makeUnexpected(fbzmq::Error());
  }
  return fbzmq::Message::from(Constants::kSuccessResponse.toString());
}

void
LinkMonitor::submitCounters() {
  VLOG(3) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  counters["link_monitor.adjacencies"] = adjacencies_.size();
  counters["link_monitor.zmq_event_queue_size"] = getEventQueueSize();
  for (const auto& kv : adjacencies_) {
    auto& adj = kv.second.adjacency;
    counters["link_monitor.metric." + adj.otherNodeName] = adj.metric;
  }

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

void
LinkMonitor::logNeighborEvent(
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

void
LinkMonitor::logLinkEvent(
    const std::string& iface,
    bool wasUp,
    bool isUp,
    std::chrono::milliseconds backoffTime) {
  // Do not log if no state transition
  if (wasUp == isUp) {
    return;
  }

  fbzmq::LogSample sample{};
  const std::string event = isUp ? "UP" : "DOWN";

  sample.addString("event", folly::sformat("IFACE_{}", event));
  sample.addString("entity", "LinkMonitor");
  sample.addString("node_name", nodeId_);
  sample.addString("interface", iface);
  sample.addInt("backoff_ms", backoffTime.count());

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));

  syslog(
      LOG_NOTICE,
      "%s",
      folly::sformat(
          "Interface {} is {} and has backoff of {}ms",
          iface,
          event,
          backoffTime.count())
          .c_str());
}

void
LinkMonitor::logPeerEvent(
    const std::string& event,
    const std::string& peerName,
    const thrift::PeerSpec& peerSpec) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "LinkMonitor");
  sample.addString("node_name", nodeId_);
  sample.addString("peer_name", peerName);
  sample.addString("pub_url", peerSpec.pubUrl);
  sample.addString("cmd_url", peerSpec.cmdUrl);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

} // namespace openr
