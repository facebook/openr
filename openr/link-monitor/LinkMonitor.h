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
#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqThrottle.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/io/async/EventBase.h>
#include <glog/logging.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/allocators/RangeAllocator.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config-store/PersistentStoreClient.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/SystemService.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/platform/PlatformPublisher.h>
#include <openr/prefix-manager/PrefixManagerClient.h>
#include <openr/spark/Spark.h>

namespace openr {

//
// This class is responsible for reacting to neighbor
// up and down events. The reaction constitutes of starting
// a peering session on the new link and reporting the link
// as an adjacency.
//

class LinkMonitor final : public fbzmq::ZmqEventLoop {
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
      // enable full mesh reduction
      bool enableFullMeshReduction,
      // enable convergence performance measurement for Adjacencies update
      bool enablePerfMeasurement,
      // is v4 enabled or not
      bool enableV4,
      // enable interface db
      bool advertiseInterfaceDb,
      // enable segment routing
      bool enableSegmentRouting,
      // KvStore's adjacency object's key prefix
      AdjacencyDbMarker adjacencyDbMarker,
      InterfaceDbMarker interfaceDbMarker,
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
      LinkMonitorGlobalCmdUrl linkMonitorCmdUrl,
      //
      // Mutable state initializers
      //
      // how long to wait before initial adjacency advertisement
      std::chrono::seconds adjHoldTime,
      // link flap backoffs
      std::chrono::milliseconds flapInitalBackoff,
      std::chrono::milliseconds flapMaxBackoff);

  ~LinkMonitor() override = default;

  // set in mock mode
  // under mock mode, will report tcp://[::]:port as kvstore communication
  // URL instead of using link local address
  void
  setAsMockMode() {
    mockMode_ = true;
  };

  // calculate peers to be deleted/added from a given oldPeers and newPeers map
  // we don't really need to worry about updated peers because LM will only
  // handle neighbor up/down event, if a peer spec ever gets changed, it will
  // be a down event followed up by a up event.
  static void getPeerDifference(
      const std::unordered_map<std::string, thrift::PeerSpec>& oldPeers,
      const std::unordered_map<std::string, thrift::PeerSpec>& newPeers,
      std::vector<std::string>& toDelPeers,
      std::unordered_map<std::string, thrift::PeerSpec>& toAddPeers);

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

  void neighborDownEvent(
      const std::string& remoteNodeName, const std::string& ifName);

  // Used for initial interface discovery and periodic sync
  // return true if sync is successful
  bool syncInterfaces();

  // Advertise my adjacencies to the KvStore's socket
  void advertiseMyAdjacencies();

  // Invoked when throttle timer expires to aggregate all newly
  // added/updated peers
  void processPendingPeerAddRequests();

  // create required peers <nodeName: PeerSpec> map from current adjacencies_
  // output peers will be different if FullMeshReduction is enabled.
  // e.g if I'm the leader, peer all neighbors;
  //     otherwise, peer with leader only;
  std::unordered_map<std::string, thrift::PeerSpec> getPeersFromAdjacencies();

  // create peers <nodeName : PeerSpec> map for all my neighbors
  std::unordered_map<std::string, thrift::PeerSpec> getPeersForAllNeighbors();

  // handle peer changes e.g remove/add peers if any
  void handlePeerChanges(
      const std::unordered_map<std::string, thrift::PeerSpec>& oldPeers,
      const std::unordered_map<std::string, thrift::PeerSpec>& newPeers);

  // Advertise all adjacencies changes in throttled and asynchronous
  // fashion. Prefer to use this instead of `advertiseMyAdjacencies`
  std::unique_ptr<fbzmq::ZmqThrottle> advertiseMyAdjacenciesThrottled_;

  // Helper function to check if there's any link/addr update between
  // incoming netlink event and local database
  bool updateLinkEvent(const thrift::LinkEntry& linkEntry);
  bool updateAddrEvent(const thrift::AddrEntry& addrEntry);
  // process an interface going up or down - we inform spark
  // of the new connection and it starts multicasting on new interface
  // attempting to discover a neighbor
  void processLinkEvent(const thrift::LinkEntry& linkEntry);
  void processAddrEvent(const thrift::AddrEntry& addrEntry);
  thrift::InterfaceDatabase createInterfaceDatabase();
  void sendInterfaceDatabase();

  // Helper functions to process redistribute addresses
  void addDelRedistAddr(
      const std::string& ifName, bool isValid, const thrift::IpPrefix& prefix);
  void advertiseRedistAddrs();

  // sendIfDbtimer callback, send all stable (backoff.canTryNow() == True)
  // interfaces to spark, mark unstable interfaces as DOWN before
  // sending out to spark
  void sendIfDbCallback();

  // Utility function to create thrift client connection to NetlinkSystemHandler
  // Can throw exception if it fails to open transport to client on
  // specified port.
  void createNetlinkSystemHandlerClient();

  // get next try time, which should be the minimum remaining time among
  // all unstable (getTimeRemainingUntilRetry() > 0) interfaces.
  // return 0 if no more unstable interface
  std::chrono::milliseconds getRetryTimeOnUnstableInterfaces();

  // process link update event
  // double up ifName's backoff, and re-schedule timer based on
  // minimum remaining retry
  void processLinkUpdatedEvent(const std::string& ifName, bool isUp);

  // process any command we may receive on cmd socket
  void processCommand();

  // Sumbmits the counter/stats to monitor
  void submitCounters();

  // helper to check if ifName has a prefix match in redistIfNames_
  bool checkRedistIfNameRegex(const std::string& ifName);

  // submit events to monitor
  void logEvent(
      const std::string& event,
      const std::string& neighbor,
      const std::string& iface,
      const std::string& remoteIface);

  // link events
  void logLinkEvent(const std::string& event, const std::string& iface);
  // peer events
  void logPeerEvent(
      const std::string& event, const std::vector<std::string>& peers);

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
  // enable full mesh reduction to reduce duplicate flooding
  const bool enableFullMeshReduction_{false};
  // enable performance measurement
  const bool enablePerfMeasurement_{false};
  // is v4 enabled in OpenR or not
  const bool enableV4_{false};
  // advertise interface DB or not
  const bool advertiseInterfaceDb_{false};
  // enable segment routing
  const bool enableSegmentRouting_{false};
  // used to match the adjacency database keys
  const std::string adjacencyDbMarker_;
  // used to encode interface database key names
  const std::string interfaceDbMarker_;
  // used to encode interface db information in KvStore
  // URL to send/remove interfaces from spark
  const std::string sparkCmdUrl_;
  // URL for spark report socket
  const std::string sparkReportUrl_;
  // URL to receive netlink events from PlatformPublisher
  const std::string platformPubUrl_;
  // Publish our events to Fib and others
  const std::string linkMonitorGlobalPubUrl_;
  // URL to receive commands
  const std::string linkMonitorGlobalCmdUrl_;
  // The IO primitives provider; this is used for mocking
  // the IO during unit-tests.  It can be passed to other
  // functions hence shared pointer.
  const std::shared_ptr<IoProvider> ioProvider_;

  // KvStore client for setting adjacency key to KvStore
  // can be shared, e.g., to gmock expetation call
  std::unique_ptr<KvStoreClient> kvStoreClient_;

  // Thrift client connection to switch SystemService, which we actually use to
  // manipulate routes.
  folly::EventBase evb_;
  std::shared_ptr<apache::thrift::async::TAsyncSocket> socket_;
  std::unique_ptr<thrift::SystemServiceAsyncClient> client_;

  //
  // Mutable state
  //

  // flag to indicate whether it's running in mock mode or not
  bool mockMode_{false};
  std::chrono::milliseconds flapInitialBackoff_;
  std::chrono::milliseconds flapMaxBackoff_;

  // LinkMonitor config attributes (defined in LinkMonitor.thrift)
  thrift::LinkMonitorConfig config_;

  // publish our own events (interfaces up/down)
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> linkMonitorPubSock_;
  // the socket we use to respond to commands
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> linkMonitorCmdSock_;
  // socket to control the spark
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> sparkCmdSock_;
  // Listen to neighbor events from spark
  fbzmq::Socket<ZMQ_PAIR, fbzmq::ZMQ_CLIENT> sparkReportSock_;
  // Used to subscribe to netlink events from PlatformPublisher
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> nlEventSub_;

  // RangAlloctor to get unique nodeLabel for this node
  std::unique_ptr<RangeAllocator<int32_t>> rangeAllocator_;

  // used for communicating over thrift/zmq sockets
  apache::thrift::CompactSerializer serializer_;

  //
  // local state used for link UP debouncing
  //

  // pending requests for adding new adjacencies
  std::unordered_map<
      std::pair<std::string /* remoteNodeName */, std::string /* interface */>,
      std::pair<thrift::PeerSpec, thrift::Adjacency>>
      peerAddRequests_;

  // currently active adjacencies
  // an adjacency is uniquely identified by interface and remote node
  // there can be multiple interfaces to a remote node, but at most 1 interface
  // (we use the "min" interface) for tcp connection
  std::unordered_map<
      std::pair<std::string /* remoteNodeName */, std::string /* interface */>,
      std::pair<thrift::PeerSpec, thrift::Adjacency>>
      adjacencies_;

  // currently UP interfaces to each remote node
  // use ordered set to find the min interface, instead of min heap, to prevent
  // duplicate interfaces, which can occur, e.g., a neighbor restarts after
  // reporting up but with no down event
  std::unordered_map<
      std::string /* remoteNodeName */,
      std::set<std::string /* interface */>>
      nbIfs_;

  // Interface Entry
  // We hold interface information (isUp and addresses) in this object
  // We can create objects with both status and addresses together or
  // only from link status information
  //
  // Piecemeal updates in response to link event or address events
  // are supported
  // We assume address events can never arrive before a link event
  //
  // Interface must always be sent on creation
  // and on updates (link or address)
  //
  // Multicast route addition should be done explicitly when link is up

  class InterfaceEntry final {
   public:
    InterfaceEntry() = default;
    ~InterfaceEntry() = default;

    InterfaceEntry(const InterfaceEntry&) = default;
    InterfaceEntry(InterfaceEntry&&) = default;

    InterfaceEntry& operator=(const InterfaceEntry&) = default;
    InterfaceEntry& operator=(InterfaceEntry&&) = default;

    // Creating entries when we have all link information
    InterfaceEntry(
        int ifIndex,
        bool isUp,
        uint64_t weight,
        const std::unordered_set<folly::CIDRNetwork>& networks)
        : ifIndex_(ifIndex),
          isUp_(isUp),
          weight_(weight),
          networks_(networks) {}

    // Creating entries only from link status information
    InterfaceEntry(int ifIndex, bool isUp) : ifIndex_(ifIndex), isUp_(isUp) {}

    // Update methods for link and address events
    bool
    updateEntry(int ifIndex, bool isUp, uint64_t weight) {
      bool isUpdated = false;
      isUpdated |= std::exchange(ifIndex_, ifIndex) != ifIndex;
      isUpdated |= std::exchange(isUp_, isUp) != isUp;
      isUpdated |= std::exchange(weight_, weight) != weight;
      return isUpdated;
    }

    bool
    updateEntry(const folly::CIDRNetwork& ipNetwork, bool isValid) {
      bool isUpdated = false;
      if (isValid) {
        isUpdated |= (networks_.insert(ipNetwork)).second;
      } else {
        isUpdated |= (networks_.erase(ipNetwork) == 1);
      }
      return isUpdated;
    }

    // Used to check for updates if doing a re-sync
    bool
    operator==(const InterfaceEntry& interfaceEntry) {
      return (
          (ifIndex_ == interfaceEntry.getIfIndex()) &&
          (isUp_ == interfaceEntry.isUp()) &&
          (networks_ == interfaceEntry.getNetworks()) &&
          (weight_ == interfaceEntry.getWeight()));
    }

    friend std::ostream&
    operator<<(std::ostream& out, const InterfaceEntry& interfaceEntry) {
      out << "Interface data: " << (interfaceEntry.isUp() ? "UP" : "DOWN")
          << " ifIndex: " << interfaceEntry.getIfIndex()
          << " weight: " << interfaceEntry.getWeight() << " IPv6ll: "
          << folly::join(", ", interfaceEntry.getV6LinkLocalAddrs())
          << " IPv4: " << folly::join(", ", interfaceEntry.getV4Addrs());
      return out;
    }

    bool
    isUp() const {
      return isUp_;
    }
    int
    getIfIndex() const {
      return ifIndex_;
    }
    uint64_t
    getWeight() const {
      return weight_;
    }

    // returns const references for optimization
    const std::unordered_set<folly::CIDRNetwork>&
    getNetworks() const {
      return networks_;
    }

    std::unordered_set<folly::IPAddress>
    getV4Addrs() const {
      std::unordered_set<folly::IPAddress> v4Addrs;
      for (auto const& ntwk : networks_) {
        if (ntwk.first.isV4()) {
          v4Addrs.insert(ntwk.first);
        }
      }
      return v4Addrs;
    }
    std::unordered_set<folly::IPAddress>
    getV6LinkLocalAddrs() const {
      std::unordered_set<folly::IPAddress> v6Addrs;
      for (auto const& ntwk : networks_) {
        if (ntwk.first.isV6() && ntwk.first.isLinkLocal()) {
            v6Addrs.insert(ntwk.first);
        }
      }
      return v6Addrs;
    }

    // Create the Interface info for Interface request
    thrift::InterfaceInfo getInterfaceInfo() const;

   private:
    int ifIndex_{0};
    bool isUp_{false};
    uint64_t weight_{1};

    // We keep the set of IPs and push to Spark
    // Spark really cares about one, but we let
    // Spark handle that
    std::unordered_set<folly::CIDRNetwork> networks_;
  };

  // all interfaces states, including DOWN one
  // Keyed by interface Name
  std::unordered_map<std::string, InterfaceEntry> interfaceDb_;

  // list of all prefixes per redistribute interface
  std::unordered_map<std::string, std::unordered_set<thrift::IpPrefix>>
      redistAddrs_;

  // interface name to pair <last flap timestamp, backoff> mapping
  // interface with backoff.canTryNow() will be
  // backoff gets penalized if last timestamp interface was flapped is within
  // penalty threshold
  std::unordered_map<
      std::string,
      std::pair<folly::Optional<std::chrono::steady_clock::time_point>,
                ExponentialBackoff<std::chrono::milliseconds>>>
    linkBackoffs_;

  // Timer for scheduling when to send interfaceDb to spark
  std::unique_ptr<fbzmq::ZmqTimeout> sendIfDbTimer_;

  // Timepoint used to hold off advertisement of link adjancecy on restart.
  std::chrono::steady_clock::time_point adjHoldUntilTimePoint_;
  bool advertiseAdj_{false};

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // Timer for resyncing InterfaceDb from netlink
  std::unique_ptr<fbzmq::ZmqTimeout> interfaceDbSyncTimer_;
  ExponentialBackoff<std::chrono::milliseconds> expBackoff_;

  // DS to hold local stats/counters
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // client to interact with ConfigStore
  std::unique_ptr<PersistentStoreClient> configStoreClient_;

  // client to prefix manager
  std::unique_ptr<PrefixManagerClient> prefixManagerClient_;
}; // LinkMonitor

} // namespace openr
