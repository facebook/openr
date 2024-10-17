/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncTimeout.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Types.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/link-monitor/AdjacencyEntry.h>
#include <openr/link-monitor/InterfaceEntry.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>
#include <openr/nl/NetlinkProtocolSocket.h>

namespace openr {

// KvStore Peer Value
struct KvStorePeerValue {
  // Current established KvStorePeer Spec, this usually is taken from the first
  // established Spark Neighbor
  thrift::PeerSpec tPeerSpec;

  // Established spark neighbors related to remoteNodeName. KvStore Peer State
  // Machine is a continuation of Spark Neighbor State Machine, tracking spark
  // neighbors here help us decide when to send ADD/DEL KvStorePeer request.
  //
  // ATTN: this is different from adjacencies_ collection!
  //
  // An adjancency remains UP when remote spark neighbor performs a graceful
  // restart(GR). However, neighbor is not in ESTABLISHED state during GR. Here
  // we only track spark neighbors in ESTABLISHED state.
  std::unordered_set<AdjacencyKey> establishedSparkNeighbors;

  KvStorePeerValue(
      thrift::PeerSpec tPeerSpec,
      std::unordered_set<AdjacencyKey> establishedSparkNeighbors)
      : tPeerSpec(std::move(tPeerSpec)),
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
      PersistentStore* configStore,
      // producer queue
      messaging::ReplicateQueue<InterfaceDatabase>& interfaceUpdatesQueue,
      messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue,
      messaging::ReplicateQueue<PeerEvent>& peerUpdatesQueue,
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue,
      // consumer queue
      messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue,
      messaging::RQueue<NeighborInitEvent> neighborUpdatesQueue,
      messaging::RQueue<fbnl::NetlinkEvent> netlinkEventsQueue);

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
   * [Public API][Hard-Drain]:
   *
   * - Set/unset node overload;
   * - Set/unset interface overload;
   */
  folly::SemiFuture<folly::Unit> semifuture_setNodeOverload(bool isOverloaded);
  folly::SemiFuture<folly::Unit> semifuture_setInterfaceOverload(
      std::string interfaceName, bool isOverloaded);

  // [TO BE DEPRECATED]
  folly::SemiFuture<folly::Unit> semifuture_setLinkMetric(
      std::string interfaceName, std::optional<int32_t> overrideMetric);

  /*
   * [Public API][Soft-Drain]:
   *
   * NOTE: all requests will be Throttled;
   *
   * - Set/unset node-level metric increment;
   * - Set/unset interface-level metric increment;
   */
  folly::SemiFuture<folly::Unit> semifuture_setNodeInterfaceMetricIncrement(
      int32_t metricIncrementVal);
  folly::SemiFuture<folly::Unit> semifuture_unsetNodeInterfaceMetricIncrement();
  folly::SemiFuture<folly::Unit> semifuture_setInterfaceMetricIncrement(
      std::string interfaceName, int32_t metricIncrementVal);
  folly::SemiFuture<folly::Unit> semifuture_unsetInterfaceMetricIncrement(
      std::string interfaceName);
  folly::SemiFuture<folly::Unit> semifuture_setInterfaceMetricIncrementMulti(
      std::vector<std::string> interfaceNames, int32_t metricIncrementVal);
  folly::SemiFuture<folly::Unit> semifuture_unsetInterfaceMetricIncrementMulti(
      std::vector<std::string> interfaceNames);

  /*
   * [Public API][Adjacency Metric Override]
   *
   * NOTE: this will have the highest priority to OVERRIDE adjacency metric
   */
  folly::SemiFuture<folly::Unit> semifuture_setAdjacencyMetric(
      std::string interfaceName,
      std::string adjNodeName,
      std::optional<int32_t> overrideMetric);

  /*
   * [Public API][Link/Adjacency Dump]
   *
   * - Dump interface/link information
   * - Dump links information from netlinkProtocolSocket
   * - Dump adjacency database information
   */
  folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
  semifuture_getInterfaces();
  folly::SemiFuture<InterfaceDatabase> semifuture_getAllLinks();
  folly::SemiFuture<std::unique_ptr<
      std::map<std::string, std::vector<thrift::AdjacencyDatabase>>>>
  semifuture_getAreaAdjacencies(thrift::AdjacenciesFilter filter = {});

  /*
   * DEPRECATED. Perfer semifuture_getAreaAdjacencies to return the
   * areas as well.
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
  semifuture_getAdjacencies(thrift::AdjacenciesFilter filter = {});

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
  void neighborUpEvent(const NeighborEvent& event, bool isGracefulRestart);
  void neighborAdjSyncedEvent(const NeighborEvent& event);
  void neighborRestartingEvent(const NeighborEvent& event);
  void neighborDownEvent(const NeighborEvent& event);
  void neighborRttChangeEvent(const NeighborEvent& event);

  /*
   * [Public Api Helper]
   */
  void setInterfaceMetricIncrementHelper(
      folly::Promise<folly::Unit>& p,
      std::vector<std::string> interfaces,
      int32_t metrics);

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
      const AdjacencyKey& adjKey,
      const thrift::PeerSpec& spec);

  void updateKvStorePeerNeighborDown(
      const std::string& area,
      const AdjacencyKey& adjKey,
      const thrift::PeerSpec& spec);

  /*
   * [Kvstore] Advertise my adjacencies_
   *
   * Called upon spark neighbor events: up/down/rtt (restarting does not trigger
   * adj update)
   */
  void advertiseAdjacencies(const std::string& area);
  void advertiseAdjacencies(); // Advertise my adjacencies_ in to all areas
  void scheduleAdvertiseAdjAllArea();
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

  // Total # of adjacencies stored.
  size_t getTotalAdjacencies();

  void updateLinkStatusRecords(
      const std::string& ifName,
      thrift::LinkStatusEnum ifStatus,
      int64_t ifStatusChangeTimestamp);

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

  //
  // immutable state/invariants
  //

  // used to build the key names for this node
  const std::string nodeId_;
  // enable performance measurement
  const bool enablePerfMeasurement_{false};
  // keep track of current status of all links in router
  // with timestamps at when they change their status.
  const bool enableLinkStatusMeasurement_{false};
  // enable v4
  bool enableV4_{false};
  // [TO_BE_DEPRECATED] prefix forwarding type and algorithm
  thrift::PrefixForwardingType prefixForwardingType_;
  thrift::PrefixForwardingAlgorithm prefixForwardingAlgorithm_;
  // Use spark measured RTT to neighbor as link metric
  bool useRttMetric_{false};
  // link flap back offs
  std::chrono::milliseconds linkflapInitBackoff_;
  std::chrono::milliseconds linkflapMaxBackoff_;

  /**
   * Flag to indicate if KVSTORE_SYNCED signal should be used
   * to advertise adjacency to kvstore.
   * By default this flag is set to false and adjacecies will
   * be advertised as soon KVSTORE_SYNCED signal received. If
   * KVSTORE_SYNCED signal is not to be used (kept purley for
   * backward compatibility), this flag can be set by tuning
   * link monitor configuration.
   */
  bool enableInitOptimization_{false};
  std::unordered_map<std::string, AreaConfiguration> const areas_;

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

  // Currently active adjacencies.
  // An adjacency is uniquely identified by interface and remote node within an
  // area.
  // There can be multiple interfaces to a remote node, but at most 1 interface
  // (we use the "min" interface) for tcp connection.
  std::unordered_map<
      std::string /* area */,
      std::unordered_map<AdjacencyKey, AdjacencyEntry>>
      adjacencies_;

  // Previously announced KvStore peers
  std::unordered_map<
      std::string /* area */,
      std::unordered_map<std::string /* node name */, KvStorePeerValue>>
      peers_;

  // all interfaces states, including DOWN one
  // Keyed by interface Name
  std::unordered_map<std::string /* interface name */, InterfaceEntry>
      interfaces_;

  // all links with their status and timestamp at when they change status
  thrift::LinkStatusRecords linkStatusRecords_;

  // Container storing map of advertised prefixes - Map<prefix, list<area>>
  std::map<folly::CIDRNetwork, std::vector<std::string>> advertisedPrefixes_;

  // Cache of interface index to name. Used for resolving ifIndex
  // on address events
  std::unordered_map<int64_t, std::string> ifIndexToName_;

  // Throttled versions of "advertise<>" functions. It batches
  // up multiple calls and send them in one go!

  // Advertise Adj needs per area throttle as KvStore calls can interrupt
  // and cause race conditions, some batched call may otherwise be lost
  std::unordered_map<std::string /* area */, std::unique_ptr<AsyncThrottle>>
      advertiseAdjacenciesThrottledPerArea_;
  std::unique_ptr<AsyncThrottle> advertiseIfaceAddrThrottled_;

  // Timer for processing interfaces which are in backoff states
  std::unique_ptr<folly::AsyncTimeout> advertiseIfaceAddrTimer_;

  // Exp backoff for resyncing InterfaceDb from netlink
  ExponentialBackoff<std::chrono::milliseconds> expBackoff_;

  // Raw ptr to interact with ConfigStore
  PersistentStore* configStore_{nullptr};

  // Raw ptr to interact with NetlinkProtocolSocket
  fbnl::NetlinkProtocolSocket* nlSock_{nullptr};

  // Timer for initial hold time expiry
  std::unique_ptr<folly::AsyncTimeout> adjHoldTimer_;

  // Boolean flag indicating whether initial neighbors are received in OpenR
  // initialization procedure.
  bool initialNeighborsReceived_{false};

  // Boolean flag indicating whether initial links are discovered during Open/R
  // initialization procedure
  bool initialLinksDiscovered_{false};

  // Stop signal for fiber to periodically dump interface info from platform
  folly::fibers::Baton syncInterfaceStopSignal_;
}; // LinkMonitor

} // namespace openr
