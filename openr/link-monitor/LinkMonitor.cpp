/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LinkMonitor.h"

#include <functional>

#include <fb303/ServiceData.h>
#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/futures/Future.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/spark/Spark.h>

namespace fb303 = facebook::fb303;

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
printLinkMonitorState(openr::thrift::LinkMonitorState const& state) {
  VLOG(1) << "LinkMonitor state .... ";
  VLOG(1) << "\tnodeLabel: " << state.nodeLabel;
  VLOG(1) << "\tisOverloaded: " << (state.isOverloaded ? "true" : "false");
  if (not state.overloadedLinks.empty()) {
    VLOG(1) << "\toverloadedLinks: " << folly::join(",", state.overloadedLinks);
  }
  if (not state.linkMetricOverrides.empty()) {
    VLOG(1) << "\tlinkMetricOverrides: ";
    for (auto const& kv : state.linkMetricOverrides) {
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
    fbzmq::Context& zmqContext,
    std::shared_ptr<const Config> config,
    fbnl::NetlinkProtocolSocket* nlSock,
    KvStore* kvStore,
    PersistentStore* configStore,
    bool enablePerfMeasurement,
    messaging::ReplicateQueue<thrift::InterfaceDatabase>& intfUpdatesQueue,
    messaging::ReplicateQueue<thrift::PrefixUpdateRequest>& prefixUpdatesQueue,
    messaging::ReplicateQueue<thrift::PeerUpdateRequest>& peerUpdatesQueue,
    messaging::RQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue,
    messaging::RQueue<fbnl::NetlinkEvent> netlinkEventsQueue,
    MonitorSubmitUrl const& monitorSubmitUrl,
    bool assumeDrained,
    bool overrideDrainState,
    std::chrono::seconds adjHoldTime)
    : nodeId_(config->getNodeName()),
      enablePerfMeasurement_(enablePerfMeasurement),
      enableV4_(config->isV4Enabled()),
      enableSegmentRouting_(config->isSegmentRoutingEnabled()),
      prefixForwardingType_(config->getConfig().prefix_forwarding_type),
      prefixForwardingAlgorithm_(
          config->getConfig().prefix_forwarding_algorithm),
      useRttMetric_(config->getLinkMonitorConfig().use_rtt_metric),
      linkflapInitBackoff_(std::chrono::milliseconds(
          config->getLinkMonitorConfig().linkflap_initial_backoff_ms)),
      linkflapMaxBackoff_(std::chrono::milliseconds(
          config->getLinkMonitorConfig().linkflap_max_backoff_ms)),
      ttlKeyInKvStore_(config->getKvStoreKeyTtl()),
      includeItfRegexes_(config->getIncludeItfRegexes()),
      excludeItfRegexes_(config->getExcludeItfRegexes()),
      redistributeItfRegexes_(config->getRedistributeItfRegexes()),
      areas_(config->getAreaIds()),
      interfaceUpdatesQueue_(intfUpdatesQueue),
      prefixUpdatesQueue_(prefixUpdatesQueue),
      peerUpdatesQueue_(peerUpdatesQueue),
      expBackoff_(Constants::kInitialBackoff, Constants::kMaxBackoff),
      configStore_(configStore),
      nlSock_(nlSock) {
  // Check non-empty module ptr
  CHECK(kvStore);
  CHECK(configStore_);
  CHECK(nlSock_);

  // Schedule callback to advertise the initial set of adjacencies and prefixes
  adjHoldTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    LOG(INFO) << "Hold time expired. Advertising adjacencies and addresses";
    // Advertise adjacencies and addresses after hold-timeout
    advertiseAdjacencies();
    advertiseRedistAddrs();
  });

  // Create throttled adjacency advertiser
  advertiseAdjacenciesThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kLinkThrottleTimeout, [this]() noexcept {
        // will advertise to all areas but will not trigger a adj key update
        // if nothing changed.
        advertiseAdjacencies();
      });

  // Create throttled interfaces and addresses advertiser
  advertiseIfaceAddrThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kLinkThrottleTimeout, [this]() noexcept {
        advertiseIfaceAddr();
      });
  // Create timer. Timer is used for immediate or delayed executions.
  advertiseIfaceAddrTimer_ = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { advertiseIfaceAddr(); });

  // [TO BE DEPRECATED]
  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Create config-store client
  LOG(INFO) << "Loading link-monitor state";
  auto state =
      configStore_->loadThriftObj<thrift::LinkMonitorState>(kConfigKey).get();
  if (state.hasValue()) {
    LOG(INFO) << "Loaded link-monitor state from disk.";
    state_ = state.value();
    printLinkMonitorState(state_);
  } else {
    // no persistent store found, use assumeDrained
    state_.isOverloaded = assumeDrained;
    LOG(WARNING) << folly::sformat(
        "Failed to load link-monitor state from disk. Setting node as {}",
        assumeDrained ? "DRAINED" : "UNDRAINED");
  }
  // overrideDrainState provided, use assumeDrained
  if (overrideDrainState) {
    state_.isOverloaded = assumeDrained;
    LOG(WARNING) << folly::sformat(
        "FLAGS_override_drain_state == true, setting drain state based on FLAGS_assume_drained to {}",
        assumeDrained ? "DRAINED" : "UNDRAINED");
  }

  //  Create KvStore client
  kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
      this, nodeId_, kvStore, std::nullopt /* persist key timer */);

  if (enableSegmentRouting_) {
    // create range allocator to get unique node labels
    for (const auto& area : areas_) {
      rangeAllocator_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(area),
          std::forward_as_tuple(
              nodeId_,
              Constants::kNodeLabelRangePrefix.toString(),
              kvStoreClient_.get(),
              [&](std::optional<int32_t> newVal) noexcept {
                state_.nodeLabel = newVal ? newVal.value() : 0;
                advertiseAdjacencies();
              }, /* callback */
              std::chrono::milliseconds(100), /* minBackoffDur */
              std::chrono::seconds(2), /* maxBackoffDur */
              false /* override owner */,
              nullptr, /* checkValueInUseCb */
              Constants::kRangeAllocTtl,
              area));

      // Delay range allocation until we have formed all of our adjcencies
      auto startAllocTimer =
          folly::AsyncTimeout::make(*getEvb(), [this, area]() noexcept {
            std::optional<int32_t> initValue;
            if (state_.nodeLabel != 0) {
              initValue = state_.nodeLabel;
            }
            rangeAllocator_.at(area).startAllocator(
                Constants::kSrGlobalRange, initValue);
          });
      startAllocTimer->scheduleTimeout(adjHoldTime);
      startAllocationTimers_.emplace_back(std::move(startAllocTimer));
    }
  }

  // start initial dump timer
  adjHoldTimer_->scheduleTimeout(adjHoldTime);

  // Add fiber to process the neighbor events
  addFiberTask([q = std::move(neighborUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        LOG(INFO) << "Terminating neighbor update processing fiber";
        break;
      }
      processNeighborEvent(std::move(maybeEvent).value());
    }
  });

  // Add fiber to process the LINK/ADDR events from platform
  addFiberTask([q = std::move(netlinkEventsQueue), this]() mutable noexcept {
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        LOG(INFO) << "Terminating neighbor update processing fiber";
        break;
      }
      processNetlinkEvent(std::move(maybeEvent).value());
    }
  });

  // Schedule periodic timer for InterfaceDb re-sync from Netlink Platform
  interfaceDbSyncTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    auto success = syncInterfaces();
    if (success) {
      VLOG(2) << "InterfaceDb Sync is successful";
      expBackoff_.reportSuccess();
      interfaceDbSyncTimer_->scheduleTimeout(Constants::kPlatformSyncInterval);
    } else {
      fb303::fbData->addStatValue(
          "link_monitor.thrift.failure.getAllLinks", 1, fb303::SUM);

      if (ifIndexToName_.empty()) {
        // initial sync failed, immediately file re-sync
        // instead of applying exponetial backoff
        LOG(ERROR) << "Initial interfaceDb sync failed, re-sync immediately";

        interfaceDbSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
      } else {
        // Apply exponential backoff and schedule next run
        expBackoff_.reportError();
        interfaceDbSyncTimer_->scheduleTimeout(
            expBackoff_.getTimeRemainingUntilRetry());
        LOG(ERROR)
            << "InterfaceDb Sync failed, apply exponential backoff and retry in "
            << expBackoff_.getTimeRemainingUntilRetry().count() << " ms";
      }
    }
  });

  // schedule immediate with small timeout
  interfaceDbSyncTimer_->scheduleTimeout(std::chrono::milliseconds(100));

  // Initialize stats keys
  fb303::fbData->addStatExportType("link_monitor.neighbor_up", fb303::SUM);
  fb303::fbData->addStatExportType("link_monitor.neighbor_down", fb303::SUM);
  fb303::fbData->addStatExportType(
      "link_monitor.advertise_adjacencies", fb303::SUM);
  fb303::fbData->addStatExportType("link_monitor.advertise_links", fb303::SUM);
}

void
LinkMonitor::stop() {
  // Stop KvStoreClient first
  kvStoreClient_->stop();

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

void
LinkMonitor::neighborUpEvent(const thrift::SparkNeighborEvent& event) {
  const auto& neighborAddrV4 = event.neighbor.transportAddressV4;
  const auto& neighborAddrV6 = event.neighbor.transportAddressV6;
  const std::string& ifName = event.ifName;
  const std::string& remoteNodeName = event.neighbor.nodeName;
  const std::string& remoteIfName = event.neighbor.ifName;
  const std::string& area = event.area;
  const auto adjId = std::make_pair(remoteNodeName, ifName);
  const int32_t kvStoreCmdPort = event.neighbor.kvStoreCmdPort;
  const int32_t openrCtrlThriftPort = event.neighbor.openrCtrlThriftPort;
  auto rttMetric = getRttMetric(event.rttUs);
  auto now = std::chrono::system_clock::now();
  // current unixtime in s
  int64_t timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  int64_t weight = 1;
  if (interfaces_.count(ifName)) {
    weight = interfaces_.at(ifName).getWeight();
  }

  thrift::Adjacency newAdj = createThriftAdjacency(
      remoteNodeName /* otherNodeName */,
      ifName,
      toString(neighborAddrV6) /* nextHopV6 */,
      toString(neighborAddrV4) /* nextHopV4 */,
      useRttMetric_ ? rttMetric : 1 /* metric */,
      enableSegmentRouting_ ? event.label : 0 /* adjacency-label */,
      false /* overload bit */,
      useRttMetric_ ? event.rttUs : 0,
      timestamp,
      weight,
      remoteIfName /* otherIfName */);

  SYSLOG(INFO)
      << "Neighbor " << remoteNodeName << " is up on interface " << ifName
      << ". Remote Interface: " << remoteIfName << ", metric: " << newAdj.metric
      << ", rttUs: " << event.rttUs << ", addrV4: " << toString(neighborAddrV4)
      << ", addrV6: " << toString(neighborAddrV6) << ", area: " << area;
  fb303::fbData->addStatValue("link_monitor.neighbor_up", 1, fb303::SUM);

  std::string repUrl{""};
  std::string peerAddr{""};
  if (!mockMode_) {
    // peer address used for KvStore external sync over ZMQ
    repUrl = folly::sformat(
        "tcp://[{}%{}]:{}", toString(neighborAddrV6), ifName, kvStoreCmdPort);
    // peer address used for KvStore external sync over thrift
    peerAddr = folly::sformat("{}%{}", toString(neighborAddrV6), ifName);
  } else {
    // use inproc address
    repUrl = folly::sformat("inproc://{}-kvstore-cmd-global", remoteNodeName);
    // TODO: address value of peerAddr under system test environment
    peerAddr = folly::sformat("::1%{}", ifName);
  }

  CHECK(not repUrl.empty()) << "Got empty repUrl";
  CHECK(not peerAddr.empty()) << "Got empty peerAddr";

  // two cases upon this event:
  // 1) the min interface changes: the previous min interface's connection will
  // be overridden by KvStoreClientInternal, thus no need to explicitly remove
  // it 2) does not change: the existing connection to a neighbor is retained
  thrift::PeerSpec peerSpec;
  peerSpec.cmdUrl = repUrl;
  peerSpec.peerAddr = peerAddr;
  peerSpec.ctrlPort = openrCtrlThriftPort;
  peerSpec.supportFloodOptimization = event.supportFloodOptimization;
  adjacencies_[adjId] =
      AdjacencyValue(peerSpec, std::move(newAdj), false, area);

  // Advertise KvStore peers immediately
  advertiseKvStorePeers(area, {{remoteNodeName, peerSpec}});

  // Advertise new adjancies in a throttled fashion
  advertiseAdjacenciesThrottled_->operator()();
}

void
LinkMonitor::neighborDownEvent(const thrift::SparkNeighborEvent& event) {
  const auto& remoteNodeName = event.neighbor.nodeName;
  const auto& ifName = event.ifName;
  const auto& area = event.area;
  const auto adjId = std::make_pair(remoteNodeName, ifName);

  SYSLOG(INFO) << "Neighbor " << remoteNodeName << " is down on interface "
               << ifName;
  fb303::fbData->addStatValue("link_monitor.neighbor_down", 1, fb303::SUM);

  auto adjValueIt = adjacencies_.find(adjId);
  if (adjValueIt != adjacencies_.end()) {
    // remove such adjacencies
    adjacencies_.erase(adjValueIt);
  }
  // advertise both peers and adjacencies
  advertiseKvStorePeers(area);
  advertiseAdjacencies(area);
}

void
LinkMonitor::neighborRestartingEvent(const thrift::SparkNeighborEvent& event) {
  const auto& remoteNodeName = event.neighbor.nodeName;
  const auto& ifName = event.ifName;
  const auto& area = event.area;
  const auto adjId = std::make_pair(remoteNodeName, ifName);

  SYSLOG(INFO) << "Neighbor " << remoteNodeName
               << " is restarting on interface " << ifName;
  fb303::fbData->addStatValue(
      "link_monitor.neighbor_restarting", 1, fb303::SUM);

  // update adjacencies_ restarting-bit and advertise peers
  auto adjValueIt = adjacencies_.find(adjId);
  if (adjValueIt != adjacencies_.end()) {
    adjValueIt->second.isRestarting = true;
  }
  advertiseKvStorePeers(area);
}

void
LinkMonitor::neighborRttChangeEvent(const thrift::SparkNeighborEvent& event) {
  const auto& remoteNodeName = event.neighbor.nodeName;
  const auto& ifName = event.ifName;
  int32_t newRttMetric = getRttMetric(event.rttUs);

  VLOG(1) << "Metric value changed for neighbor " << remoteNodeName << " to "
          << newRttMetric;

  auto it = adjacencies_.find({remoteNodeName, ifName});
  if (it != adjacencies_.end()) {
    auto& adj = it->second.adjacency;
    adj.metric = newRttMetric;
    adj.rtt = event.rttUs;
    advertiseAdjacenciesThrottled_->operator()();
  }
}

std::unordered_map<std::string, thrift::PeerSpec>
LinkMonitor::getPeersFromAdjacencies(
    const std::unordered_map<AdjacencyKey, AdjacencyValue>& adjacencies,
    const std::string& area) {
  std::unordered_map<std::string, std::string> neighborToIface;
  for (const auto& adjKv : adjacencies) {
    if (adjKv.second.area != area || adjKv.second.isRestarting) {
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
LinkMonitor::advertiseKvStorePeers(
    const std::string& area,
    const std::unordered_map<std::string, thrift::PeerSpec>& upPeers) {
  // Prepare peer update request
  thrift::PeerUpdateRequest req;
  req.area = area;

  // Get old and new peer list. Also update local state
  const auto oldPeers = std::move(peers_[area]);
  peers_[area] = getPeersFromAdjacencies(adjacencies_, area);
  const auto& newPeers = peers_[area];

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
    thrift::PeerDelParams params;
    params.peerNames = std::move(toDelPeers);
    req.peerDelParams_ref() = std::move(params);
  }

  // Get list of peers to add
  std::unordered_map<std::string, thrift::PeerSpec> toAddPeers;
  for (const auto& newKv : newPeers) {
    const auto& nodeName = newKv.first;
    // send out peer-add to kvstore if
    // 1. it's a new peer (not exist in old-peers)
    // 2. old-peer but peer-spec changed (e.g parallel link case)
    if (oldPeers.find(nodeName) == oldPeers.end() or
        oldPeers.at(nodeName) != newKv.second) {
      toAddPeers.emplace(nodeName, newKv.second);
      logPeerEvent("ADD_PEER", newKv.first, newKv.second);
    }
  }

  for (const auto& upPeer : upPeers) {
    const auto& name = upPeer.first;
    const auto& spec = upPeer.second;
    // upPeer MUST already be in current state peers_
    CHECK(peers_.at(area).count(name));

    if (toAddPeers.count(name)) {
      // already added, skip it
      continue;
    }
    if (spec != peers_.at(area).at(name)) {
      // spec does not match, skip it
      continue;
    }
    toAddPeers.emplace(name, spec);
  }

  // Add new peers
  if (toAddPeers.size() > 0) {
    thrift::PeerAddParams params;
    params.peers = std::move(toAddPeers);
    req.peerAddParams_ref() = std::move(params);
  }

  if (req.peerDelParams_ref().has_value() ||
      req.peerAddParams_ref().has_value()) {
    peerUpdatesQueue_.push(std::move(req));
  }
}

void
LinkMonitor::advertiseKvStorePeers(
    const std::unordered_map<std::string, thrift::PeerSpec>& upPeers) {
  // Get old and new peer list. Also update local state
  for (const auto& area : areas_) {
    advertiseKvStorePeers(area, upPeers);
  }
}

void
LinkMonitor::advertiseAdjacencies(const std::string& area) {
  if (adjHoldTimer_->isScheduled()) {
    return;
  }

  // Cancel throttle timeout if scheduled
  if (advertiseAdjacenciesThrottled_->isActive()) {
    advertiseAdjacenciesThrottled_->cancel();
  }

  auto adjDb = thrift::AdjacencyDatabase();
  adjDb.thisNodeName = nodeId_;
  adjDb.isOverloaded = state_.isOverloaded;
  adjDb.nodeLabel = enableSegmentRouting_ ? state_.nodeLabel : 0;
  adjDb.area = area;
  for (const auto& adjKv : adjacencies_) {
    // 'second.second' is the adj object for this peer
    // must match the area
    if (adjKv.second.area != area) {
      continue;
    }
    // NOTE: copy on purpose
    auto adj = folly::copy(adjKv.second.adjacency);

    // Set link overload bit
    adj.isOverloaded = state_.overloadedLinks.count(adj.ifName) > 0;

    // Override metric with link metric if it exists
    adj.metric =
        folly::get_default(state_.linkMetricOverrides, adj.ifName, adj.metric);

    // Override metric with adj metric if it exists
    thrift::AdjKey adjKey;
    adjKey.nodeName = adj.otherNodeName;
    adjKey.ifName = adj.ifName;
    adj.metric =
        folly::get_default(state_.adjMetricOverrides, adjKey, adj.metric);

    adjDb.adjacencies.emplace_back(std::move(adj));
  }

  // Add perf information if enabled
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "ADJ_DB_UPDATED");
    adjDb.perfEvents_ref() = perfEvents;
  } else {
    DCHECK(!adjDb.perfEvents_ref().has_value());
  }

  LOG(INFO) << "Updating adjacency database in KvStore with "
            << adjDb.adjacencies.size() << " entries in area: " << area;
  const auto keyName = Constants::kAdjDbMarker.toString() + nodeId_;
  std::string adjDbStr = fbzmq::util::writeThriftObjStr(adjDb, serializer_);
  kvStoreClient_->persistKey(keyName, adjDbStr, ttlKeyInKvStore_, area);
  fb303::fbData->addStatValue(
      "link_monitor.advertise_adjacencies", 1, fb303::SUM);

  // Config is most likely to have changed. Update it in `ConfigStore`
  configStore_->storeThriftObj(kConfigKey, state_); // not awaiting on result

  // Update some flat counters
  fb303::fbData->setCounter("link_monitor.adjacencies", adjacencies_.size());
  for (const auto& kv : adjacencies_) {
    auto& adj = kv.second.adjacency;
    fb303::fbData->setCounter(
        "link_monitor.metric." + adj.otherNodeName, adj.metric);
  }
}
void
LinkMonitor::advertiseAdjacencies() {
  // advertise to all areas. Once area configuration per link is implemented
  // then adjacencies can be advertised to a specific area
  for (const auto& area : areas_) {
    // Update KvStore
    advertiseAdjacencies(area);
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
  fb303::fbData->addStatValue("link_monitor.advertise_links", 1, fb303::SUM);

  // Create interface database
  thrift::InterfaceDatabase ifDb;
  ifDb.thisNodeName = nodeId_;
  for (auto& kv : interfaces_) {
    auto& ifName = kv.first;
    auto& interface = kv.second;
    // Perform regex match
    if (not checkIncludeExcludeRegex(
            ifName, includeItfRegexes_, excludeItfRegexes_)) {
      continue;
    }
    // Get interface info and override active status
    auto interfaceInfo = interface.getInterfaceInfo();
    interfaceInfo.isUp = interface.isActive();
    ifDb.interfaces.emplace(ifName, std::move(interfaceInfo));
  }

  // publish new interface database to other modules (Fib & Spark)
  interfaceUpdatesQueue_.push(std::move(ifDb));
}

void
LinkMonitor::advertiseRedistAddrs() {
  if (adjHoldTimer_->isScheduled()) {
    return;
  }
  std::vector<thrift::PrefixEntry> prefixes;

  // Add redistribute addresses
  for (auto& kv : interfaces_) {
    auto& interface = kv.second;
    // Ignore in-active interfaces
    if (not interface.isActive()) {
      continue;
    }
    // Perform regex match
    if (not matchRegexSet(interface.getIfName(), redistributeItfRegexes_)) {
      continue;
    }
    // Add all prefixes of this interface
    for (auto& prefix : interface.getGlobalUnicastNetworks(enableV4_)) {
      prefix.forwardingType = prefixForwardingType_;
      prefix.forwardingAlgorithm = prefixForwardingAlgorithm_;
      // Tags
      {
        auto& tags = prefix.tags_ref().value();
        tags.emplace("INTERFACE_SUBNET");
        tags.emplace(folly::sformat("{}:{}", nodeId_, interface.getIfName()));
      }
      // Metrics
      {
        auto& metrics = prefix.metrics_ref().value();
        metrics.path_preference_ref() = Constants::kDefaultPathPreference;
        metrics.source_preference_ref() = Constants::kDefaultSourcePreference;
      }
      prefixes.emplace_back(std::move(prefix));
    }
  }

  LOG_IF(INFO, prefixes.empty()) << "Advertising empty LOOPBACK addresses.";
  // Advertise via prefix manager client
  thrift::PrefixUpdateRequest request;
  request.cmd = thrift::PrefixUpdateCommand::SYNC_PREFIXES_BY_TYPE;
  request.type_ref() = openr::thrift::PrefixType::LOOPBACK;
  request.prefixes = std::move(prefixes);
  // publish LOOPBACK prefixes to prefix manager
  prefixUpdatesQueue_.push(std::move(request));
}

std::chrono::milliseconds
LinkMonitor::getRetryTimeOnUnstableInterfaces() {
  std::chrono::milliseconds minRemainMs{0};
  for (auto& kv : interfaces_) {
    auto& interface = kv.second;
    if (interface.isActive()) {
      continue;
    }

    const auto& curRemainMs = interface.getBackoffDuration();
    if (curRemainMs.count() > 0) {
      VLOG(2) << "Interface " << interface.getIfName()
              << " is in backoff state for " << curRemainMs.count() << "ms";
      minRemainMs = std::min(linkflapMaxBackoff_, curRemainMs);
    }
  }

  return minRemainMs;
}

InterfaceEntry* FOLLY_NULLABLE
LinkMonitor::getOrCreateInterfaceEntry(const std::string& ifName) {
  // Return null if ifName doesn't quality regex match criteria
  if (not checkIncludeExcludeRegex(
          ifName, includeItfRegexes_, excludeItfRegexes_) and
      not matchRegexSet(ifName, redistributeItfRegexes_)) {
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
          linkflapInitBackoff_,
          linkflapMaxBackoff_,
          *advertiseIfaceAddrThrottled_,
          *advertiseIfaceAddrTimer_));

  return &(res.first->second);
}

bool
LinkMonitor::syncInterfaces() {
  VLOG(1) << "Syncing Interface DB from Netlink Platform";

  // Retrieve latest link snapshot from NetlinkProtocolSocket
  std::vector<thrift::Link> links;
  try {
    links = *(getAllLinks().get());
  } catch (const std::exception& e) {
    LOG(ERROR) << "Failed to sync linkDb from NetlinkProtocolSocket. Error: "
               << folly::exceptionStr(e);
    return false;
  }

  // Make updates in InterfaceEntry objects
  for (const auto& link : links) {
    // update cache of ifIndex -> ifName mapping
    //  1) if ifIndex exists, override it with new ifName;
    //  2) if ifIndex does NOT exist, cache the ifName;
    ifIndexToName_[*link.ifIndex_ref()] = *link.ifName_ref();

    // Get interface entry
    auto interfaceEntry = getOrCreateInterfaceEntry(link.ifName);
    if (not interfaceEntry) {
      continue;
    }

    const std::unordered_set<folly::CIDRNetwork> oldNetworks =
        interfaceEntry->getNetworks(); // NOTE: Copy intended
    std::unordered_set<folly::CIDRNetwork> newNetworks;
    for (const auto& network : link.networks) {
      newNetworks.emplace(toIPNetwork(network, false /* no masking */));
    }

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

void
LinkMonitor::processNetlinkEvent(fbnl::NetlinkEvent&& event) {
  if (auto* link = std::get_if<fbnl::Link>(&event)) {
    VLOG(3) << "Received Link Event from NetlinkProtocolSocket...";

    auto ifName = link->getLinkName();
    auto ifIndex = link->getIfIndex();
    auto isUp = link->isUp();

    // Cache interface index name mapping
    // ATTN: will create new ifIndex -> ifName mapping if it is unknown link
    //       `[]` operator is used in purpose
    ifIndexToName_[ifIndex] = ifName;

    auto interfaceEntry = getOrCreateInterfaceEntry(ifName);
    if (interfaceEntry) {
      const bool wasUp = interfaceEntry->isUp();
      interfaceEntry->updateAttrs(ifIndex, isUp, Constants::kDefaultAdjWeight);
      logLinkEvent(
          interfaceEntry->getIfName(),
          wasUp,
          interfaceEntry->isUp(),
          interfaceEntry->getBackoffDuration());
    }
  } else if (auto* addr = std::get_if<fbnl::IfAddress>(&event)) {
    VLOG(3) << "Received Address Event from NetlinkProtocolSocket...";

    auto ifIndex = addr->getIfIndex();
    auto prefix = addr->getPrefix(); // std::optional<folly::CIDRNetwork>
    auto isValid = addr->isValid();

    // Check for interface name
    auto it = ifIndexToName_.find(ifIndex);
    if (it == ifIndexToName_.end()) {
      LOG(ERROR) << "Address event for unknown iface index: " << ifIndex;
      return;
    }

    // Cached ifIndex -> ifName mapping
    auto interfaceEntry = getOrCreateInterfaceEntry(it->second);
    if (interfaceEntry) {
      interfaceEntry->updateAddr(prefix.value(), isValid);
    }
  }
}

void
LinkMonitor::processNeighborEvent(thrift::SparkNeighborEvent&& event) {
  auto neighborAddrV4 = event.neighbor.transportAddressV4;
  auto neighborAddrV6 = event.neighbor.transportAddressV6;

  VLOG(1)
      << "Received neighbor event for " << event.neighbor.nodeName << " from "
      << event.neighbor.ifName << " at " << event.ifName << " with addrs "
      << toString(neighborAddrV6) << " and "
      << (enableV4_ ? toString(neighborAddrV4) : "") << " Area:" << event.area
      << " Event Type: " << apache::thrift::util::enumNameSafe(event.eventType);

  switch (event.eventType) {
  case thrift::SparkNeighborEventType::NEIGHBOR_UP:
  case thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED: {
    logNeighborEvent(event);
    neighborUpEvent(event);
    break;
  }
  case thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING: {
    logNeighborEvent(event);
    neighborRestartingEvent(event);
    break;
  }
  case thrift::SparkNeighborEventType::NEIGHBOR_DOWN: {
    logNeighborEvent(event);
    neighborDownEvent(event);
    break;
  }
  case thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE: {
    if (!useRttMetric_) {
      break;
    }
    logNeighborEvent(event);
    neighborRttChangeEvent(event);
    break;
  }
  default:
    LOG(ERROR) << "Unknown event type " << (int32_t)event.eventType;
  }
}

// NOTE: add commands which set/unset overload bit or metric values will
// immediately advertise new adjacencies into the KvStore.
folly::SemiFuture<folly::Unit>
LinkMonitor::setNodeOverload(bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), isOverloaded]() mutable {
    std::string cmd =
        isOverloaded ? "SET_NODE_OVERLOAD" : "UNSET_NODE_OVERLOAD";
    if (state_.isOverloaded == isOverloaded) {
      LOG(INFO) << "Skip cmd: [" << cmd << "]. Node already in target state: ["
                << (isOverloaded ? "OVERLOADED" : "NOT OVERLOADED") << "]";
    } else {
      state_.isOverloaded = isOverloaded;
      SYSLOG(INFO) << (isOverloaded ? "Setting" : "Unsetting")
                   << " overload bit for node";
      advertiseAdjacencies();
    }
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setInterfaceOverload(
    std::string interfaceName, bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), interfaceName, isOverloaded]() mutable {
        std::string cmd =
            isOverloaded ? "SET_LINK_OVERLOAD" : "UNSET_LINK_OVERLOAD";
        if (0 == interfaces_.count(interfaceName)) {
          LOG(ERROR) << "Skip cmd: [" << cmd
                     << "] due to unknown interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (isOverloaded && state_.overloadedLinks.count(interfaceName)) {
          LOG(INFO) << "Skip cmd: [" << cmd << "]. Interface: " << interfaceName
                    << " is already overloaded";
          p.setValue();
          return;
        }

        if (!isOverloaded && !state_.overloadedLinks.count(interfaceName)) {
          LOG(INFO) << "Skip cmd: [" << cmd << "]. Interface: " << interfaceName
                    << " is currently NOT overloaded";
          p.setValue();
          return;
        }

        if (isOverloaded) {
          state_.overloadedLinks.insert(interfaceName);
          SYSLOG(INFO) << "Setting overload bit for interface "
                       << interfaceName;
        } else {
          state_.overloadedLinks.erase(interfaceName);
          SYSLOG(INFO) << "Unsetting overload bit for interface "
                       << interfaceName;
        }
        advertiseAdjacenciesThrottled_->operator()();
        p.setValue();
      });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setLinkMetric(
    std::string interfaceName, std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), interfaceName, overrideMetric]() mutable {
        std::string cmd = overrideMetric.has_value() ? "SET_LINK_METRIC"
                                                     : "UNSET_LINK_METRIC";
        if (0 == interfaces_.count(interfaceName)) {
          LOG(ERROR) << "Skip cmd: [" << cmd
                     << "] due to unknown interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value() &&
            state_.linkMetricOverrides.count(interfaceName) &&
            state_.linkMetricOverrides[interfaceName] ==
                overrideMetric.value()) {
          LOG(INFO) << "Skip cmd: " << cmd
                    << ". Overridden metric: " << overrideMetric.value()
                    << " already set for interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (!overrideMetric.has_value() &&
            !state_.linkMetricOverrides.count(interfaceName)) {
          LOG(INFO) << "Skip cmd: " << cmd
                    << ". No overridden metric found for interface: "
                    << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value()) {
          state_.linkMetricOverrides[interfaceName] = overrideMetric.value();
          SYSLOG(INFO) << "Overriding metric for interface " << interfaceName
                       << " to " << overrideMetric.value();
        } else {
          state_.linkMetricOverrides.erase(interfaceName);
          SYSLOG(INFO) << "Removing metric override for interface "
                       << interfaceName;
        }
        advertiseAdjacenciesThrottled_->operator()();
        p.setValue();
      });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setAdjacencyMetric(
    std::string interfaceName,
    std::string adjNodeName,
    std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceName,
                        adjNodeName,
                        overrideMetric]() mutable {
    std::string cmd = overrideMetric.has_value() ? "SET_ADJACENCY_METRIC"
                                                 : "UNSET_ADJACENCY_METRIC";
    thrift::AdjKey adjKey;
    adjKey.ifName = interfaceName;
    adjKey.nodeName = adjNodeName;

    // Invalid adj encountered. Ignore
    if (!adjacencies_.count(std::make_pair(adjNodeName, interfaceName))) {
      LOG(ERROR) << "Skip cmd: [" << cmd << "] due to unknown adj: ["
                 << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value() && state_.adjMetricOverrides.count(adjKey) &&
        state_.adjMetricOverrides[adjKey] == overrideMetric.value()) {
      LOG(INFO) << "Skip cmd: " << cmd
                << ". Overridden metric: " << overrideMetric.value()
                << " already set for: [" << adjNodeName << ":" << interfaceName
                << "]";
      p.setValue();
      return;
    }

    if (!overrideMetric.has_value() &&
        !state_.adjMetricOverrides.count(adjKey)) {
      LOG(INFO) << "Skip cmd: " << cmd << ". No overridden metric found for: ["
                << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value()) {
      state_.adjMetricOverrides[adjKey] = overrideMetric.value();
      SYSLOG(INFO) << "Overriding metric for adjacency: [" << adjNodeName << ":"
                   << interfaceName << "] to " << overrideMetric.value();
    } else {
      state_.adjMetricOverrides.erase(adjKey);
      SYSLOG(INFO) << "Removing metric override for adjacency: [" << adjNodeName
                   << ":" << interfaceName << "]";
    }
    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
LinkMonitor::getInterfaces() {
  VLOG(2) << "Dump Links requested, replying withV " << interfaces_.size()
          << " links";

  folly::Promise<std::unique_ptr<thrift::DumpLinksReply>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    // reply with the dump of known interfaces and their states
    thrift::DumpLinksReply reply;
    reply.thisNodeName = nodeId_;
    reply.isOverloaded = state_.isOverloaded;

    // Fill interface details
    for (auto& kv : interfaces_) {
      auto& ifName = kv.first;
      auto& interface = kv.second;
      auto ifDetails = thrift::InterfaceDetails(
          apache::thrift::FRAGILE,
          interface.getInterfaceInfo(),
          state_.overloadedLinks.count(ifName) > 0,
          0 /* custom metric value */,
          0 /* link flap back off time */);

      // Add metric override if any
      folly::Optional<int32_t> maybeMetric;
      if (state_.linkMetricOverrides.count(ifName) > 0) {
        maybeMetric.assign(state_.linkMetricOverrides.at(ifName));
      }
      apache::thrift::fromFollyOptional(
          ifDetails.metricOverride_ref(), maybeMetric);

      // Add link-backoff
      auto backoffMs = interface.getBackoffDuration();
      if (backoffMs.count() != 0) {
        ifDetails.linkFlapBackOffMs_ref() = backoffMs.count();
      } else {
        ifDetails.linkFlapBackOffMs_ref().reset();
      }

      reply.interfaceDetails.emplace(ifName, std::move(ifDetails));
    }
    p.setValue(std::make_unique<thrift::DumpLinksReply>(std::move(reply)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::AdjacencyDatabase>>
LinkMonitor::getLinkMonitorAdjacencies() {
  VLOG(2) << "Dump adj requested, reply with " << adjacencies_.size()
          << " adjs";

  folly::Promise<std::unique_ptr<thrift::AdjacencyDatabase>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    // build adjacency database
    thrift::AdjacencyDatabase adjDb;
    adjDb.thisNodeName = nodeId_;
    adjDb.isOverloaded = state_.isOverloaded;
    adjDb.nodeLabel = enableSegmentRouting_ ? state_.nodeLabel : 0;

    // fill adjacency details
    for (const auto& adjKv : adjacencies_) {
      // NOTE: copy on purpose
      auto adj = folly::copy(adjKv.second.adjacency);

      // Set link overload bit
      adj.isOverloaded = state_.overloadedLinks.count(adj.ifName) > 0;

      // Override metric with link metric if it exists
      adj.metric = folly::get_default(
          state_.linkMetricOverrides, adj.ifName, adj.metric);

      // Override metric with adj metric if it exists
      thrift::AdjKey adjKey;
      adjKey.nodeName = adj.otherNodeName;
      adjKey.ifName = adj.ifName;
      adj.metric =
          folly::get_default(state_.adjMetricOverrides, adjKey, adj.metric);

      adjDb.adjacencies.emplace_back(std::move(adj));
    }
    p.setValue(std::make_unique<thrift::AdjacencyDatabase>(std::move(adjDb)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::Link>>>
LinkMonitor::getAllLinks() {
  VLOG(2) << "Querying all links and their addresses from system";
  return collectAll(nlSock_->getAllLinks(), nlSock_->getAllIfAddresses())
      .deferValue(
          [](std::tuple<
              folly::Try<folly::Expected<std::vector<fbnl::Link>, int>>,
              folly::Try<folly::Expected<std::vector<fbnl::IfAddress>, int>>>&&
                 res) {
            std::unordered_map<int, thrift::Link> links;
            // Create links
            auto nlLinks = std::get<0>(res).value();
            if (nlLinks.hasError()) {
              throw fbnl::NlException("Failed fetching links", nlLinks.error());
            }
            for (auto& nlLink : nlLinks.value()) {
              thrift::Link link;
              link.ifName_ref() = nlLink.getLinkName();
              link.ifIndex_ref() = nlLink.getIfIndex();
              link.isUp_ref() = nlLink.isUp();
              links.emplace(nlLink.getIfIndex(), std::move(link));
            }

            // Add addresses
            auto nlAddrs = std::get<1>(res).value();
            if (nlAddrs.hasError()) {
              throw fbnl::NlException("Failed fetching addrs", nlAddrs.error());
            }
            for (auto& nlAddr : nlAddrs.value()) {
              auto& link = links.at(nlAddr.getIfIndex());
              link.networks_ref()->emplace_back(
                  toIpPrefix(nlAddr.getPrefix().value()));
            }

            // Convert to list and return
            auto result = std::make_unique<std::vector<thrift::Link>>();
            for (auto& kv : links) {
              result->emplace_back(std::move(kv.second));
            }
            return result;
          });
}

void
LinkMonitor::logNeighborEvent(thrift::SparkNeighborEvent const& event) {
  fbzmq::LogSample sample{};
  sample.addString(
      "event",
      apache::thrift::TEnumTraits<thrift::SparkNeighborEventType>::findName(
          event.eventType));
  sample.addString("node_name", nodeId_);
  sample.addString("neighbor", event.neighbor.nodeName);
  sample.addString("interface", event.ifName);
  sample.addString("remote_interface", event.neighbor.ifName);
  sample.addString("area", event.area);
  sample.addInt("rtt_us", event.rttUs);

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
  sample.addString("node_name", nodeId_);
  sample.addString("interface", iface);
  sample.addInt("backoff_ms", backoffTime.count());

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));

  SYSLOG(INFO) << "Interface " << iface << " is " << event
               << " and has backoff of " << backoffTime.count() << "ms";
}

void
LinkMonitor::logPeerEvent(
    const std::string& event,
    const std::string& peerName,
    const thrift::PeerSpec& peerSpec) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("node_name", nodeId_);
  sample.addString("peer_name", peerName);
  sample.addString("cmd_url", peerSpec.cmdUrl);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

} // namespace openr
