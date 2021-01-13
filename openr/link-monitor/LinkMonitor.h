/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/CppAttributes.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <glog/logging.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/allocators/RangeAllocator.h>
#include <openr/common/AsyncThrottle.h>
#include <openr/common/OpenrEventBase.h>
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

// Pair <remoteNodeName, interface>
using AdjacencyKey = std::pair<std::string, std::string>;

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

//
// This class is responsible for reacting to neighbor
// up and down events. The reaction constitutes of starting
// a peering session on the new link and reporting the link
// as an adjacency.
//

class LinkMonitor final : public OpenrEventBase {
 public:
  LinkMonitor(
      // config
      std::shared_ptr<const Config> config,
      // raw ptr for modules
      fbnl::NetlinkProtocolSocket* nlSock,
      KvStore* kvstore,
      PersistentStore* configStore,
      // enable convergence performance measurement for Adjacencies update
      bool enablePerfMeasurement,
      // producer queue
      messaging::ReplicateQueue<InterfaceDatabase>& intfUpdatesQueue,
      messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue,
      messaging::ReplicateQueue<PeerEvent>& peerUpdatesQueue,
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      // consumer queue
      messaging::RQueue<NeighborEvent> neighborUpdatesQueue,
      messaging::RQueue<KvStoreSyncEvent> kvStoreSyncEventsQueue,
      messaging::RQueue<fbnl::NetlinkEvent> netlinkEventsQueue,
      // if set, we will assume drained if no drain state is found in the
      // persitentStore
      bool assumeDrained,
      // if set, we will override drain state from persistent store with
      // assumeDrained value
      bool overrideDrainState,
      // how long to wait before initial adjacency advertisement
      std::chrono::seconds adjHoldTime);

  ~LinkMonitor() override = default;

  /**
   * Override stop method of OpenrEventBase
   */
  void stop() override;

  // set in mock mode
  // under mock mode, will report tcp://[::]:port as kvstore communication
  // URL instead of using link local address
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
  folly::SemiFuture<folly::Unit> setNodeOverload(bool isOverloaded);
  folly::SemiFuture<folly::Unit> setInterfaceOverload(
      std::string interfaceName, bool isOverloaded);
  folly::SemiFuture<folly::Unit> setLinkMetric(
      std::string interfaceName, std::optional<int32_t> overrideMetric);
  folly::SemiFuture<folly::Unit> setAdjacencyMetric(
      std::string interfaceName,
      std::string adjNodeName,
      std::optional<int32_t> overrideMetric);
  folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>> getInterfaces();
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
  getAdjacencies(thrift::AdjacenciesFilter filter = {});
  folly::SemiFuture<InterfaceDatabase> getAllLinks();

  // create required peers <nodeName: PeerSpec> map from current adjacencies_
  static std::unordered_map<std::string, thrift::PeerSpec>
  getPeersFromAdjacencies(
      const std::unordered_map<AdjacencyKey, AdjacencyValue>& adjacencies,
      const std::string& area);

 private:
  // make no-copy
  LinkMonitor(const LinkMonitor&) = delete;
  LinkMonitor& operator=(const LinkMonitor&) = delete;

  /*
   * [Spark] neighbor event functions
   */

  // process neighbor event updates from Spark module
  void processNeighborEvent(NeighborEvent&& event);

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
   * [Netlink Platform] related functions
   */

  // process LINK/ADDR event updates from platform
  void processNetlinkEvent(fbnl::NetlinkEvent&& event);

  // Used for initial interface discovery and periodic sync with system handler
  // return true if sync is successful
  bool syncInterfaces();

  // Get or create InterfaceEntry object.
  // Returns nullptr if ifName doesn't qualify regex match
  // used in syncInterfaces() and LINK/ADDRESS EVENT
  InterfaceEntry* FOLLY_NULLABLE
  getOrCreateInterfaceEntry(const std::string& ifName);

  // call advertiseInterfaces() and advertiseRedistAddrs()
  // throttle updates if there's any unstable interface by
  // getRetryTimeOnUnstableInterfaces() time
  // used in advertiseIfaceAddrThrottled_ and advertiseIfaceAddrTimer_
  // called upon interface change in getOrCreateInterfaceEntry()
  void advertiseIfaceAddr();

  /*
   * [Kvstore] PEER UP/DOWN events sent to Kvstore over peerUpdatesQueue_
   */

  // Called upon spark neighborUp events
  // If there's only one adj for this peer, we create new KvStorePeerValue and
  // send peer add requrest to KvStore. If there are established adjs, only
  // update establishedSparkNeighbors in existing KvStorePeer struct
  void updateKvStorePeerNeighborUp(
      const std::string& area,
      const AdjacencyKey& adjId,
      const AdjacencyValue& adjVal);

  // Called upon spark neighborRestarting, neighborDown events
  // In single adj case, send KvStorePeer Delete Request.
  // In parallel adj case, update peer spec with left adj peer spec, send
  // KvStore Peer Add request if new peer spec is different.
  void updateKvStorePeerNeighborDown(
      const std::string& area,
      const AdjacencyKey& adjId,
      const AdjacencyValue& adjVal);

  /*
   * [Kvstore] Advertise my adjacencies_ (kvStoreClient_->persistKey)
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

  // get next try time, which should be the minimum remaining time among
  // all unstable (getTimeRemainingUntilRetry() > 0) interfaces.
  // return 0 if no more unstable interface
  std::chrono::milliseconds getRetryTimeOnUnstableInterfaces();

  // build AdjacencyDatabase
  thrift::AdjacencyDatabase buildAdjacencyDatabase(const std::string& area);

  // submit events to monitor
  void logNeighborEvent(NeighborEvent const& event);

  // link events
  void logLinkEvent(
      const std::string& iface,
      bool wasUp,
      bool isUp,
      std::chrono::milliseconds backoffTime);

  // peer events
  void logPeerEvent(
      const std::string& event,
      const std::string& peerName,
      const thrift::PeerSpec& peerSpec);

  // returns any(a.shouldDiscoverOnIface(iface) for a in areas_)
  bool anyAreaShouldDiscoverOnIface(std::string const& iface) const;

  // returns any(a.anyAreaShouldRedistributeIface(iface) for a in areas_)
  bool anyAreaShouldRedistributeIface(std::string const& iface) const;

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

  //
  // Mutable state
  //

  // flag to indicate whether it's running in mock mode or not
  // TODO: Get rid of mockMode_
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

  // Cache of interface index to name. Used for resolving ifIndex
  // on address events
  std::unordered_map<int64_t, std::string> ifIndexToName_;

  // Throttled versions of "advertise<>" functions. It batches
  // up multiple calls and send them in one go!
  std::unique_ptr<AsyncThrottle> advertiseAdjacenciesThrottled_;
  std::unique_ptr<AsyncThrottle> advertiseIfaceAddrThrottled_;

  // Timer for processing interfaces which are in backoff states
  std::unique_ptr<folly::AsyncTimeout> advertiseIfaceAddrTimer_;

  // Timer for resyncing InterfaceDb from netlink
  std::unique_ptr<folly::AsyncTimeout> interfaceDbSyncTimer_;
  ExponentialBackoff<std::chrono::milliseconds> expBackoff_;

  // client to interact with KvStore
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_;

  // RangAlloctor to get unique nodeLabel for this node
  std::unordered_map<std::string /* area */, RangeAllocator<int32_t>>
      rangeAllocator_;

  // Raw ptr to interact with ConfigStore
  PersistentStore* configStore_{nullptr};

  // Raw ptr to interact with NetlinkProtocolSocket
  fbnl::NetlinkProtocolSocket* nlSock_{nullptr};

  // Timer for starting range allocator
  // this is equal to adjHoldTimer_
  // because we'll delay range allocation until we have formed all of our
  // adjcencies
  std::vector<std::unique_ptr<folly::AsyncTimeout>> startAllocationTimers_;

  // Timer for initial hold time expiry
  std::unique_ptr<folly::AsyncTimeout> adjHoldTimer_;
}; // LinkMonitor

} // namespace openr
