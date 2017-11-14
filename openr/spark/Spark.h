/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <functional>

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Optional.h>
#include <folly/SocketAddress.h>
#include <folly/container/EvictingCacheMap.h>
#include <folly/stats/BucketedTimeSeries.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/KnownKeysStore.h>
#include <openr/common/StepDetector.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Spark_types.h>
#include <openr/spark/IoProvider.h>

namespace openr {

enum class PacketValidationResult {
  SUCCESS = 1,
  FAILURE = 2,
  NEIGHBOR_RESTART = 3,
};

//
// Spark is responsible of telling our peer of our existence
// and also tracking the neighbor liveness. It publishes the
// neighbor state changes to a single downstream consumer
// via a PAIR socket.
//
// It receives commands in form of "add interface" / "remove inteface"
// and starts hello process on those interfaces.
//

class Spark final : public fbzmq::ZmqEventLoop {
 public:
  Spark(
      std::string const& myDomainName,
      std::string const& myNodeName,
      uint16_t const udpMcastPort,
      std::chrono::milliseconds myHoldTime,
      std::chrono::milliseconds myKeepAliveTime,
      std::chrono::milliseconds fastInitKeepAliveTime,
      folly::Optional<int> ipTos,
      KeyPair keyPair,
      KnownKeysStore* knownKeysStore,
      bool enableV4,
      bool enableSignature,
      SparkReportUrl const& reportUrl,
      SparkCmdUrl const& cmdUrl,
      MonitorSubmitUrl const& monitorSubmitUrl,
      KvStorePubPort kvStorePubPort,
      KvStoreCmdPort kvStoreCmdPort,
      fbzmq::Context& zmqContext);

  ~Spark() override = default;

  // set the mocked IO provider, used for unit-testing
  void
  setIoProvider(std::shared_ptr<IoProvider> ioProvider) {
    ioProvider_ = std::move(ioProvider);
  }

  // set key pair, used for unit-testing
  void
  setKeyPair(const KeyPair& keyPair) {
    // can only be called when stopped or from the same thread
    CHECK(isInEventLoop());
    keyPair_ = keyPair;
  }

  //
  // Crypto methods. We put them here for unit-testing.
  //

  // sign a given message
  static std::string signMessage(
      std::string const& msg, std::string const& privateKey);

  // verify a message signature
  static bool validateSignature(
      std::string const& signature,
      std::string const& msg,
      std::string const& publicKey);

 private:
  // Spark is non-copyable
  Spark(Spark const&) = delete;
  Spark& operator=(Spark const&) = delete;

  // Initializes ZMQ sockets
  void prepare(folly::Optional<int> maybeIpTos) noexcept;

  // check neighbor's hello packet; return true if packet is valid and
  // passed the following checks:
  //
  // (1) neighbor is not self (packet not looped back)
  // (2) authentication if keys are already known before
  // (3) signature is good
  // (4) validate hello packet sequence number. detects neighbor restart if
  //     sequence number gets wrapped up again.
  PacketValidationResult validateHelloPacket(
      std::string const& ifName, thrift::SparkHelloPacket const& helloPacket);

  // invoked when a neighbor's rtt changes
  void processNeighborRttChange(
      std::string const& ifName,
      thrift::SparkNeighbor const& originator,
      int64_t const newRtt);

  // Invoked when a neighbor's hold timer is expired. We remove the neighbor
  // from our tracking list.
  void processNeighborHoldTimeout(
      std::string const& ifName, std::string const& neighborName);

  // Determine if we should process the next packte from this ifName, addr pair
  bool shouldProcessHelloPacket(
      std::string const& ifName, folly::IPAddress const& addr);

  // process hello packet from a neighbor. we want to see if
  // the neighbor could be added as adjacent peer.
  void processHelloPacket();

  // originate my hello packet on given interface
  void sendHelloPacket(std::string const& ifName, bool inFastInitState = false);

  // process interfaceDb update from LinkMonitor
  // iface add/remove , join/leave iface for UDP mcasting
  void processInterfaceDbUpdate();

  // find an interface name in the interfaceDb given an ifIndex
  folly::Optional<std::string> findInterfaceFromIfindex(int ifIndex);

  // Utility function to generate a new label for neighbor on given interface.
  // If there is only one neighbor per interface then labels are expected to be
  // same across process-restarts
  int32_t getNewLabelForIface(std::string const& ifName);

  // Sumbmits the counter/stats to monitor
  void submitCounters();

  //
  // Private state
  //

  // This node's domain name
  const std::string myDomainName_{};

  // this node's name
  const std::string myNodeName_{};

  // UDP port for send/recv of spark hello messages
  const uint16_t udpMcastPort_{6666};

  // the hold time to announce on all interfaces. Can't be less than 3s
  const std::chrono::milliseconds myHoldTime_{0};

  // hello message (keepAlive) exchange interval. Must be less than holdtime
  // and greater than 0
  const std::chrono::milliseconds myKeepAliveTime_{0};

  // hello message exchange interval during fast init state, much faster than
  // usual keep alive interval
  const std::chrono::milliseconds fastInitKeepAliveTime_{0};

  // the ECC key pair to be used with libsodium for signing
  KeyPair keyPair_{};

  // known public keys to authenticate peers
  // authenticate is disabled if set to nullptr, e.g., for testing
  const KnownKeysStore* knownKeysStore_{nullptr};

  // This flag indicates that we will also exchange v4 transportAddress in
  // Spark HelloMessage
  const bool enableV4_{false};

  // If enabled, then all spark hello packets will be signed with ECC
  // signatures. This can consume too much CPU out of your box if there
  // are hundreds of interfaces
  const bool enableSignature_{true};

  // the next sequence number to be used on any interface for outgoing hellos
  // NOTE: we increment this on hello sent out of any interfaces
  uint64_t mySeqNum_{1};

  // the multicast socket we use
  int mcastFd_{-1};

  //
  // zmq sockets section
  //

  // this is used to communicate events to downstream consumer
  const std::string reportUrl_{""};
  fbzmq::Socket<ZMQ_PAIR, fbzmq::ZMQ_SERVER> reportSocket_;

  // this is used to add/remove network interfaces for tracking
  const std::string cmdUrl_{""};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> cmdSocket_;

  // this is used to inform peers about my kvstore tcp ports
  const uint16_t kKvStorePubPort_;
  const uint16_t kKvStoreCmdPort_;

  //
  // Interface tracking
  //
  class Interface {
   public:
    Interface(
        int ifIndex,
        const folly::IPAddressV4& v4Addr,
        const folly::IPAddressV6& v6LinkLocalAddr)
        : ifIndex(ifIndex), v4Addr(v4Addr), v6LinkLocalAddr(v6LinkLocalAddr) {}

    bool
    operator==(const Interface& interface) const {
      return (
          (ifIndex == interface.ifIndex) && (v4Addr == interface.v4Addr) &&
          (v6LinkLocalAddr == interface.v6LinkLocalAddr));
    }

    int ifIndex{0};
    folly::IPAddressV4 v4Addr;
    folly::IPAddressV6 v6LinkLocalAddr;
  };

  // Map of interface entries keyed by ifName
  std::unordered_map<std::string, Interface> interfaceDb_{};

  // Hello packet send timers for each interface
  std::unordered_map<
      std::string /* ifName */,
      std::unique_ptr<fbzmq::ZmqTimeout>>
      ifNameToHelloTimers_;

  // Ordered set to keep track of allocated labels
  std::set<int32_t> allocatedLabels_;

  //
  // Neighbor state tracking
  //

  // Struct for neighbor information per interface
  struct Neighbor {
    Neighbor(
        thrift::SparkNeighbor const& info,
        uint32_t label,
        uint64_t seqNum,
        std::unique_ptr<fbzmq::ZmqTimeout> holdTimer,
        const std::chrono::milliseconds& samplingPeriod,
        std::function<void(const int64_t&)> rttChangeCb);

    // Neighbor info
    thrift::SparkNeighbor info;

    // Hold timer. If expired will declare the neighbor as stopped.
    const std::unique_ptr<fbzmq::ZmqTimeout> holdTimer{nullptr};

    // SR Label to reach Neighbor over this specific adjacency. Generated
    // using ifIndex to this neighbor. Only local within the node.
    const uint32_t label{0};

    // Last sequence number received from neighbor
    uint64_t seqNum{0};

    // Timestamps of last hello packet received from this neighbor. All
    // timestamps are derived from std::chrono::steady_clock.
    std::chrono::microseconds neighborTimestamp{0};
    std::chrono::microseconds localTimestamp{0};

    // Do we have adjacency with this neighbor. We use this to see if an UP/DOWN
    // notification is needed
    bool isAdjacent{false};

    // Currently RTT value being used to neighbor. Must be initialized to zero
    std::chrono::microseconds rtt{0};

    // Lastest measured RTT on receipt of every hello packet
    std::chrono::microseconds rttLatest{0};

    // detect rtt changes
    StepDetector<int64_t, std::chrono::milliseconds> stepDetector;
  };

  std::unordered_map<
      std::string /* ifName */,
      std::unordered_map<std::string /* neighborName */, Neighbor>>
      neighbors_{};

  // to serdeser messages over ZMQ sockets
  apache::thrift::CompactSerializer serializer_;

  // The IO primitives provider; this is used for mocking
  // the IO during unit-tests. This could be shared with other
  // instances, hence the shared_ptr
  std::shared_ptr<IoProvider> ioProvider_{nullptr};

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // vector of BucketedTimeSeries to make sure we don't take too many
  // hello packets from any one iface, address pair
  std::vector<folly::BucketedTimeSeries<int64_t, std::chrono::steady_clock>>
      timeSeriesVector_{};

  // DS to hold local stats/counters
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;
};
} // namespace openr
