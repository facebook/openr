/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Types.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreClientInternal.h>
#include <openr/link-monitor/InterfaceEntry.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/spark/Spark.h>

namespace openr {

using AdjacencyKey = std::
    pair<std::string /* remoteNodeName */, std::string /* localInterfaceName*/>;

struct AdjacencyValue {
  thrift::PeerSpec peerSpec;
  thrift::Adjacency adjacency;
  bool isRestarting{false};
  std::string area{};

  AdjacencyValue() {}
  AdjacencyValue(
      std::string areaId,
      thrift::PeerSpec spec,
      thrift::Adjacency adj,
      bool restarting = false)
      : peerSpec(spec),
        adjacency(adj),
        isRestarting(restarting),
        area(areaId) {}
};

// KvStore Peer Value
struct KvStorePeerValue {
  // Current established KvStorePeer Spec, this usually is taken from the first
  // established Spark Neighbor
  thrift::PeerSpec tPeerSpec;

  // one time flag set by KvStore Peer Initialized event
  // Only when a peer reaches initialSynced state, its adjancency UP events
  // can be announced to the network
  bool initialSynced{false};

  // Established spark neighbors related to remoteNodeName. KvStore Peer State
  // Machine is a continuation of Spark Neighbor State Machine, tracking spark
  // neighbors here help us decide when to send ADD/DEL KvStorePeer request.
  //
  // * Notice this is different from Adjacencies.
  // An Adjancency remains up when remote spark neighbor performs a graceful
  // restart, but spark neighbor state is IDLE during neighbor restart. Here
  // we only track spark neighbors in ESTABLISHED state.
  std::unordered_set<AdjacencyKey> establishedSparkNeighbors;

  KvStorePeerValue(
      thrift::PeerSpec tPeerSpec,
      bool initialSynced,
      std::unordered_set<AdjacencyKey> establishedSparkNeighbors)
      : tPeerSpec(std::move(tPeerSpec)),
        initialSynced(initialSynced),
        establishedSparkNeighbors(std::move(establishedSparkNeighbors)) {}
};

/*
 * This class is mainly responsible for
 *    - Monitor system interface status & address;
 *    - Initiate neighbor discovery for newly added links;
 *    - Maintain KvStore peering with discovered neighbors;
 *    - Maintain `AdjacencyDatabase` of current node in KvStore by injecting
 *      `adj:<node-name>`;
 *
 * Please refer to `openr/docs/Protocol_Guide/LinkMonitor.md` for more details.
 */
class LinkMonitor final : public OpenrEventBase {
 public:
  LinkMonitor(
      // config
      std::shared_ptr<const Config> config,
      // raw ptr for modules
      fbnl::NetlinkProtocolSocket* nlSock,
      KvStore* kvstore,
      PersistentStore* configStore,
      // producer queue
      messaging::ReplicateQueue<InterfaceDatabase>& interfaceUpdatesQueue,
      messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue,
      messaging::ReplicateQueue<PeerEvent>& peerUpdatesQueue,
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue,
      // consumer queue
      messaging::RQueue<NeighborEvents> neighborUpdatesQueue,
      messaging::RQueue<KvStoreSyncEvent> kvStoreSyncEventsQueue,
      messaging::RQueue<fbnl::NetlinkEvent> netlinkEventsQueue,
      // if set, we will override drain state from persistent store with
      // assumeDrained value
      bool overrideDrainState);

  ~LinkMonitor() override = default;

  /**
   * Override stop method of OpenrEventBase
   */
  void stop() override;

  /*
   * Mock mode
   *
   * under mock mode, LinkMonitor will report tcp://[::]:port as kvstore
   * communication URL instead of using link local address
   */
  void
  setAsMockMode() {
    mockMode_ = true;
  }

  /*
   * Public APIs to change metric:
   * NOTE: except node overload, all requests will be throttled.
   *
   * - Set/unset node overload (Node Drain)
   * - Set/unset interface overload
   * - Set/unset interface metric
   * - Set/unset node adj metric
   * - Dump interface/link information
   * - Dump adjacency database information
   * - Dump links information from netlinkProtocolSocket
   */
  folly::SemiFuture<folly::Unit> semifuture_setNodeOverload(bool isOverloaded);
  folly::SemiFuture<folly::Unit> semifuture_setInterfaceOverload(
      std::string interfaceName, bool isOverloaded);
  folly::SemiFuture<folly::Unit> semifuture_setLinkMetric(
      std::string interfaceName, std::optional<int32_t> overrideMetric);
  folly::SemiFuture<folly::Unit> semifuture_setAdjacencyMetric(
      std::string interfaceName,
      std::string adjNodeName,
      std::optional<int32_t> overrideMetric);
  folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
  semifuture_getInterfaces();
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
  semifuture_getAdjacencies(thrift::AdjacenciesFilter filter = {});
  folly::SemiFuture<InterfaceDatabase> semifuture_getAllLinks();

 private:
  // make no-copy
  LinkMonitor(const LinkMonitor&) = delete;
  LinkMonitor& operator=(const LinkMonitor&) = delete;

  /*
   * [Spark] neighbor event functions
   */

  // process neighbor event updates from Spark module
  void processNeighborEvents(NeighborEvents&& events);

  // individual neighbor event function
  void neighborUpEvent(const thrift::SparkNeighbor& info);
  void neighborRestartingEvent(const thrift::SparkNeighbor& info);
  void neighborDownEvent(const thrift::SparkNeighbor& info);
  void neighborRttChangeEvent(const thrift::SparkNeighbor& info);

  /*
   * [KvStore] initial sync event
   */
  void processKvStoreSyncEvent(KvStoreSyncEvent&& event);

  /*
   * [Netlink Platform]
   *
   * LinkMonitor maintains multiple fiber tasks to:
   *  1) retrieve interface from netlink for initial/periodic interface sync;
   *  2) process LINK/ADDR event updates from platform
   */

  // visitor dispatcher class we use to parse messages for further processing
  struct NetlinkEventProcessor;

  void processLinkEvent(fbnl::Link&& link);
  void processAddressEvent(fbnl::IfAddress&& addr);

  void syncInterfaceTask() noexcept;
  bool syncInterfaces();

  // Get or create InterfaceEntry object.
  // Returns nullptr if ifName doesn't qualify regex match
  // used in syncInterfaces() and LINK/ADDRESS EVENT
  InterfaceEntry* FOLLY_NULLABLE
  getOrCreateInterfaceEntry(const std::string& ifName);

  /*
   * [Kvstore] PEER UP/DOWN events sent to Kvstore over peerUpdatesQueue_
   *
   * 1) updateKvStorePeerNeighborUp:
   *    Called upon Spark neighborUp events.
   *    - If there's only one adj for this peer, we create new KvStorePeerValue
   *      and send ADD_PEER request to KvStore;
   *    - If there are established adjs, just update the existing KvStorePeer
   *      struct. DO NOT report ADD_PEER event again.
   * 2) updateKvStorePeerNeighborDown:
   *    Called upon Spark neighborRestarting, neighborDown events.
   *    - In single adj case, send DEL_PEER request;
   *    - In parallel adj case, update peer spec with left adj peer spec, send
   *      ADD_PEER request if new peer spec is different;
   */
  void updateKvStorePeerNeighborUp(
      const std::string& area,
      const AdjacencyKey& adjId,
      const AdjacencyValue& adjVal);

  void updateKvStorePeerNeighborDown(
      const std::string& area,
      const AdjacencyKey& adjId,
      const AdjacencyValue& adjVal);

  /*
   * [Kvstore] Advertise my adjacencies_
   * kvStoreClient_->persistKey OR kvRequestQueue.push(PersistKeyValueRequest)
   *
   * Called upon spark neighbor events: up/down/rtt (restarting does not trigger
   * adj update)
   */
  void advertiseAdjacencies(const std::string& area);
  void advertiseAdjacencies(); // Advertise my adjacencies_ in to all areas

  /*
   * [Spark/Fib] Advertise interfaces_ over interfaceUpdatesQueue_ to Spark/Fib
   *
   * Called in advertiseIfaceAddr() upon interface changes
   */
  void advertiseInterfaces();

  /*
   * [PrefixManager] Advertise redistribute prefixes over prefixUpdatesQueue_ to
   * prefix manager "redistribute prefixes" includes addresses of interfaces
   * that match redistribute_interface_regexes
   *
   * Called in
   * - adjHoldTimer_ during initial start
   * - and advertiseIfaceAddr() upon interface changes
   */
  void advertiseRedistAddrs();

  /*
   * [Util function] general function used for util purpose
   */

  // call advertiseInterfaces() and advertiseRedistAddrs()
  // throttle updates if there's any unstable interface by
  // getRetryTimeOnUnstableInterfaces() time
  // used in advertiseIfaceAddrThrottled_ and advertiseIfaceAddrTimer_
  // called upon interface change in getOrCreateInterfaceEntry()
  void advertiseIfaceAddr();

  // get next try time, which should be the minimum remaining time among
  // all unstable (getTimeRemainingUntilRetry() > 0) interfaces.
  // return 0 if no more unstable interface
  std::chrono::milliseconds getRetryTimeOnUnstableInterfaces();

  // build AdjacencyDatabase
  thrift::AdjacencyDatabase buildAdjacencyDatabase(const std::string& area);

  // returns any(a.shouldDiscoverOnIface(iface) for a in areas_)
  bool anyAreaShouldDiscoverOnIface(std::string const& iface) const;

  // returns any(a.anyAreaShouldRedistributeIface(iface) for a in areas_)
  bool anyAreaShouldRedistributeIface(std::string const& iface) const;

  /*
   * [Logging]
   *
   * events logging submitted to Monitor
   */
  void logNeighborEvent(NeighborEvent const& event);
  void logLinkEvent(
      const std::string& iface,
      bool wasUp,
      bool isUp,
      std::chrono::milliseconds backoffTime);
  void logPeerEvent(
      const std::string& event,
      const std::string& peerName,
      const thrift::PeerSpec& peerSpec);

  /*
   * [Segment Routing]
   *
   * util functions to support label allocation in dynamic/static way
   */

  // Get the label range from the configuration
  const std::pair<int32_t, int32_t> getNodeSegmentLabelRange(
      AreaConfiguration const& areaConfig) const;

  // Get static area node segment label
  int32_t getStaticNodeSegmentLabel(AreaConfiguration const& areaConfig) const;

  //
  // immutable state/invariants
  //

  // used to build the key names for this node
  const std::string nodeId_;
  // enable performance measurement
  const bool enablePerfMeasurement_{false};
  // enable v4
  bool enableV4_{false};
  // enable segment routing
  bool enableSegmentRouting_{false};
  // Feature gate for new graceful restart behavior:
  // If enableNewGRBehavior_, GR = neighbor restart -> kvstore initial sync.
  // We promote adj up after kvstore initial sync event.
  // Else: GR = neighbor restart -> spark neighbor establishment.
  bool enableNewGRBehavior_{false};
  // prefix forwarding type and algorithm
  thrift::PrefixForwardingType prefixForwardingType_;
  thrift::PrefixForwardingAlgorithm prefixForwardingAlgorithm_;
  // Use spark measured RTT to neighbor as link metric
  bool useRttMetric_{false};
  // link flap back offs
  std::chrono::milliseconds linkflapInitBackoff_;
  std::chrono::milliseconds linkflapMaxBackoff_;
  // TTL for a key in the key value store
  std::chrono::milliseconds ttlKeyInKvStore_;

  std::unordered_map<std::string, AreaConfiguration> const areas_;
  // Send update requests to KvStore via queue
  bool enableKvStoreRequestQueue_{false};

  //
  // Mutable state
  //

  // flag to indicate whether it's running in mock mode or not
  bool mockMode_{false};

  // LinkMonitor config attributes
  thrift::LinkMonitorState state_;

  // Queue to publish interface updates to fib/spark
  messaging::ReplicateQueue<InterfaceDatabase>& interfaceUpdatesQueue_;

  // Queue to publish prefix updates to PrefixManager
  messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue_;

  // Queue to publish peer updates to KvStore
  messaging::ReplicateQueue<PeerEvent>& peerUpdatesQueue_;

  // Queue to publish the event log
  messaging::ReplicateQueue<LogSample>& logSampleQueue_;

  // Queue to send key-value udpate requests to KvStore
  messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue_;

  // ser/deser binary data for transmission
  apache::thrift::CompactSerializer serializer_;

  // currently active adjacencies
  // an adjacency is uniquely identified by interface and remote node
  // there can be multiple interfaces to a remote node, but at most 1 interface
  // (we use the "min" interface) for tcp connection
  std::unordered_map<AdjacencyKey, AdjacencyValue> adjacencies_;

  // Previously announced KvStore peers
  std::unordered_map<
      std::string /* area */,
      std::unordered_map<std::string /* node name */, KvStorePeerValue>>
      peers_;

  // all interfaces states, including DOWN one
  // Keyed by interface Name
  std::unordered_map<std::string, InterfaceEntry> interfaces_;

  // Container storing map of advertised prefixes - Map<prefix, list<area>>
  std::map<folly::CIDRNetwork, std::vector<std::string>> advertisedPrefixes_;

  // Cache of interface index to name. Used for resolving ifIndex
  // on address events
  std::unordered_map<int64_t, std::string> ifIndexToName_;

  // Throttled versions of "advertise<>" functions. It batches
  // up multiple calls and send them in one go!
  std::unique_ptr<AsyncThrottle> advertiseAdjacenciesThrottled_;
  std::unique_ptr<AsyncThrottle> advertiseIfaceAddrThrottled_;

  // Timer for processing interfaces which are in backoff states
  std::unique_ptr<folly::AsyncTimeout> advertiseIfaceAddrTimer_;

  // Exp backoff for resyncing InterfaceDb from netlink
  ExponentialBackoff<std::chrono::milliseconds> expBackoff_;

  // [TO BE DEPRECATED]
  // client to interact with KvStore
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_;

  // Raw ptr to interact with ConfigStore
  PersistentStore* configStore_{nullptr};

  // Raw ptr to interact with NetlinkProtocolSocket
  fbnl::NetlinkProtocolSocket* nlSock_{nullptr};

  // Timer for initial hold time expiry
  std::unique_ptr<folly::AsyncTimeout> adjHoldTimer_;

  // Boolean flag indicating whether initial neighbors are received in OpenR
  // initialization procedure.
  bool initialNeighborsReceived_{false};

  // Stop signal for fiber to periodically dump interface info from platform
  folly::fibers::Baton syncInterfaceStopSignal_;
}; // LinkMonitor

} // namespace openr
