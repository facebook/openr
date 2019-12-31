/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqThrottle.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/CppAttributes.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <glog/logging.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/allocators/RangeAllocator.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config-store/PersistentStoreClient.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/SystemService.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/link-monitor/InterfaceEntry.h>
#include <openr/platform/PlatformPublisher.h>
#include <openr/prefix-manager/PrefixManagerClient.h>
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
      thrift::PeerSpec spec,
      thrift::Adjacency adj,
      bool restarting = false,
      std::string areaId = openr::thrift::KvStore_constants::kDefaultArea())
      : peerSpec(spec),
        adjacency(adj),
        isRestarting(restarting),
        area(areaId) {}
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
      //
      // Immutable state initializers
      //
      // the zmq context to use for IO
      fbzmq::Context& zmqContext,
      // the id of the node running link monitor
      std::string nodeId,
      int32_t platformThriftPort,
      // for kvstore client
      KvStoreLocalCmdUrl kvStoreLocalCmdUrl,
      KvStoreLocalPubUrl kvStoreLocalPubUrl,
      // interface names to monitor
      std::unique_ptr<re2::RE2::Set> includeRegexList,
      // interface names to exclude
      std::unique_ptr<re2::RE2::Set> excludeRegexList,
      // interface names to advertise their addresses
      std::unique_ptr<re2::RE2::Set> redistRegexList,
      // static list of prefixes to announce
      std::vector<thrift::IpPrefix> const& staticPrefixes,
      // measure and use RTT of adjacencies for link
      // metrics
      bool useRttMetric,
      // enable convergence performance measurement for Adjacencies update
      bool enablePerfMeasurement,
      // is v4 enabled or not
      bool enableV4,
      // enable segment routing
      bool enableSegmentRouting,
      // prefix forwarding type MPLS
      bool forwardingTypeMpls,
      // prefix algorithm type KSP2_ED
      bool forwardingAlgoKsp2Ed,
      // KvStore's adjacency object's key prefix
      AdjacencyDbMarker adjacencyDbMarker,
      // URLs for spark, kv-store and monitor
      SparkCmdUrl sparkCmdUrl,
      SparkReportUrl sparkReportUrl,
      MonitorSubmitUrl const& monitorSubmitUrl,
      PersistentStoreUrl const& configStoreUrl,
      // if set, we will assume drained if no drain state is found in the
      // persitentStore
      bool assumeDrained,
      PrefixManagerLocalCmdUrl const& prefixManagerUrl,
      // URL for platform publisher
      PlatformPublisherUrl const& platformPubUrl,
      // Link monitor's own URLs
      LinkMonitorGlobalPubUrl linkMonitorPubUrl,
      // how long to wait before initial adjacency advertisement
      std::chrono::seconds adjHoldTime,
      // link flap backoffs
      std::chrono::milliseconds flapInitalBackoff,
      std::chrono::milliseconds flapMaxBackoff,
      // ttl for a key in the keyvalue store
      std::chrono::milliseconds ttlKeyInKvStore,
      const std::unordered_set<std::string>& areas = {
          openr::thrift::KvStore_constants::kDefaultArea()});

  ~LinkMonitor() override = default;

  // set in mock mode
  // under mock mode, will report tcp://[::]:port as kvstore communication
  // URL instead of using link local address
  void
  setAsMockMode() {
    mockMode_ = true;
  };

  // create required peers <nodeName: PeerSpec> map from current adjacencies_
  static std::unordered_map<std::string, thrift::PeerSpec>
  getPeersFromAdjacencies(
      const std::unordered_map<AdjacencyKey, AdjacencyValue>& adjacencies,
      const std::string& area = thrift::KvStore_constants::kDefaultArea());

 private:
  // make no-copy
  LinkMonitor(const LinkMonitor&) = delete;
  LinkMonitor& operator=(const LinkMonitor&) = delete;

  // Initializes ZMQ sockets
  void prepare() noexcept;

  //
  // The following are used to process Spark neighbor up/down
  // events
  //

  void neighborUpEvent(
      const thrift::BinaryAddress& neighborAddrV4,
      const thrift::BinaryAddress& neighborAddrV6,
      const thrift::SparkNeighborEvent& event);

  void neighborRestartingEvent(
      const std::string& remoteNodeName,
      const std::string& ifName,
      const std::string& area);

  void neighborDownEvent(
      const std::string& remoteNodeName,
      const std::string& ifName,
      const std::string& area);

  // Used for initial interface discovery and periodic sync with system handler
  // return true if sync is successful
  bool syncInterfaces();

  // derive current peer-spec info from current adjacencies_
  // calculate delta and announce them to KvStore (peer add/remove) if any
  //
  // upPeers: a set of peers we just detected them UP.
  // this covers the case where peer restarted, but we didn't detect restarting
  // spark packet (e.g peer non-graceful-shutdown or all spark messages lost)
  // in this case, the above delta will miss these peers, advertise them
  // if peer-spec matches
  void advertiseKvStorePeers(
      const std::string& area,
      const std::unordered_map<std::string, thrift::PeerSpec>& upPeers = {});

  // advertise to all areas
  void advertiseKvStorePeers(
      const std::unordered_map<std::string, thrift::PeerSpec>& upPeers = {});

  // Advertise my adjacencies_ to the KvStore to a specific area
  void advertiseAdjacencies(const std::string& area);

  // Advertise my adjacencies_ to the KvStore to all areas
  void advertiseAdjacencies();

  // Advertise interfaces and addresses to Spark/Fib and PrefixManager
  // respectively
  void advertiseIfaceAddr();
  void advertiseInterfaces();
  void advertiseRedistAddrs();

  // get next try time, which should be the minimum remaining time among
  // all unstable (getTimeRemainingUntilRetry() > 0) interfaces.
  // return 0 if no more unstable interface
  std::chrono::milliseconds getRetryTimeOnUnstableInterfaces();

  // Get or create InterfaceEntry object. Returns nullptr if ifName doesn't
  // qualify regex match
  InterfaceEntry* FOLLY_NULLABLE
  getOrCreateInterfaceEntry(const std::string& ifName);

  // Utility function to create thrift client connection to NetlinkSystemHandler
  // Can throw exception if it fails to open transport to client on
  // specified port.
  void createNetlinkSystemHandlerClient();

  folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsg(
      fbzmq::Message&& request) override;

  // Sumbmits the counter/stats to monitor
  void submitCounters();

  // submit events to monitor
  void logNeighborEvent(thrift::SparkNeighborEvent const& event);

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

  //
  // immutable state/invariants
  //

  // used to build the key names for this node
  const std::string nodeId_;
  // Switch agent thrift server port
  const int32_t platformThriftPort_{0};
  // used for kvStoreClient
  const std::string kvStoreLocalCmdUrl_;
  const std::string kvStoreLocalPubUrl_;
  // the interface names that match we can run on
  std::unique_ptr<re2::RE2::Set> includeRegexList_;
  // the interface names that match we can't run on
  std::unique_ptr<re2::RE2::Set> excludeRegexList_;
  // the interface names regex for advertising their global addresses
  std::unique_ptr<re2::RE2::Set> redistRegexList_;
  // static list of prefixes to announce
  const std::vector<thrift::IpPrefix> staticPrefixes_;
  // Use spark measured RTT to neighbor as link metric
  const bool useRttMetric_{true};
  // enable performance measurement
  const bool enablePerfMeasurement_{false};
  // is v4 enabled in OpenR or not
  const bool enableV4_{false};
  // enable segment routing
  const bool enableSegmentRouting_{false};
  // prefix forwarding type MPLS
  const bool forwardingTypeMpls_{false};
  // prefix algorithm type KSP2_ED_ECMP
  const bool forwardingAlgoKsp2Ed_{false};
  // used to match the adjacency database keys
  const std::string adjacencyDbMarker_;
  // used to encode interface db information in KvStore
  // URL to send/remove interfaces from spark
  const std::string sparkCmdUrl_;
  // URL for spark report socket
  const std::string sparkReportUrl_;
  // URL to receive netlink events from PlatformPublisher
  const std::string platformPubUrl_;
  // Publish our events to Fib and others
  const std::string linkMonitorGlobalPubUrl_;
  // Backoff timers
  const std::chrono::milliseconds flapInitialBackoff_;
  const std::chrono::milliseconds flapMaxBackoff_;
  // ttl for kvstore
  const std::chrono::milliseconds ttlKeyInKvStore_;
  // Timepoint used to hold off advertisement of link adjancecy on restart.
  const std::chrono::steady_clock::time_point adjHoldUntilTimePoint_;
  // The IO primitives provider; this is used for mocking
  // the IO during unit-tests.  It can be passed to other
  // functions hence shared pointer.
  const std::shared_ptr<IoProvider> ioProvider_;

  //
  // Mutable state
  //

  // flag to indicate whether it's running in mock mode or not
  // TODO: Get rid of mockMode_
  bool mockMode_{false};

  // LinkMonitor config attributes (defined in LinkMonitor.thrift)
  thrift::LinkMonitorConfig config_;

  // publish our own events (interfaces up/down)
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> linkMonitorPubSock_;
  // socket to control the spark
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> sparkCmdSock_;
  // Listen to neighbor events from spark
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT> sparkReportSock_;
  // Used to subscribe to netlink events from PlatformPublisher
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> nlEventSub_;

  // used for communicating over thrift/zmq sockets
  apache::thrift::CompactSerializer serializer_;

  // currently active adjacencies
  // an adjacency is uniquely identified by interface and remote node
  // there can be multiple interfaces to a remote node, but at most 1 interface
  // (we use the "min" interface) for tcp connection
  std::unordered_map<AdjacencyKey, AdjacencyValue> adjacencies_;

  // Previously announced KvStore peers
  std::unordered_map<
      std::string /* area */,
      std::unordered_map<std::string /* node name */, thrift::PeerSpec>>
      peers_;

  // all interfaces states, including DOWN one
  // Keyed by interface Name
  std::unordered_map<std::string, InterfaceEntry> interfaces_;

  // Throttled versions of "advertise<>" functions. It batches
  // up multiple calls and send them in one go!
  std::unique_ptr<fbzmq::ZmqThrottle> advertiseAdjacenciesThrottled_;
  std::unique_ptr<fbzmq::ZmqThrottle> advertiseIfaceAddrThrottled_;

  // Timer for processing interfaces which are in backoff states
  std::unique_ptr<fbzmq::ZmqTimeout> advertiseIfaceAddrTimer_;

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // Timer for resyncing InterfaceDb from netlink
  std::unique_ptr<fbzmq::ZmqTimeout> interfaceDbSyncTimer_;
  ExponentialBackoff<std::chrono::milliseconds> expBackoff_;

  // Timer for starting range allocator
  std::vector<std::unique_ptr<folly::AsyncTimeout>> startAllocationTimers_;

  // Timer for initial hold time expiry
  std::unique_ptr<folly::AsyncTimeout> adjHoldTimer_;

  // DS to hold local stats/counters
  fbzmq::ThreadData tData_;

  // Thrift client connection to switch SystemService, which we actually use to
  // manipulate routes.
  folly::EventBase evb_;
  std::shared_ptr<apache::thrift::async::TAsyncSocket> socket_;
  std::unique_ptr<thrift::SystemServiceAsyncClient> client_;

  // client to interact with KvStore
  std::unique_ptr<KvStoreClient> kvStoreClient_;

  // RangAlloctor to get unique nodeLabel for this node
  std::unordered_map<std::string /* area */, RangeAllocator<int32_t>>
      rangeAllocator_;

  // client to interact with ZmqMonitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // client to interact with ConfigStore
  std::unique_ptr<PersistentStoreClient> configStoreClient_;

  // client to interact with PrefixManager
  std::unique_ptr<PrefixManagerClient> prefixManagerClient_;

  // areas_ configured on this node
  std::unordered_set<std::string> areas_{};
}; // LinkMonitor

} // namespace openr
