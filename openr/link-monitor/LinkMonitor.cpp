/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>

#include <openr/common/Constants.h>
#include <openr/common/EventLogger.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/link-monitor/AdjacencyEntry.h>
#include <openr/link-monitor/LinkMonitor.h>

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
  // Hard-drain state
  XLOG(DBG1) << fmt::format(
      "[Drain Status] Node Overloaded: {}",
      (*state.isOverloaded() ? "true" : "false"));
  if (!state.overloadedLinks()->empty()) {
    XLOG(DBG1) << fmt::format(
        "[Drain Status] Overloaded Links: {}",
        folly::join(",", *state.overloadedLinks()));
  }

  // Soft-drain state
  XLOG(DBG1) << fmt::format(
      "[Drain Status] Node Metric Increment: {}",
      *state.nodeMetricIncrementVal());
  if (!state.linkMetricIncrementMap()->empty()) {
    XLOG(DBG1) << fmt::format("[Drain Status] Link Metric Increment:");
    for (auto const& [key, val] : *state.linkMetricIncrementMap()) {
      XLOG(DBG1) << fmt::format("\t{}: {}", key, val);
    }
  }

  // [TO BE DEPRECATED]
  if (!state.linkMetricOverrides()->empty()) {
    XLOG(DBG1) << "\tlinkMetricOverrides: ";
    for (auto const& [key, val] : *state.linkMetricOverrides()) {
      XLOG(DBG1) << "\t\t" << key << ": " << val;
    }
  }
}

} // anonymous namespace

namespace openr {

/*
 * NetlinkEventProcessor serves as the general processor struct to parse
 * and understand different types of netlink event LinkMonitor interested in.
 */
struct LinkMonitor::NetlinkEventProcessor {
  LinkMonitor& lm_;
  explicit NetlinkEventProcessor(LinkMonitor& lm) : lm_(lm) {}

  void
  operator()(fbnl::Link&& link) {
    lm_.processLinkEvent(std::move(link));
  }

  void
  operator()(fbnl::IfAddress&& addr) {
    lm_.processAddressEvent(std::move(addr));
  }

  void
  operator()(fbnl::Neighbor&&) {}

  void
  operator()(fbnl::Rule&&) {}
};

//
// LinkMonitor code
//
LinkMonitor::LinkMonitor(
    std::shared_ptr<const Config> config,
    fbnl::NetlinkProtocolSocket* nlSock,
    PersistentStore* configStore,
    messaging::ReplicateQueue<InterfaceDatabase>& interfaceUpdatesQueue,
    messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue,
    messaging::ReplicateQueue<PeerEvent>& peerUpdatesQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue,
    messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue,
    messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue,
    messaging::RQueue<NeighborInitEvent> neighborUpdatesQueue,
    messaging::RQueue<fbnl::NetlinkEvent> netlinkEventsQueue)
    : nodeId_(config->getNodeName()),
      enablePerfMeasurement_(
          *config->getLinkMonitorConfig().enable_perf_measurement()),
      enableLinkStatusMeasurement_(
          *config->getLinkMonitorConfig().enable_link_status_measurement()),
      enableV4_(config->isV4Enabled()),
      useRttMetric_(*config->getLinkMonitorConfig().use_rtt_metric()),
      linkflapInitBackoff_(
          std::chrono::milliseconds(
              *config->getLinkMonitorConfig().linkflap_initial_backoff_ms())),
      linkflapMaxBackoff_(
          std::chrono::milliseconds(
              *config->getLinkMonitorConfig().linkflap_max_backoff_ms())),
      areas_(config->getAreas()),
      interfaceUpdatesQueue_(interfaceUpdatesQueue),
      prefixUpdatesQueue_(prefixUpdatesQueue),
      peerUpdatesQueue_(peerUpdatesQueue),
      logSampleQueue_(logSampleQueue),
      kvRequestQueue_(kvRequestQueue),
      expBackoff_(Constants::kInitialBackoff, Constants::kMaxBackoff),
      configStore_(configStore),
      nlSock_(nlSock) {
  // Check non-empty module ptr
  CHECK(configStore_);
  CHECK(nlSock_);

  // Hold time for synchronizing adjacencies in KvStore. We expect all the
  // adjacencies to be fully established within hold time after Open/R starts.
  // TODO: remove this with strict Open/R initialization sequence
  const std::chrono::seconds initialAdjHoldTime{
      *config->getConfig().adj_hold_time_s()};

  /**
   * The flag that indicates if adjacencies are to be advertised only when
   * adjacency hold timer is expired. Advertised only on hold timer expiry
   * when init optimization flag is disabled, otherwise advertised as soon
   * KvStoreSynced signal received from kvstore
   */
  if (auto enableInitOptimization =
          config->getConfig().enable_init_optimization()) {
    enableInitOptimization_ = *enableInitOptimization;
  }
  if (enableInitOptimization_) {
    XLOG(INFO) << "[Initialization] Init Optimization enabled";
  }

  // Schedule callback to advertise the initial set of adjacencies and prefixes
  adjHoldTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    XLOG(INFO) << "Hold time expired. Advertising adjacencies and addresses";
    // Advertise adjacencies and addresses after hold-timeout
    advertiseAdjacencies();
    advertiseRedistAddrs();
  });

  // Create throttled adjacency advertiser per area
  for (const auto& it : areas_) {
    const auto& area = it.first;
    advertiseAdjacenciesThrottledPerArea_.emplace(
        area,
        std::make_unique<AsyncThrottle>(
            getEvb(),
            Constants::kAdjacencyThrottleTimeout,
            [this, area]() noexcept { advertiseAdjacencies(area); }));
  }

  // Create throttled interfaces and addresses advertiser
  advertiseIfaceAddrThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kLinkThrottleTimeout, [this]() noexcept {
        advertiseIfaceAddr();
      });
  // Create timer. Timer is used for immediate or delayed executions.
  advertiseIfaceAddrTimer_ = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { advertiseIfaceAddr(); });
  // ATTN: LINK_DISCOVERY stage can't stuck forever if there is NO interface
  // being discovered.
  advertiseIfaceAddrTimer_->scheduleTimeout(
      Constants::kMaxDurationLinkDiscovery);

  /*
   * [Config-Store]
   *
   * Load link-monitor state from previous incarnation. This includes:
   *  - drain/undrain/softdrain state;
   *  - link/node overload status;
   *  - etc.;
   */
  auto state =
      configStore_->loadThriftObj<thrift::LinkMonitorState>(kConfigKey).get();

  /*
   * [Drain/Undrain/Softdrain Status]
   *
   *  - isOverloaded: this is the HARD-DRAIN status. The Open/R instance will
   *                  be hard-drained if this flag is TRUE;
   *  - nodeMetricIncrementVal: this is the SOFT_DRAIN value. The Open/R
   *                            instance will add metric to the adj database;
   */
  if (state.hasValue()) {
    XLOG(INFO) << "Successfully loaded link-monitor state from disk.";
    state_ = state.value();
    printLinkMonitorState(state_);
  } else {
    /*
     * In case that persistent store can't be read, `assumeDrained` flag will
     * be the back-up default value Open/R uses upon restarting.
     *
     * NOTE:
     * Depend on if soft-drain is enabled or not, Open/R will choose different
     * settings to start with.
     */
    auto assumeDrained = config->isAssumeDrained();
    if (config->isSoftdrainEnabled()) {
      const auto nodeInc = config->getNodeMetricIncrement();
      state_.nodeMetricIncrementVal() = assumeDrained ? nodeInc : 0;

      // ATTN: node should NOT be soft and hard drained at the same time
      state_.isOverloaded() = false;

      XLOG(INFO)
          << "[Drain Status] Failed to load persistent store from file system. "
          << fmt::format(
                 "Set node soft-drain increment value: {}",
                 *state_.nodeMetricIncrementVal());
    } else {
      state_.isOverloaded() = assumeDrained;

      // ATTN: node should NOT be soft and hard drained at the same time
      state_.nodeMetricIncrementVal() = 0;

      XLOG(INFO)
          << "[Drain Status] Failed to load persistent store from file system. "
          << fmt::format(
                 "Set node hard-drain state: {}",
                 *state_.isOverloaded() ? "DRAINED" : "UNDRAINED");
    }
  }

  /*
   * NOTE:
   * In case there is a discrepancy between persistent store and file system
   * flag set by drainer. Open/R will always trust fs flag over persistent
   * store(can be potentially corrupted/can't read due to software iisue/etc.)
   */
  if (config->isDrainerFlagInUse()) {
    if (config->isUndrainedPathExist()) {
      // Device is undrained
      state_.nodeMetricIncrementVal() = 0;
      state_.isOverloaded() = false;
    } else {
      // Device is hard-drained/soft-drained
      if (config->isSoftdrainEnabled()) {
        const auto nodeInc = config->getNodeMetricIncrement();
        state_.nodeMetricIncrementVal() = nodeInc;

        // ATTN: node should NOT be soft and hard drained at the same time
        state_.isOverloaded() = false;

        XLOG(INFO) << fmt::format(
            "[Drain Status] Override node soft-drain increment value: {}",
            nodeInc);
      } else {
        // ATTN: node should NOT be soft and hard drained at the same time
        state_.nodeMetricIncrementVal() = 0;

        state_.isOverloaded() = true;

        XLOG(INFO) << "[Drain Status] Override node hard-drain state: DRAINED";
      }
    }
  }

  // start initial dump timer
  adjHoldTimer_->scheduleTimeout(initialAdjHoldTime);

  /**
   * It is guaranteed that KVSTORE_SYNCED signal is received only after LM has
   * sent peer updates to kvstore (exception only when there are no peers to
   * learn). Thus, it is okay to treat this signal as soon it is received.
   */
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybePub = q.get(); // perform read
      if (maybePub.hasError()) {
        break;
      }

      folly::variant_match(
          std::move(maybePub).value(),
          [this](thrift::Publication&& pub /* unused */) { return; },
          [this](thrift::InitializationEvent&& event) {
            if (event == thrift::InitializationEvent::KVSTORE_SYNCED) {
              if (enableInitOptimization_) {
                XLOG(INFO)
                    << "Advertise adjacencies upon receiving KVSTORE_SYNCED";

                /**
                 * If KVSTORE_SYNCED was received after hold timer expired
                 * then adjacencies are already advertised. No need to
                 * advertise again in that case.
                 */
                if (adjHoldTimer_->isScheduled()) {
                  adjHoldTimer_->cancelTimeout();
                  advertiseAdjacencies();
                  advertiseRedistAddrs();
                }
              }
            }
          });
    }
  });

  // Add fiber to process the neighbor events
  addFiberTask([q = std::move(neighborUpdatesQueue), this]() mutable noexcept {
    XLOG(DBG1) << "Starting neighbor-event processing task";
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        break;
      }

      folly::variant_match(
          std::move(maybeEvent).value(),
          [this](NeighborEvents&& event) {
            // process different types of event
            processNeighborEvents(std::move(event));
          },
          [this](thrift::InitializationEvent&& event) {
            CHECK(event == thrift::InitializationEvent::NEIGHBOR_DISCOVERED)
                << fmt::format(
                       "Unexpected initialization event: {}",
                       apache::thrift::util::enumNameSafe(event));
            // Publish all peers to KvStore in OpenR initialization procedure.
            CHECK(!initialNeighborsReceived_)
                << "Received NEIGHBOR_DISCOVERED when initial neighbors received is set to true";
            initialNeighborsReceived_ = true;

            PeerEvent peerEvent;
            for (auto& [area, areaPeers] : peers_) {
              // Get added peers in each area.
              thrift::PeersMap peersToAdd;
              for (auto& [remoteNodeName, peerVal] : areaPeers) {
                peersToAdd.emplace(remoteNodeName, peerVal.tPeerSpec);
                logPeerEvent("ADD_PEER", remoteNodeName, peerVal.tPeerSpec);
              }
              peerEvent.emplace(
                  area, AreaPeerEvent(peersToAdd, {} /* peersToDel */));
            }
            // Send peers to add in all areas in a batch.
            peerUpdatesQueue_.push(std::move(peerEvent));
          });
    }
    XLOG(DBG1) << "[Exit] Neighbor-event processing task finished.";
  });

  // Add fiber to process the LINK/ADDR events from platform
  addFiberTask([q = std::move(netlinkEventsQueue), this]() mutable noexcept {
    XLOG(DBG1) << "Starting netlink event processing task";

    NetlinkEventProcessor visitor(*this);
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        break;
      }
      std::visit(visitor, std::move(*maybeEvent));
    }

    XLOG(DBG1) << "[Exit] Netlink-event processing task finished.";
  });

  // Add fiber to process interfaceDb syning from netlink platform
  addFiberTask([this]() mutable noexcept {
    XLOG(DBG1) << "Starting interface syncing task";
    syncInterfaceTask();
    XLOG(DBG1) << "[Exit] Interface-syncing task finished.";
  });

  // Initialize stats keys
  fb303::fbData->addStatExportType("link_monitor.neighbor_up", fb303::SUM);
  fb303::fbData->addStatExportType("link_monitor.neighbor_down", fb303::SUM);
  fb303::fbData->addStatExportType(
      "link_monitor.advertise_adjacencies", fb303::SUM);
  fb303::fbData->addStatExportType("link_monitor.advertise_links", fb303::SUM);
  fb303::fbData->addStatExportType(
      "link_monitor.sync_interface.failure", fb303::SUM);
}

void
LinkMonitor::stop() {
  XLOG(DBG1) << fmt::format(
      "[Exit] Send termination signals to stop {} tasks.", getFiberTaskNum());

  // Send stop signal for internal fibers
  syncInterfaceStopSignal_.post();

  // Invoke stop method of super class
  OpenrEventBase::stop();
  XLOG(INFO) << "[Exit] Successfully stopped LinkMonitor eventbase.";
}

void
LinkMonitor::neighborUpEvent(
    const NeighborEvent& event, bool isGracefulRestart) {
  const auto& neighborAddrV4 = event.neighborAddrV4;
  const auto& neighborAddrV6 = event.neighborAddrV6;
  const auto& localIfName = event.localIfName;
  const auto& remoteIfName = event.remoteIfName;
  const auto& remoteNodeName = event.remoteNodeName;
  const auto& area = event.area;
  const auto ctrlThriftPort = event.ctrlThriftPort;
  const auto rttUs = event.rttUs;
  const auto onlyUsedByOtherNode = event.adjOnlyUsedByOtherNode;

  // current unixtime
  auto now = std::chrono::system_clock::now();
  int64_t timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  thrift::Adjacency newAdj = createThriftAdjacency(
      remoteNodeName /* neighbor node name */,
      localIfName /* local ifName neighbor discovered on */,
      toString(neighborAddrV6) /* nextHopV6 */,
      toString(neighborAddrV4) /* nextHopV4 */,
      useRttMetric_ ? getRttMetric(rttUs) : 1 /* metric */,
      0 /* adjacency-label */,
      false /* overload bit */,
      useRttMetric_ ? rttUs : 0 /* rtt */,
      timestamp,
      1 /* weight */,
      remoteIfName);

  SYSLOG(INFO)
      << EventTag() << "Neighbor " << remoteNodeName << " is up on interface "
      << localIfName << ". Remote Interface: " << remoteIfName
      << ", metric: " << *newAdj.metric() << ", rttUs: " << rttUs
      << ", addrV4: " << toString(neighborAddrV4)
      << ", addrV6: " << toString(neighborAddrV6) << ", area: " << area
      << ", onlyUsedByOtherNode: " << std::boolalpha << onlyUsedByOtherNode;
  fb303::fbData->addStatValue("link_monitor.neighbor_up", 1, fb303::SUM);

  std::string peerAddr;
  if (!mockMode_) {
    // peer address used for KvStore external sync over thrift
    peerAddr = fmt::format("{}%{}", toString(neighborAddrV6), localIfName);
  } else {
    // TODO: address value of peerAddr under system test environment
    peerAddr =
        fmt::format("{}%{}", Constants::kPlatformHost.toString(), localIfName);
  }
  CHECK(!peerAddr.empty()) << "Got empty peerAddr";

  // create AdjacencyKey to uniquely map to AdjacencyEntry
  const auto adjKey = std::make_pair(remoteNodeName, localIfName);
  const auto tPeerSpec =
      createPeerSpec(peerAddr, ctrlThriftPort, thrift::KvStorePeerState::IDLE);

  // NOTE: for Graceful Restart(GR) case, we don't expect any adjacency
  // information change. Ignore the `onlyUsedByOtherNode` flag for adjacency
  // advertisement.
  adjacencies_[area].insert_or_assign(
      adjKey,
      AdjacencyEntry(
          adjKey,
          tPeerSpec,
          newAdj,
          useRttMetric_ ? getRttMetric(rttUs) : 1, /* baseMetric */
          false, /* isRestarting flag */
          isGracefulRestart ? false : onlyUsedByOtherNode));

  // update kvstore peer
  updateKvStorePeerNeighborUp(area, adjKey, tPeerSpec);

  // Advertise new adjancies in a throttled fashion
  advertiseAdjacenciesThrottledPerArea_.at(area)->operator()();
}

void
LinkMonitor::neighborAdjSyncedEvent(const NeighborEvent& event) {
  const auto& area = event.area;
  const auto& localIfName = event.localIfName;
  const auto& remoteNodeName = event.remoteNodeName;

  auto areaAdjIt = adjacencies_.find(area);
  if (areaAdjIt == adjacencies_.end()) {
    LOG(WARNING) << fmt::format(
        "Skip processing neighbor event due to no known adjacencies for area {}",
        area);
    return;
  }

  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  auto adjIt = areaAdjIt->second.find(adjId);
  if (adjIt == areaAdjIt->second.end()) {
    LOG(WARNING) << fmt::format(
        "Skip processing neighbor event due to adjKey: [{}, {}] not found",
        remoteNodeName,
        localIfName);
    return;
  }

  LOG(INFO) << fmt::format(
      "[Initialization] Reset onlyUsedByOtherNode flag for adjKey: [{}, {}]",
      remoteNodeName,
      localIfName);

  // reset flag to indicate adjacency can be used by everyone
  adjIt->second.onlyUsedByOtherNode_ = false;

  // advertise new adjacencies in a throttled fashion
  advertiseAdjacencies(area);
}

void
LinkMonitor::neighborDownEvent(const NeighborEvent& event) {
  const auto& remoteNodeName = event.remoteNodeName;
  const auto& localIfName = event.localIfName;
  const auto& area = event.area;

  SYSLOG(INFO) << EventTag() << "Neighbor " << remoteNodeName
               << " is down on interface " << localIfName;
  fb303::fbData->addStatValue("link_monitor.neighbor_down", 1, fb303::SUM);

  // A neighbor is down, but it's not necessary that link is down.
  // So we may not receive down netlink event.
  // However, we consider a down neighbor event indicates link to the
  // neighbor is down in link event database context.
  auto linkStatus = linkStatusRecords_.linkStatusMap()->find(localIfName);
  if (linkStatus != linkStatusRecords_.linkStatusMap()->end() &&
      *linkStatus->second.status() == thrift::LinkStatusEnum::UP) {
    // If link is up, change it to down
    updateLinkStatusRecords(
        localIfName,
        thrift::LinkStatusEnum::DOWN,
        getUnixTimeStampMs() /* now */);
  }

  auto areaAdjIt = adjacencies_.find(area);
  // No corresponding adj, ignore.
  if (areaAdjIt == adjacencies_.end()) {
    return;
  }

  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  auto adjValueIt = areaAdjIt->second.find(adjId);
  // invalid adj, ignore
  if (adjValueIt == areaAdjIt->second.end()) {
    return;
  }

  // update KvStore Peer
  updateKvStorePeerNeighborDown(area, adjId, adjValueIt->second.peerSpec_);

  // Remove such adjacencies.
  adjacencies_[area].erase(adjValueIt);
  if (adjacencies_[area].empty()) {
    adjacencies_.erase(areaAdjIt);
  }

  // Advertise adjacencies. Note - If all adjacencies in the area are gone,
  // the below function will persist an empty list of adjacencies to the
  // kvstore. Thus, correctly synchornizing kvstore with current state of
  // adjacencies. As an improvement, we could consider erasing this key
  // altogether.
  advertiseAdjacencies(area);
}

void
LinkMonitor::neighborRestartingEvent(const NeighborEvent& event) {
  const auto& remoteNodeName = event.remoteNodeName;
  const auto& localIfName = event.localIfName;
  const auto& area = event.area;

  SYSLOG(INFO) << EventTag() << "Neighbor " << remoteNodeName
               << " is restarting on interface " << localIfName;
  fb303::fbData->addStatValue(
      "link_monitor.neighbor_restarting", 1, fb303::SUM);

  auto areaAdjIt = adjacencies_.find(area);
  // invalid adj, ignore
  if (areaAdjIt == adjacencies_.end()) {
    return;
  }

  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  auto adjValueIt = areaAdjIt->second.find(adjId);
  // invalid adj, ignore
  if (adjValueIt == areaAdjIt->second.end()) {
    return;
  }

  // update adjacencies_ restarting-bit and advertise peers
  adjValueIt->second.isRestarting_ = true;

  // update KvStore Peer
  updateKvStorePeerNeighborDown(area, adjId, adjValueIt->second.peerSpec_);
}

void
LinkMonitor::neighborRttChangeEvent(const NeighborEvent& event) {
  const auto& remoteNodeName = event.remoteNodeName;
  const auto& localIfName = event.localIfName;
  const auto& rttUs = event.rttUs;
  int32_t newRttMetric = getRttMetric(rttUs);
  const auto& area = event.area;

  XLOG(DBG1) << "Metric value changed for neighbor " << remoteNodeName
             << " on interface: " << localIfName << " to " << newRttMetric;

  auto areaAdjIt = adjacencies_.find(area);
  if (areaAdjIt != adjacencies_.end()) {
    auto it = areaAdjIt->second.find({remoteNodeName, localIfName});
    if (it != areaAdjIt->second.end()) {
      auto& adj = it->second.adj_;
      adj.metric() = newRttMetric;
      adj.rtt() = rttUs;
      advertiseAdjacenciesThrottledPerArea_.at(area)->operator()();
    }
  }
}

void
LinkMonitor::updateKvStorePeerNeighborUp(
    const std::string& area,
    const AdjacencyKey& adjId,
    const thrift::PeerSpec& spec) {
  const auto& remoteNodeName = adjId.first;

  // update kvstore peers
  auto areaPeers = peers_.find(area);
  if (areaPeers == peers_.end()) {
    areaPeers =
        peers_
            .emplace(area, std::unordered_map<std::string, KvStorePeerValue>())
            .first;
  }

  auto peerVal = areaPeers->second.find(remoteNodeName);
  // kvstore peer exists, no need to refresh KvStore session
  if (peerVal != areaPeers->second.end()) {
    // update established adjs
    peerVal->second.establishedSparkNeighbors.emplace(adjId);
    return;
  }

  // create new KvStore Peer struct if it's first adj up
  areaPeers->second.emplace(remoteNodeName, KvStorePeerValue(spec, {adjId}));

  // Do not publish incremental peer event before initial peers are received and
  // published.
  if (!initialNeighborsReceived_) {
    return;
  }

  // Advertise KvStore peers immediately
  thrift::PeersMap peersToAdd;
  peersToAdd.emplace(remoteNodeName, spec);
  logPeerEvent("ADD_PEER", remoteNodeName, spec);

  PeerEvent event;
  event.emplace(area, AreaPeerEvent(peersToAdd, {} /*peersToDel*/));
  peerUpdatesQueue_.push(std::move(event));
}

void
LinkMonitor::updateKvStorePeerNeighborDown(
    const std::string& area,
    const AdjacencyKey& adjId,
    const thrift::PeerSpec& spec) {
  const auto& remoteNodeName = adjId.first;

  // find kvstore peer for adj
  const auto& areaPeers = peers_.find(area);
  if (areaPeers == peers_.end()) {
    XLOG(WARNING) << "No previous established KvStorePeer found for neighbor "
                  << remoteNodeName
                  << ". Skip updateKvStorePeer for interface down event on "
                  << adjId.second;
    return;
  }
  const auto& peerVal = areaPeers->second.find(remoteNodeName);
  if (peerVal == areaPeers->second.end()) {
    XLOG(WARNING) << "No previous established KvStorePeer found for neighbor "
                  << remoteNodeName
                  << ". Skip updateKvStorePeer for interface down event on "
                  << adjId.second;
    return;
  }

  // get handler of peer to update internal fields
  auto& peer = peerVal->second;

  // remove neighbor from establishedSparkNeighbors list
  peer.establishedSparkNeighbors.erase(adjId);

  // send PEER_DEL request to bring DOWN TCP session if all Spark neighbor
  // sessions are down.
  //
  // ATTN:
  //  - TCP session MUST be brought DOWN if this is the last neighbor session;
  //  - A new TCP session(PEER_UP) will be established once UP/RESTARTED;
  if (peer.establishedSparkNeighbors.empty()) {
    logPeerEvent("DEL_PEER", remoteNodeName, peer.tPeerSpec);

    // send peer del event
    std::vector<std::string> peersToDel{remoteNodeName};

    PeerEvent event;
    event.emplace(area, AreaPeerEvent({} /* peersToAdd */, peersToDel));
    peerUpdatesQueue_.push(std::move(event));

    // remove kvstore peer from internal store.
    areaPeers->second.erase(remoteNodeName);
    return;
  }

  // If current KvStore tPeerSpec != this sparkNeighbor's peerSpec, no need to
  // update peer spec, we are done.
  if (spec != peer.tPeerSpec) {
    return;
  }

  // Update tPeerSpec to peerSpec in remaining establishedSparkNeighbors.
  // e.g. adj_1 up -> adj_1 peer spec is used in KvStore Peer
  //      adj_2 up -> peer spec does not change
  //      adj_1 down -> Now adj_2 will be the peer-spec being used to establish
  peer.tPeerSpec = adjacencies_.at(area)
                       .at(*peer.establishedSparkNeighbors.begin())
                       .peerSpec_;

  // peer spec change, send peer add event
  logPeerEvent("ADD_PEER", remoteNodeName, peer.tPeerSpec);

  thrift::PeersMap peersToAdd;
  peersToAdd.emplace(remoteNodeName, peer.tPeerSpec);
  PeerEvent event;
  event.emplace(area, AreaPeerEvent(peersToAdd, {} /* peersToDel */));
  peerUpdatesQueue_.push(std::move(event));
}

void
LinkMonitor::advertiseAdjacencies(const std::string& area) {
  if (adjHoldTimer_->isScheduled()) {
    return;
  }

  auto& advertiseAdjPerAreaThrottle =
      advertiseAdjacenciesThrottledPerArea_.at(area);
  // Cancel throttle timeout if scheduled
  if (advertiseAdjPerAreaThrottle->isActive()) {
    advertiseAdjPerAreaThrottle->cancel();
  }

  // Extract information from `adjacencies_`
  auto adjDb = buildAdjacencyDatabase(area);

  XLOG(INFO) << fmt::format(
      "Updating adjacency database in KvStore with {} entries in area: {}",
      adjDb.adjacencies()->size(),
      area);

  // Persist `adj:node_Id` key into KvStore
  const auto keyName = Constants::kAdjDbMarker.toString() + nodeId_;
  std::string adjDbStr = writeThriftObjStr(adjDb, serializer_);
  auto persistAdjacencyKeyVal =
      PersistKeyValueRequest(AreaId{area}, keyName, adjDbStr);
  kvRequestQueue_.push(std::move(persistAdjacencyKeyVal));

  // Config is most likely to have changed. Update it in `ConfigStore`
  configStore_->storeThriftObj(kConfigKey, state_); // not awaiting on result

  // Update some flat counters
  fb303::fbData->addStatValue(
      "link_monitor.advertise_adjacencies", 1, fb303::SUM);
  fb303::fbData->setCounter("link_monitor.adjacencies", getTotalAdjacencies());
  for (const auto& [_, areaAdjacencies] : adjacencies_) {
    for (const auto& [_, adjValue] : areaAdjacencies) {
      auto& adj = adjValue.adj_;
      fb303::fbData->setCounter(
          "link_monitor.metric." + *adj.otherNodeName(), *adj.metric());
    }
  }
}

void
LinkMonitor::advertiseAdjacencies() {
  // advertise to all areas. Once area configuration per link is implemented
  // then adjacencies can be advertised to a specific area
  for (const auto& [areaId, _] : areas_) {
    // Update KvStore
    advertiseAdjacencies(areaId);
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
    XLOG(DBG2) << fmt::format(
        "advertiseIfaceAddr timer scheduled in {}ms", retryTime.count());
  }
}

void
LinkMonitor::advertiseInterfaces() {
  fb303::fbData->addStatValue("link_monitor.advertise_links", 1, fb303::SUM);

  // Create interface database
  InterfaceDatabase ifDb;
  for (auto& [_, interface] : interfaces_) {
    // Perform regex match
    if (!anyAreaShouldDiscoverOnIface(interface.getIfName())) {
      continue;
    }
    // Transform to `InterfaceInfo` object
    auto interfaceInfo = interface.getInterfaceInfo();

    // Override `UP` status
    interfaceInfo.isUp = interface.isActive();

    // Construct `InterfaceDatabase` object
    ifDb.emplace_back(std::move(interfaceInfo));
  }

  // Publish via replicate queue
  interfaceUpdatesQueue_.push(std::move(ifDb));

  // Mark `initialLinkDiscovered_` for the first call upon initialization
  if (!initialLinksDiscovered_) {
    initialLinksDiscovered_ = true;

    logInitializationEvent(
        "LinkMonitor", thrift::InitializationEvent::LINK_DISCOVERED);
  }
}

void
LinkMonitor::advertiseRedistAddrs() {
  std::map<folly::CIDRNetwork, std::vector<std::string>> prefixesToAdvertise;
  std::unordered_map<folly::CIDRNetwork, thrift::PrefixEntry> prefixMap;

  // Add redistribute addresses
  for (auto& [_, interface] : interfaces_) {
    // Ignore in-active interfaces
    if (!interface.isActive()) {
      XLOG(DBG2) << fmt::format(
          "Interface: {} is NOT active. Skip advertising.",
          interface.getIfName());
      continue;
    }

    // Derive list of area to advertise (NOTE: areas are ordered persistently)
    std::vector<std::string> dstAreas;
    for (auto const& [areaId, areaConf] : areas_) {
      if (areaConf.shouldRedistributeIface(interface.getIfName())) {
        dstAreas.emplace_back(areaId);
      }
    }

    // Do not advertise interface addresses if no destination area qualifies
    if (dstAreas.empty()) {
      continue;
    }

    // Add all prefixes of this interface
    for (auto& prefix : interface.getGlobalUnicastNetworks(enableV4_)) {
      // Add prefix in the cache
      prefixesToAdvertise.emplace(prefix, dstAreas);

      // Create prefix entry and populate the
      thrift::PrefixEntry prefixEntry;
      prefixEntry.prefix() = toIpPrefix(prefix);
      prefixEntry.type() = thrift::PrefixType::LOOPBACK;

      // Tags
      {
        auto& tags = prefixEntry.tags().value();
        tags.emplace("INTERFACE_SUBNET");
        tags.emplace(fmt::format("{}:{}", nodeId_, interface.getIfName()));
      }
      // Metrics
      {
        auto& metrics = prefixEntry.metrics().value();
        metrics.path_preference() = Constants::kDefaultPathPreference;
        metrics.source_preference() = Constants::kDefaultSourcePreference;
      }

      prefixMap.emplace(prefix, std::move(prefixEntry));
    }
  }

  // Find prefixes to advertise or update
  std::map<std::vector<std::string>, std::vector<thrift::PrefixEntry>>
      toAdvertise;
  for (auto const& [prefix, areas] : prefixesToAdvertise) {
    toAdvertise[areas].emplace_back(std::move(prefixMap.at(prefix)));

    XLOG(DBG1) << fmt::format(
        "Advertise LOOPBACK prefix: {} within areas: [{}]",
        folly::IPAddress::networkToString(prefix),
        folly::join(",", areas));
  }

  // Find prefixes to withdraw
  std::vector<thrift::PrefixEntry> toWithdraw;
  for (auto const& [prefix, areas] : advertisedPrefixes_) {
    if (prefixesToAdvertise.contains(prefix)) {
      continue; // Do not mark for withdraw
    }
    thrift::PrefixEntry prefixEntry;
    prefixEntry.prefix() = toIpPrefix(prefix);
    prefixEntry.type() = thrift::PrefixType::LOOPBACK;
    toWithdraw.emplace_back(std::move(prefixEntry));

    XLOG(DBG1) << fmt::format(
        "Withdraw LOOPBACK prefix: {} within areas: [{}]",
        folly::IPAddress::networkToString(prefix),
        folly::join(",", areas));
  }

  // Advertise prefixes (one for each area)
  for (auto& [areas, prefixEntries] : toAdvertise) {
    PrefixEvent event(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::LOOPBACK,
        std::move(prefixEntries),
        std::unordered_set<std::string>(areas.begin(), areas.end()));
    prefixUpdatesQueue_.push(std::move(event));
  }

  // Withdraw prefixes
  {
    PrefixEvent event(
        PrefixEventType::WITHDRAW_PREFIXES,
        thrift::PrefixType::LOOPBACK,
        std::move(toWithdraw));
    prefixUpdatesQueue_.push(std::move(event));
  }

  // Store advertised prefixes locally
  advertisedPrefixes_.swap(prefixesToAdvertise);
}

std::chrono::milliseconds
LinkMonitor::getRetryTimeOnUnstableInterfaces() {
  std::chrono::milliseconds minRemainMs{0};
  for (auto& [_, interface] : interfaces_) {
    if (interface.isActive()) {
      continue;
    }

    const auto& curRemainMs = interface.getBackoffDuration();
    if (curRemainMs.count() > 0) {
      XLOG(DBG2) << "Interface " << interface.getIfName()
                 << " is in backoff state for " << curRemainMs.count() << "ms";
      minRemainMs = std::min(linkflapMaxBackoff_, curRemainMs);
    }
  }

  return minRemainMs;
}

thrift::AdjacencyDatabase
LinkMonitor::buildAdjacencyDatabase(const std::string& area) {
  // prepare adjacency database
  thrift::AdjacencyDatabase adjDb;

  adjDb.thisNodeName() = nodeId_;
  adjDb.area() = area;
  adjDb.nodeLabel() = 0;

  // [Hard-Drain] set node overload bit
  adjDb.isOverloaded() = *state_.isOverloaded();

  // [Soft-Drain] set nodeMetricIncrementVal
  adjDb.nodeMetricIncrementVal() = *state_.nodeMetricIncrementVal();

  // populate thrift::AdjacencyDatabase.adjacencies based on
  // various condition.
  auto areaAdjIt = adjacencies_.find(area);
  if (areaAdjIt != adjacencies_.end()) {
    for (auto& [adjKey, adjValue] : areaAdjIt->second) {
      // NOTE: copy on purpose
      auto adj = folly::copy(adjValue.adj_);

      // [Hard-Drain] set link overload bit
      adj.isOverloaded() = state_.overloadedLinks()->contains(*adj.ifName());
      // [Soft-Drain] set linkMetricIncrementVal.
      auto linkMetricIt = state_.linkMetricIncrementMap()->find(*adj.ifName());
      adj.linkMetricIncrementVal() =
          linkMetricIt != state_.linkMetricIncrementMap()->end()
          ? linkMetricIt->second
          : 0;
      // Calculate the adj metric - there are 3 places potentially contributing
      // to the final result, which is stackable:
      //
      // 1. base metric derived from round-trip-time(RTT) or default hop-count;
      // 2. [Soft-Drain] node-level incremental metric;
      // 3. [Soft-Drain] link-level incremental metric.
      int32_t metric = adjValue.baseMetric_;

      // [TO BE DEPRECATED]
      // override metric with link metric if it exists
      metric = folly::get_default(
          *state_.linkMetricOverrides(), *adj.ifName(), adjValue.baseMetric_);

      // increment the node-level metric if any
      metric += *state_.nodeMetricIncrementVal();

      // increment the link-level metric if any
      metric += *adj.linkMetricIncrementVal();

      // ATTN: adj-metric override will be honored if being configured.
      thrift::AdjKey tAdjKey;
      tAdjKey.nodeName() = *adj.otherNodeName();
      tAdjKey.ifName() = *adj.ifName();
      metric =
          folly::get_default(*state_.adjMetricOverrides(), tAdjKey, metric);

      // NOTE: this will be the final metric used for SPF calculation later
      adj.metric() = metric;

      // set flag to indicate if adjacency will ONLY be used by other node
      adj.adjOnlyUsedByOtherNode() = adjValue.onlyUsedByOtherNode_;

      adjDb.adjacencies()->emplace_back(std::move(adj));
    }
  }

  // Add perf information if enabled
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "ADJ_DB_UPDATED");
    adjDb.perfEvents() = perfEvents;
  } else {
    DCHECK(!adjDb.perfEvents().has_value());
  }

  // Add link events if enabled
  if (enableLinkStatusMeasurement_) {
    adjDb.linkStatusRecords() = linkStatusRecords_;
  } else {
    DCHECK(!adjDb.linkStatusRecords().has_value());
  }

  return adjDb;
}

InterfaceEntry* FOLLY_NULLABLE
LinkMonitor::getOrCreateInterfaceEntry(const std::string& ifName) {
  // Return null if ifName doesn't quality regex match criteria
  if (!anyAreaShouldDiscoverOnIface(ifName) &&
      !anyAreaShouldRedistributeIface(ifName)) {
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

void
LinkMonitor::syncInterfaceTask() noexcept {
  // ATTN: use initial timeoff as the default value to wait for
  // small amount of time when thread starts before syncing
  std::chrono::milliseconds timeout{expBackoff_.getInitialBackoff()};

  while (true) { // Break when stop signal is ready
    // Sleep before next check
    if (syncInterfaceStopSignal_.try_wait_for(timeout)) {
      break; // Baton was posted
    } else {
      syncInterfaceStopSignal_.reset(); // Baton experienced timeout
    }

    auto success = syncInterfaces();
    if (success) {
      expBackoff_.reportSuccess();
      timeout = std::chrono::milliseconds(Constants::kPlatformSyncInterval);

      XLOG(DBG2) << fmt::format(
          "[Interface Sync] Successfully synced interfaceDb. Schedule next sync in {}ms",
          timeout.count());
    } else {
      // Apply exponential backoff and schedule next run
      expBackoff_.reportError();
      timeout = expBackoff_.getTimeRemainingUntilRetry();

      fb303::fbData->addStatValue(
          "link_monitor.sync_interface.failure", 1, fb303::SUM);

      XLOG(ERR) << fmt::format(
          "[Interface Sync] Failed to sync interfaceDb, apply exp backoff and retry in {}ms",
          timeout.count());
    }
  } // while
}

bool
LinkMonitor::syncInterfaces() {
  // Retrieve latest link snapshot from NetlinkProtocolSocket
  folly::Try<InterfaceDatabase> maybeIfDb;
  try {
    maybeIfDb = semifuture_getAllLinks().getTry(Constants::kReadTimeout);
    if (!maybeIfDb.hasValue()) {
      XLOG(ERR) << fmt::format(
          "[Interface Sync] Failed to sync interfaceDb. Exception: {}",
          folly::exceptionStr(maybeIfDb.exception()));

      return false;
    }
  } catch (const folly::FutureTimeout&) {
    XLOG(ERR)
        << "[Interface Sync] Timeout retrieving links. Retry in a moment.";

    return false;
  }

  // ATTN: treat empty link as failure to make sure LinkMonitor can keep
  // retrying to retrieve data from underneath platform.
  InterfaceDatabase ifDb = maybeIfDb.value();
  if (ifDb.empty()) {
    XLOG(ERR) << "[Interface Sync] No interface found. Retry in a moment.";
    return false;
  }

  XLOG(INFO) << fmt::format(
      "[Interface Sync] Successfully retrieved {} links from netlink.",
      ifDb.size());

  // Make updates in InterfaceEntry objects
  for (const auto& info : ifDb) {
    // update cache of ifIndex -> ifName mapping
    //  1) if ifIndex exists, override it with new ifName;
    //  2) if ifIndex does NOT exist, cache the ifName;
    ifIndexToName_[info.ifIndex] = info.ifName;

    // Get interface entry
    auto interfaceEntry = getOrCreateInterfaceEntry(info.ifName);
    if (!interfaceEntry) {
      continue;
    }

    const auto oldNetworks =
        interfaceEntry->getNetworks(); // NOTE: Copy intended
    const auto& newNetworks = info.networks;

    // Update link attributes
    const bool wasUp = interfaceEntry->isUp();
    interfaceEntry->updateAttrs(info.ifIndex, info.isUp);
    // If link status changes, keep its status/timestamp into record
    updateLinkStatusRecords(
        info.ifName,
        interfaceEntry->isUp() ? thrift::LinkStatusEnum::UP
                               : thrift::LinkStatusEnum::DOWN,
        interfaceEntry->getStatusChangeTimestamp());

    // Event logging
    logLinkEvent(
        interfaceEntry->getIfName(),
        wasUp,
        interfaceEntry->isUp(),
        interfaceEntry->getBackoffDuration());

    // Remove old addresses if they are not in new
    for (auto const& oldNetwork : oldNetworks) {
      if (!newNetworks.contains(oldNetwork)) {
        interfaceEntry->updateAddr(oldNetwork, false);
      }
    }

    // Add new addresses if they are not in old
    for (auto const& newNetwork : newNetworks) {
      if (!oldNetworks.contains(newNetwork)) {
        interfaceEntry->updateAddr(newNetwork, true);
      }
    }
  }
  return true;
}

void
LinkMonitor::processLinkEvent(fbnl::Link&& link) {
  XLOG(DBG3) << "Received Link Event from NetlinkProtocolSocket...";

  auto ifName = link.getLinkName();
  auto ifIndex = link.getIfIndex();
  auto isUp = link.isUp();

  // Cache interface index name mapping
  // ATTN: will create new ifIndex -> ifName mapping if it is unknown link
  //       `[]` operator is used in purpose
  ifIndexToName_[ifIndex] = ifName;

  auto interfaceEntry = getOrCreateInterfaceEntry(ifName);
  if (interfaceEntry) {
    const bool wasUp = interfaceEntry->isUp();
    interfaceEntry->updateAttrs(ifIndex, isUp);
    // If link status changes, keep its status/timestamp into record
    updateLinkStatusRecords(
        ifName,
        interfaceEntry->isUp() ? thrift::LinkStatusEnum::UP
                               : thrift::LinkStatusEnum::DOWN,
        interfaceEntry->getStatusChangeTimestamp());
    logLinkEvent(
        interfaceEntry->getIfName(),
        wasUp,
        interfaceEntry->isUp(),
        interfaceEntry->getBackoffDuration());
  }
}

void
LinkMonitor::processAddressEvent(fbnl::IfAddress&& addr) {
  XLOG(DBG3) << "Received Address Event from NetlinkProtocolSocket...";

  auto ifIndex = addr.getIfIndex();
  auto prefix = addr.getPrefix(); // std::optional<folly::CIDRNetwork>
  auto isValid = addr.isValid();

  // Check for interface name
  auto it = ifIndexToName_.find(ifIndex);
  if (it == ifIndexToName_.end()) {
    XLOG(ERR)
        << fmt::format("Address event for unknown iface index: {}", ifIndex);
    return;
  }

  // Cached ifIndex -> ifName mapping
  auto interfaceEntry = getOrCreateInterfaceEntry(it->second);
  if (interfaceEntry) {
    interfaceEntry->updateAddr(prefix.value(), isValid);
  }
}

void
LinkMonitor::processNeighborEvents(NeighborEvents&& events) {
  for (const auto& event : events) {
    const auto& neighborAddrV4 = event.neighborAddrV4;
    const auto& neighborAddrV6 = event.neighborAddrV6;
    const auto& localIfName = event.localIfName;
    const auto& remoteIfName = event.remoteIfName;
    const auto& remoteNodeName = event.remoteNodeName;
    const auto& area = event.area;

    XLOG(DBG1) << "Received neighbor event for " << remoteNodeName << " from "
               << remoteIfName << " at " << localIfName << " with addrs "
               << toString(neighborAddrV6) << " and "
               << (enableV4_ ? toString(neighborAddrV4) : "")
               << " Area:" << area
               << " Event Type: " << toString(event.eventType);

    switch (event.eventType) {
    case NeighborEventType::NEIGHBOR_UP:
      logNeighborEvent(event);
      neighborUpEvent(event, false);
      break;
    case NeighborEventType::NEIGHBOR_RESTARTED: {
      logNeighborEvent(event);
      neighborUpEvent(event, true);
      break;
    }
    case NeighborEventType::NEIGHBOR_ADJ_SYNCED: {
      logNeighborEvent(event);
      neighborAdjSyncedEvent(event);
      break;
    }
    case NeighborEventType::NEIGHBOR_RESTARTING: {
      logNeighborEvent(event);
      neighborRestartingEvent(event);
      break;
    }
    case NeighborEventType::NEIGHBOR_DOWN: {
      logNeighborEvent(event);
      neighborDownEvent(event);
      break;
    }
    case NeighborEventType::NEIGHBOR_RTT_CHANGE: {
      if (!useRttMetric_) {
        break;
      }
      logNeighborEvent(event);
      neighborRttChangeEvent(event);
      break;
    }
    default:
      XLOG(ERR) << "Unknown event type " << (int32_t)event.eventType;
    }
  } // for
}

// NOTE: add commands which set/unset overload bit or metric values will
// immediately advertise new adjacencies into the KvStore.
folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setNodeOverload(bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), isOverloaded]() mutable {
    std::string cmd =
        isOverloaded ? "SET_NODE_OVERLOAD" : "UNSET_NODE_OVERLOAD";
    if (*state_.isOverloaded() == isOverloaded) {
      XLOG(INFO) << "Skip cmd: [" << cmd << "]. Node already in target state: ["
                 << (isOverloaded ? "OVERLOADED" : "NOT OVERLOADED") << "]";
    } else {
      state_.isOverloaded() = isOverloaded;
      SYSLOG(INFO) << EventTag() << (isOverloaded ? "Setting" : "Unsetting")
                   << " overload bit for node";
      advertiseAdjacencies();
    }
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setInterfaceOverload(
    std::string interfaceName, bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), interfaceName, isOverloaded]() mutable {
        std::string cmd =
            isOverloaded ? "SET_LINK_OVERLOAD" : "UNSET_LINK_OVERLOAD";
        if (0 == interfaces_.count(interfaceName)) {
          XLOG(ERR) << "Skip cmd: [" << cmd
                    << "] due to unknown interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (isOverloaded && state_.overloadedLinks()->count(interfaceName)) {
          XLOG(INFO) << "Skip cmd: [" << cmd
                     << "]. Interface: " << interfaceName
                     << " is already overloaded";
          p.setValue();
          return;
        }

        if (!isOverloaded && !state_.overloadedLinks()->count(interfaceName)) {
          XLOG(INFO) << "Skip cmd: [" << cmd
                     << "]. Interface: " << interfaceName
                     << " is currently NOT overloaded";
          p.setValue();
          return;
        }

        if (isOverloaded) {
          state_.overloadedLinks()->insert(interfaceName);
          SYSLOG(INFO) << EventTag() << "Setting overload bit for interface "
                       << interfaceName;
        } else {
          state_.overloadedLinks()->erase(interfaceName);
          SYSLOG(INFO) << EventTag() << "Unsetting overload bit for interface "
                       << interfaceName;
        }
        scheduleAdvertiseAdjAllArea();
        p.setValue();
      });
  return sf;
}

// [TO_BE_DEPRECATED]
//
// ATTN: this achieves the SAME functionality of:
//
// semifuture_setInterfaceMetricIncrement
folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setLinkMetric(
    std::string interfaceName, std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), interfaceName, overrideMetric]() mutable {
        std::string cmd = overrideMetric.has_value() ? "SET_LINK_METRIC"
                                                     : "UNSET_LINK_METRIC";
        if (0 == interfaces_.count(interfaceName)) {
          XLOG(ERR) << "Skip cmd: [" << cmd
                    << "] due to unknown interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value() &&
            state_.linkMetricOverrides()->count(interfaceName) &&
            state_.linkMetricOverrides()[interfaceName] ==
                overrideMetric.value()) {
          XLOG(INFO) << "Skip cmd: " << cmd
                     << ". Overridden metric: " << overrideMetric.value()
                     << " already set for interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (!overrideMetric.has_value() &&
            !state_.linkMetricOverrides()->count(interfaceName)) {
          XLOG(INFO) << "Skip cmd: " << cmd
                     << ". No overridden metric found for interface: "
                     << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value()) {
          state_.linkMetricOverrides()[interfaceName] = overrideMetric.value();
          SYSLOG(INFO) << "Overriding metric for interface " << interfaceName
                       << " to " << overrideMetric.value();
        } else {
          state_.linkMetricOverrides()->erase(interfaceName);
          SYSLOG(INFO) << "Removing metric override for interface "
                       << interfaceName;
        }
        scheduleAdvertiseAdjAllArea();
        p.setValue();
      });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setAdjacencyMetric(
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
    *adjKey.ifName() = interfaceName;
    *adjKey.nodeName() = adjNodeName;

    auto adjacencyKey = std::make_pair(adjNodeName, interfaceName);
    bool unknownAdj{true};
    for (const auto& [_, areaAdjacencies] : adjacencies_) {
      if (areaAdjacencies.contains(adjacencyKey)) {
        unknownAdj = false;
        // Found it.
        break;
      }
    }
    // Invalid adj encountered, ignoring.
    if (unknownAdj) {
      XLOG(ERR) << "Skip cmd: [" << cmd << "] due to unknown adj: ["
                << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value() &&
        state_.adjMetricOverrides()->count(adjKey) &&
        state_.adjMetricOverrides()[adjKey] == overrideMetric.value()) {
      XLOG(INFO) << "Skip cmd: " << cmd
                 << ". Overridden metric: " << overrideMetric.value()
                 << " already set for: [" << adjNodeName << ":" << interfaceName
                 << "]";
      p.setValue();
      return;
    }

    if (!overrideMetric.has_value() &&
        !state_.adjMetricOverrides()->count(adjKey)) {
      XLOG(INFO) << "Skip cmd: " << cmd << ". No overridden metric found for: ["
                 << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value()) {
      state_.adjMetricOverrides()[adjKey] = overrideMetric.value();
      SYSLOG(INFO) << "Overriding metric for adjacency: [" << adjNodeName << ":"
                   << interfaceName << "] to " << overrideMetric.value();
    } else {
      state_.adjMetricOverrides()->erase(adjKey);
      SYSLOG(INFO) << "Removing metric override for adjacency: [" << adjNodeName
                   << ":" << interfaceName << "]";
    }
    scheduleAdvertiseAdjAllArea();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_unsetNodeInterfaceMetricIncrement() {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    if (0 == state_.nodeMetricIncrementVal()) {
      // the increment value already applied
      XLOG(INFO) << "Skip cmd: unsetNodeInterfaceMetricIncrement."
                 << "\n  Already set this node-level metric increment to 0";
      p.setValue();
      return;
    }
    // reset the increment to 0
    state_.nodeMetricIncrementVal() = 0;

    scheduleAdvertiseAdjAllArea();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setNodeInterfaceMetricIncrement(
    int32_t metricIncrementVal) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), metricIncrementVal]() mutable {
    // invalid increment input
    if (metricIncrementVal <= 0) {
      XLOG(ERR)
          << "Skip cmd: setNodeInterfaceMetricIncrement."
          << "\n  Parameter `metricIncrementVal` should be a positive integer.";
      p.setValue();
      return;
    }

    if (metricIncrementVal == *state_.nodeMetricIncrementVal()) {
      // the increment value already applied
      XLOG(INFO) << "Skip cmd: setNodeInterfaceMetricIncrement"
                 << "\n  Already set this node-level metric increment value: "
                 << metricIncrementVal;
      p.setValue();
      return;
    }

    XLOG(INFO)
        << "Set the node-level static metric increment value:"
        << "\n  Old increment value: " << *state_.nodeMetricIncrementVal()
        << "\n  Setting new increment value: " << metricIncrementVal;

    // set the state
    state_.nodeMetricIncrementVal() = metricIncrementVal;

    scheduleAdvertiseAdjAllArea();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setInterfaceMetricIncrement(
    std::string interfaceName, int32_t metricIncrementVal) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceName,
                        metricIncrementVal]() mutable {
    // invalid increment input
    if (metricIncrementVal <= 0) {
      XLOG(ERR)
          << "Skip cmd: setInterfaceMetricIncrement."
          << "\n   Parameter `metricIncrementVal` should be a positive integer.";
      p.setValue();
      return;
    }

    if (0 == interfaces_.count(interfaceName)) {
      XLOG(ERR) << "Skip cmd: setInterfaceMetricIncrement."
                << "due to unknown interface: " << interfaceName;
      p.setValue();
      return;
    }

    auto it = state_.linkMetricIncrementMap()->find(interfaceName);
    if (it != state_.linkMetricIncrementMap()->end() &&
        it->second == metricIncrementVal) {
      XLOG(INFO) << "Skip cmd: setInterfaceMetricIncrement."
                 << "\n  Increment metric: " << metricIncrementVal
                 << " already set for interface: " << interfaceName;
      p.setValue();
      return;
    }

    // set the link-level metric increment
    auto oldMetric =
        folly::get_default(*state_.linkMetricIncrementMap(), interfaceName, 0);
    SYSLOG(INFO) << "Increment metric for interface " << interfaceName
                 << "\n  Old increment value: " << oldMetric
                 << "\n  Setting new increment value: " << metricIncrementVal;

    state_.linkMetricIncrementMap()[interfaceName] = metricIncrementVal;

    scheduleAdvertiseAdjAllArea();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setInterfaceMetricIncrementMulti(
    std::vector<std::string> interfaceNames, int32_t metricIncrementVal) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceNames,
                        metricIncrementVal]() mutable {
    // invalid increment input
    if (metricIncrementVal <= 0) {
      XLOG(ERR)
          << "Skip cmd: setInterfaceMetricIncrement."
          << "\n   Parameter `metricIncrementVal` should be a positive integer.";
      p.setValue();
      return;
    }
    setInterfaceMetricIncrementHelper(p, interfaceNames, metricIncrementVal);
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_unsetInterfaceMetricIncrementMulti(
    std::vector<std::string> interfaces) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), interfaces]() mutable {
    setInterfaceMetricIncrementHelper(p, interfaces, 0);
  });
  return sf;
}

void
LinkMonitor::setInterfaceMetricIncrementHelper(
    folly::Promise<folly::Unit>& p,
    std::vector<std::string> interfaces,
    int32_t metrics) {
  // Caller must ensure non negative value
  CHECK_GE(metrics, 0);
  auto command = metrics != 0 ? "setInterfaceMetricIncrement"
                              : "unsetInterfaceMetricIncrement";

  for (const auto& interfaceName : interfaces) {
    if (0 == interfaces_.count(interfaceName)) {
      XLOG(ERR) << "Skip cmd: " << command
                << " due to unknown interface: " << interfaceName;
      p.setValue();
      return;
    }
  }
  bool stateChanged = false;

  for (const auto& interfaceName : interfaces) {
    auto oldMetric =
        folly::get_default(*state_.linkMetricIncrementMap(), interfaceName, 0);
    if (oldMetric == metrics) {
      XLOG(INFO) << "Skip cmd: " << command
                 << "\n  Increment metric: " << metrics
                 << " already set for interface: " << interfaceName;
      continue;
    }

    SYSLOG(INFO) << "Increment metric for interface " << interfaceName
                 << "\n  Old increment value: " << oldMetric
                 << "\n  Setting new increment value: " << metrics;

    if (metrics == 0) {
      state_.linkMetricIncrementMap()->erase(interfaceName);
    } else {
      state_.linkMetricIncrementMap()[interfaceName] = metrics;
    }
    stateChanged = true;
  }

  if (stateChanged) {
    scheduleAdvertiseAdjAllArea();
  }
  p.setValue();
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_unsetInterfaceMetricIncrement(
    std::string interfaceName) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), interfaceName]() mutable {
    if (0 == interfaces_.count(interfaceName)) {
      XLOG(ERR) << "Skip cmd: [unsetInterfaceMetricIncrement]."
                << "due to unknown interface: " << interfaceName;
      p.setValue();
      return;
    }

    auto it = state_.linkMetricIncrementMap()->find(interfaceName);
    if (it == state_.linkMetricIncrementMap()->end()) {
      XLOG(INFO) << "Skip cmd: [unsetInterfaceMetricIncrement]."
                 << "due the interface " << interfaceName
                 << "didn't set the link-level metric increment before.";
      p.setValue();
      return;
    }

    SYSLOG(INFO) << "Removing link-level metric increment for interface: "
                 << interfaceName;
    state_.linkMetricIncrementMap()->erase(it);

    scheduleAdvertiseAdjAllArea();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
LinkMonitor::semifuture_getInterfaces() {
  XLOG(DBG2)
      << fmt::format("Dump links requested with {} links", interfaces_.size());

  folly::Promise<std::unique_ptr<thrift::DumpLinksReply>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    thrift::DumpLinksReply reply;

    // Populate nodeId
    reply.thisNodeName() = nodeId_;

    // Populate node-level overload state(hard-drain)
    reply.isOverloaded() = *state_.isOverloaded();

    // Populate node-level metric override(soft-drain)
    reply.nodeMetricIncrementVal() = *state_.nodeMetricIncrementVal();

    // Fill interface details
    for (auto& [_, interface] : interfaces_) {
      const auto& ifName = interface.getIfName();

      thrift::InterfaceDetails ifDetails;
      ifDetails.info() = interface.getInterfaceInfo().toThrift();

      // Populate link-level overload state
      ifDetails.isOverloaded() = state_.overloadedLinks()->contains(ifName);

      // [TO_BE_DEPRECATED] Add metric override if any
      if (state_.linkMetricOverrides()->contains(ifName)) {
        ifDetails.metricOverride() = state_.linkMetricOverrides()->at(ifName);
      }

      // Populate link-level metric override if any
      auto it = state_.linkMetricIncrementMap()->find(ifName);
      if (it != state_.linkMetricIncrementMap()->end()) {
        ifDetails.linkMetricIncrementVal() = it->second;
      }

      // Add link-backoff
      auto backoffMs = interface.getBackoffDuration();
      if (backoffMs.count() != 0) {
        ifDetails.linkFlapBackOffMs() = backoffMs.count();
      } else {
        ifDetails.linkFlapBackOffMs().reset();
      }

      reply.interfaceDetails()->emplace(ifName, std::move(ifDetails));
    }
    p.setValue(std::make_unique<thrift::DumpLinksReply>(std::move(reply)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
LinkMonitor::semifuture_getAdjacencies(thrift::AdjacenciesFilter filter) {
  XLOG(DBG2) << "Dump adj requested, reply with " << getTotalAdjacencies()
             << " adjs";

  folly::Promise<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable {
        auto res = std::make_unique<std::vector<thrift::AdjacencyDatabase>>();
        if (filter.selectAreas()->empty()) {
          for (auto const& [areaId, _] : areas_) {
            res->push_back(buildAdjacencyDatabase(areaId));
          }
        } else {
          for (auto const& areaId : *filter.selectAreas()) {
            res->push_back(buildAdjacencyDatabase(areaId));
          }
        }
        p.setValue(std::move(res));
      });
  return sf;
}

folly::SemiFuture<std::unique_ptr<
    std::map<std::string, std::vector<thrift::AdjacencyDatabase>>>>
LinkMonitor::semifuture_getAreaAdjacencies(thrift::AdjacenciesFilter filter) {
  XLOG(DBG2) << "Dump adj requested, reply with " << getTotalAdjacencies()
             << " adjs";

  folly::Promise<std::unique_ptr<
      std::map<std::string, std::vector<thrift::AdjacencyDatabase>>>>
      p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable {
        auto res = std::make_unique<
            std::map<std::string, std::vector<thrift::AdjacencyDatabase>>>();
        if (filter.selectAreas()->empty()) {
          for (auto const& [areaId, _] : areas_) {
            res->operator[](areaId).push_back(buildAdjacencyDatabase(areaId));
          }
        } else {
          for (auto const& areaId : *filter.selectAreas()) {
            res->operator[](areaId).push_back(buildAdjacencyDatabase(areaId));
          }
        }
        p.setValue(std::move(res));
      });
  return sf;
}

folly::SemiFuture<InterfaceDatabase>
LinkMonitor::semifuture_getAllLinks() {
  XLOG(DBG2) << "Querying all links and their addresses from system";
  return collectAll(nlSock_->getAllLinks(), nlSock_->getAllIfAddresses())
      .deferValue(
          [](std::tuple<
              folly::Try<folly::Expected<std::vector<fbnl::Link>, int>>,
              folly::Try<folly::Expected<std::vector<fbnl::IfAddress>, int>>>&&
                 res) {
            std::unordered_map<int64_t, InterfaceInfo> links;
            // Create links
            auto nlLinks = std::get<0>(res).value();
            if (nlLinks.hasError()) {
              throw fbnl::NlException("Failed fetching links", nlLinks.error());
            }
            for (auto& nlLink : nlLinks.value()) {
              // explicitly constuct linkEntry with EMPTY addresses
              InterfaceInfo link(
                  nlLink.getLinkName(), nlLink.isUp(), nlLink.getIfIndex(), {});
              links.emplace(nlLink.getIfIndex(), std::move(link));
            }

            // Add addresses
            auto nlAddrs = std::get<1>(res).value();
            if (nlAddrs.hasError()) {
              throw fbnl::NlException("Failed fetching addrs", nlAddrs.error());
            }
            for (auto& nlAddr : nlAddrs.value()) {
              auto& link = links.at(nlAddr.getIfIndex());
              link.networks.emplace(nlAddr.getPrefix().value());
            }

            // Convert to list and return
            InterfaceDatabase result{};
            for (auto& [_, link] : links) {
              result.emplace_back(std::move(link));
            }
            return result;
          });
}

void
LinkMonitor::logNeighborEvent(NeighborEvent const& event) {
  LogSample sample{};
  sample.addString("event", toString(event.eventType));
  sample.addString("neighbor", event.remoteNodeName);
  sample.addString("interface", event.localIfName);
  sample.addString("remote_interface", event.remoteIfName);
  sample.addString("area", event.area);
  sample.addInt("rtt_us", event.rttUs);

  logSampleQueue_.push(std::move(sample));
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

  LogSample sample{};
  const std::string event = isUp ? "UP" : "DOWN";
  sample.addString("event", fmt::format("IFACE_{}", event));
  sample.addString("interface", iface);
  sample.addInt("backoff_ms", backoffTime.count());

  logSampleQueue_.push(sample);

  SYSLOG(INFO) << "Interface " << iface << " is " << event
               << " and has backoff of " << backoffTime.count() << "ms";
}

void
LinkMonitor::logPeerEvent(
    const std::string& event,
    const std::string& peerName,
    const thrift::PeerSpec& peerSpec) {
  LogSample sample{};
  const auto& peerAddr = *peerSpec.peerAddr();
  const auto& ctrlPort = *peerSpec.ctrlPort();
  sample.addString("event", event);
  sample.addString("node_name", nodeId_);
  sample.addString("peer_name", peerName);
  sample.addString("peer_addr", peerAddr);
  sample.addInt("ctrl_port", ctrlPort);

  logSampleQueue_.push(sample);

  SYSLOG(INFO) << "[" << event << "] for " << peerName
               << " with address: " << peerAddr
               << ", port: " << std::to_string(ctrlPort);
}

bool
LinkMonitor::anyAreaShouldDiscoverOnIface(std::string const& iface) const {
  bool anyMatch = false;
  for (auto const& [_, areaConf] : areas_) {
    anyMatch |= areaConf.shouldDiscoverOnIface(iface);
  }
  return anyMatch;
}

bool
LinkMonitor::anyAreaShouldRedistributeIface(std::string const& iface) const {
  bool anyMatch = false;
  for (auto const& [_, areaConf] : areas_) {
    anyMatch |= areaConf.shouldRedistributeIface(iface);
  }
  return anyMatch;
}

void
LinkMonitor::scheduleAdvertiseAdjAllArea() {
  for (const auto& [area, _] : areas_) {
    advertiseAdjacenciesThrottledPerArea_.at(area)->operator()();
  }
}

/// Total # of adjacencies stored across all areas.
size_t
LinkMonitor::getTotalAdjacencies() {
  size_t numAdjacencies{0};
  for (const auto& [_, areaAdjacencies] : adjacencies_) {
    numAdjacencies += areaAdjacencies.size();
  }
  return numAdjacencies;
}

void
LinkMonitor::updateLinkStatusRecords(
    const std::string& ifName,
    thrift::LinkStatusEnum ifStatus,
    int64_t ifStatusChangeTimestamp) {
  thrift::LinkStatus linkStatus;
  linkStatus.status() = ifStatus;
  linkStatus.unixTs() = ifStatusChangeTimestamp;
  linkStatusRecords_.linkStatusMap()[ifName] = std::move(linkStatus);
}

} // namespace openr
