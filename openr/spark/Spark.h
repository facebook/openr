/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <functional>

#include <folly/SocketAddress.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/stats/BucketedTimeSeries.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/StepDetector.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Spark_types.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/spark/IoProvider.h>

namespace openr {

enum class PacketValidationResult {
  SUCCESS = 1,
  FAILURE = 2,
  NEIGHBOR_RESTART = 3,
  SKIP_LOOPED_SELF = 4,
  INVALID_AREA_CONFIGURATION = 5,
};

//
// Define SparkNeighState for Spark usage. This is used to define
// transition state for neighbors as part of the Finite State Machine.
//
enum class SparkNeighState {
  IDLE = 0,
  WARM = 1,
  NEGOTIATE = 2,
  ESTABLISHED = 3,
  RESTART = 4,
};

enum class SparkNeighEvent {
  HELLO_RCVD_INFO = 0,
  HELLO_RCVD_NO_INFO = 1,
  HELLO_RCVD_RESTART = 2,
  HEARTBEAT_RCVD = 3,
  HANDSHAKE_RCVD = 4,
  HEARTBEAT_TIMER_EXPIRE = 5,
  NEGOTIATE_TIMER_EXPIRE = 6,
  GR_TIMER_EXPIRE = 7,
  NEGOTIATION_FAILURE = 8,
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

class Spark final : public OpenrEventBase {
  friend class SparkWrapper;

 public:
  Spark(
      std::optional<int> ipTos,
      messaging::RQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue,
      messaging::ReplicateQueue<thrift::SparkNeighborEvent>& nbrUpdatesQueue,
      KvStoreCmdPort kvStoreCmdPort,
      OpenrCtrlThriftPort openrCtrlThriftPort,
      std::shared_ptr<IoProvider> ioProvider,
      std::shared_ptr<const Config> config,
      std::pair<uint32_t, uint32_t> version = std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
      std::optional<uint32_t> maybeMaxAllowedPps = Constants::kMaxAllowedPps);

  ~Spark() override = default;

  // Public APIs
  folly::SemiFuture<folly::Unit> floodRestartingMsg();

  // get the current state of neighborNode, used for unit-testing
  folly::SemiFuture<std::optional<SparkNeighState>> getSparkNeighState(
      std::string const& ifName, std::string const& neighborName);

  // override eventloop stop()
  void stop() override;

  // Turn on the throwing of parsing errors.
  void setThrowParserErrors(bool);

 private:
  //
  // Interface tracking
  //
  class Interface {
   public:
    Interface(
        int ifIndex,
        const folly::CIDRNetwork& v4Network,
        const folly::CIDRNetwork& v6LinkLocalNetwork)
        : ifIndex(ifIndex),
          v4Network(v4Network),
          v6LinkLocalNetwork(v6LinkLocalNetwork) {}

    bool
    operator==(const Interface& interface) const {
      return (
          (ifIndex == interface.ifIndex) &&
          (v4Network == interface.v4Network) &&
          (v6LinkLocalNetwork == interface.v6LinkLocalNetwork));
    }

    int ifIndex{0};
    folly::CIDRNetwork v4Network;
    folly::CIDRNetwork v6LinkLocalNetwork;
  };

  // Spark is non-copyable
  Spark(Spark const&) = delete;
  Spark& operator=(Spark const&) = delete;

  // Initializes UDP socket for multicast neighbor discovery
  void prepareSocket(std::optional<int> maybeIpTos) noexcept;

  // check neighbor's hello packet; return true if packet is valid and
  // passed the following checks:
  // (1) neighbor is not self (packet not looped back)
  // (2) performs various other validation e.g. domain, version etc.
  PacketValidationResult sanityCheckHelloPkt(
      std::string const& domainName,
      std::string const& neighborName,
      std::string const& remoteIfName,
      uint32_t const& remoteVersion);

  // Determine if we should process the next packte from this ifName, addr pair
  bool shouldProcessHelloPacket(
      std::string const& ifName, folly::IPAddress const& addr);

  // process hello packet from a neighbor. we want to see if
  // the neighbor could be added as adjacent peer.
  void processPacket();

  // process helloMsg in Spark context
  void processHelloMsg(
      thrift::SparkHelloMsg const& helloMsg,
      std::string const& ifName,
      std::chrono::microseconds const& myRecvTimeInUs);

  // process heartbeatMsg in Spark context
  void processHeartbeatMsg(
      thrift::SparkHeartbeatMsg const& heartbeatMsg, std::string const& ifName);

  // process handshakeMsg to update sparkNeighbors_ db
  void processHandshakeMsg(
      thrift::SparkHandshakeMsg const& handshakeMsg, std::string const& ifName);

  // util call to send hello msg
  void sendHelloMsg(
      std::string const& ifName,
      bool inFastInitState = false,
      bool restarting = false);

  // util call to send handshake msg
  void sendHandshakeMsg(
      std::string const& ifName,
      std::string const& neighborName,
      std::string const& neighborAreaId,
      bool isAdjEstablished);

  // util call to send heartbeat msg
  void sendHeartbeatMsg(std::string const& ifName);

  // Function processes interface updates from LinkMonitor and appropriately
  // enable/disable neighbor discovery
  void processInterfaceUpdates(thrift::InterfaceDatabase&& interfaceUpdates);

  // util function to delete interface in spark
  void deleteInterfaceFromDb(const std::set<std::string>& toDel);

  // util function to add interface in spark
  void addInterfaceToDb(
      const std::set<std::string>& toAdd,
      const std::unordered_map<std::string, Interface>& newInterfaceDb);

  // util function to update interface in spark
  void updateInterfaceInDb(
      const std::set<std::string>& toUpdate,
      const std::unordered_map<std::string, Interface>& newInterfaceDb);

  // find an interface name in the interfaceDb given an ifIndex
  std::optional<std::string> findInterfaceFromIfindex(int ifIndex);

  // Utility function to generate a new label for neighbor on given interface.
  // If there is only one neighbor per interface then labels are expected to be
  // same across process-restarts
  int32_t getNewLabelForIface(std::string const& ifName);

  // set flat counter/stats
  void updateGlobalCounters();

  // utility method to add regex for:
  //
  //  tuple(areaId, neighbor_regex, interface_regex)
  //
  // NOTE: by default, use ".*" to match everything.
  void addAreaRegex(
      const std::string& areaId,
      const std::vector<std::string>& neighbor_regexes,
      const std::vector<std::string>& interface_regexes);

  // util function to deduce `areaId` from neighbor.
  // This is util function to deduce `areaId` from neighbor during helloMsg
  // processing by leveraging:
  //
  //  1). interface from which helloMsg received;
  //  2). neighbor's nodeName;
  //
  // against `thrift::AreaConfig` parsed by Spark. It support both
  // interface and peer node name regexes. Treat multiple/conflict
  // deduced area as error. Tie-breaking mechanism can be implemented
  // if needed.
  static std::optional<std::string> getNeighborArea(
      const std::string& peerNodeName,
      const std::string& ifName,
      const std::unordered_map<std::string /* areaId */, AreaConfiguration>&
          areaConfigs);

  // function to receive and parse received pkt
  bool parsePacket(
      thrift::SparkHelloPacket& pkt /* packet( type will be renamed later) */,
      std::string& ifName /* interface */,
      std::chrono::microseconds& recvTime /* kernel timestamp when recved */);

  // function to validate v4Address with its subnet
  PacketValidationResult validateV4AddressSubnet(
      std::string const& ifName, thrift::BinaryAddress neighV4Addr);

  // function wrapper to update RTT for neighbor
  void updateNeighborRtt(
      std::chrono::microseconds const& myRecvTimeInUs,
      std::chrono::microseconds const& mySentTimeInUs,
      std::chrono::microseconds const& nbrRecvTimeInUs,
      std::chrono::microseconds const& nbrSentTimeInUs,
      std::string const& neighborName,
      std::string const& remoteIfName,
      std::string const& ifName);

  //
  // Spark related function call
  //
  struct SparkNeighbor {
    SparkNeighbor(
        const thrift::StepDetectorConfig&,
        std::string const& domainName,
        std::string const& nodeName,
        std::string const& localIfName,
        std::string const& remoteIfName,
        uint32_t label,
        uint64_t seqNum,
        std::chrono::milliseconds const& samplingPeriod,
        std::function<void(const int64_t&)> rttChangeCb,
        const std::string& area);

    // util function to transfer to SparkNeighbor
    thrift::SparkNeighbor
    toThrift() const {
      thrift::SparkNeighbor info;

      // populate basic info
      info.nodeName_ref() = nodeName;
      info.state_ref() = toStr(state); // translate to string
      info.area_ref() = area;

      // populate address/port info for TCP connection
      info.transportAddressV4_ref() = transportAddressV4;
      info.transportAddressV6_ref() = transportAddressV6;
      info.openrCtrlThriftPort_ref() = openrCtrlThriftPort;
      info.kvStoreCmdPort_ref() = kvStoreCmdPort;

      // populate interface info
      info.localIfName_ref() = localIfName;
      info.remoteIfName_ref() = remoteIfName;

      // populate misc info
      info.rttUs_ref() = rtt.count();
      info.label_ref() = label;

      return info;
    }

    // doamin name
    const std::string domainName{};

    // node name
    const std::string nodeName{};

    // local interface name
    const std::string localIfName{};

    // remote interface name on neighbor side
    const std::string remoteIfName{};

    // SR Label to reach Neighbor over this specific adjacency. Generated
    // using ifIndex to this neighbor. Only local within the node.
    const uint32_t label{0};

    // Last sequence number received from neighbor
    uint64_t seqNum{0};

    // neighbor state(IDLE by default)
    SparkNeighState state{SparkNeighState::IDLE};

    // timer to periodically send out handshake pkt
    std::unique_ptr<folly::AsyncTimeout> negotiateTimer{nullptr};

    // negotiate stage hold-timer
    std::unique_ptr<folly::AsyncTimeout> negotiateHoldTimer{nullptr};

    // heartbeat hold-timer
    std::unique_ptr<folly::AsyncTimeout> heartbeatHoldTimer{nullptr};

    // graceful restart hold-timer
    std::unique_ptr<folly::AsyncTimeout> gracefulRestartHoldTimer{nullptr};

    // KvStore related port. Info passed to LinkMonitor for neighborEvent
    int32_t kvStoreCmdPort{0};
    int32_t openrCtrlThriftPort{0};

    // hold time
    std::chrono::milliseconds heartbeatHoldTime{0};
    std::chrono::milliseconds gracefulRestartHoldTime{0};

    // v4/v6 network address
    thrift::BinaryAddress transportAddressV4;
    thrift::BinaryAddress transportAddressV6;

    // Timestamps of last hello packet received from this neighbor.
    // All timestamps are derived from std::chrono::steady_clock.
    std::chrono::microseconds neighborTimestamp{0};
    std::chrono::microseconds localTimestamp{0};

    // Currently RTT value being used to neighbor. Must be initialized to zero
    std::chrono::microseconds rtt{0};

    // Lastest measured RTT on receipt of every hello packet
    std::chrono::microseconds rttLatest{0};

    // Time when a neighbor state becomes IDLE
    std::chrono::time_point<std::chrono::steady_clock> idleStateTransitionTime =
        std::chrono::steady_clock::now();

    // Time when a neighbor state becomes RESTART
    std::chrono::time_point<std::chrono::steady_clock>
        restartStateTransitionTime = std::chrono::steady_clock::now();

    // detect rtt changes
    StepDetector<int64_t, std::chrono::milliseconds> stepDetector;

    // area on which adjacency is formed
    std::string area{};
  };

  std::unordered_map<
      std::string /* ifName */,
      std::unordered_map<std::string /* neighborName */, SparkNeighbor>>
      sparkNeighbors_{};

  // util function to log Spark neighbor state transition
  void logStateTransition(
      std::string const& neighborName,
      std::string const& ifName,
      SparkNeighState const& oldState,
      SparkNeighState const& newState);

  // util function to check SparkNeighState
  void checkNeighborState(
      SparkNeighbor const& neighbor, SparkNeighState const& state);

  // wrapper call to declare neighborship down
  void neighborUpWrapper(
      SparkNeighbor& neighbor,
      std::string const& ifName,
      std::string const& neighborName);

  // wrapper call to declare neighborship down
  void neighborDownWrapper(
      SparkNeighbor const& neighbor,
      std::string const& ifName,
      std::string const& neighborName);

  // utility call to send SparkNeighborEvent
  void notifySparkNeighborEvent(
      thrift::SparkNeighborEventType type, thrift::SparkNeighbor const& info);

  // callback function for rtt change
  void processRttChange(
      std::string const& ifName,
      std::string const& neighborName,
      int64_t const newRtt);

  // wrapper function to process GR msg
  void processGRMsg(
      std::string const& neighborName,
      std::string const& ifName,
      SparkNeighbor& neighbor);

  // process timeout for heartbeat
  void processHeartbeatTimeout(
      std::string const& ifName, std::string const& neighborName);

  // process timeout for negotiate stage
  void processNegotiateTimeout(
      std::string const& ifName, std::string const& neighborName);

  // process timeout for graceful restart
  void processGRTimeout(
      std::string const& ifName, std::string const& neighborName);

  // Util function to convert ENUM SparlNeighborState to string
  static std::string toStr(SparkNeighState state);

  // Util function for state transition
  static SparkNeighState getNextState(
      std::optional<SparkNeighState> const& currState,
      SparkNeighEvent const& event);

  //
  // Private state
  //

  // This node's domain name
  const std::string myDomainName_{};

  // This node's name
  const std::string myNodeName_{};

  // UDP port for send/recv of spark hello messages
  const uint16_t neighborDiscoveryPort_{6666};

  // Spark hello msg sendout interval
  const std::chrono::milliseconds helloTime_{0};

  // Spark hello msg sendout interval under fast-init case
  const std::chrono::milliseconds fastInitHelloTime_{0};

  // Spark handshake msg sendout interval
  const std::chrono::milliseconds handshakeTime_{0};

  // Spark heartbeat msg sendout interval (keepAliveTime)
  const std::chrono::milliseconds keepAliveTime_{0};

  // Spark negotiate stage hold time
  const std::chrono::milliseconds handshakeHoldTime_{0};

  // Spark heartbeat msg hold time
  const std::chrono::milliseconds holdTime_{0};

  // Spark hold time under graceful-restart mode
  const std::chrono::milliseconds gracefulRestartTime_{0};

  // This flag indicates that we will also exchange v4 transportAddress in
  // Spark HelloMessage
  const bool enableV4_{false};

  // the next sequence number to be used on any interface for outgoing hellos
  // NOTE: we increment this on hello sent out of any interfaces
  uint64_t mySeqNum_{1};

  // the multicast socket we use
  int mcastFd_{-1};

  // state transition matrix for Finite-State-Machine
  static const std::vector<std::vector<std::optional<SparkNeighState>>>
      stateMap_;

  // Queue to publish neighbor events
  messaging::ReplicateQueue<thrift::SparkNeighborEvent>& neighborUpdatesQueue_;

  // this is used to inform peers about my kvstore tcp ports
  const uint16_t kKvStoreCmdPort_{0};
  const uint16_t kOpenrCtrlThriftPort_{0};

  // current version and supported version
  const thrift::OpenrVersions kVersion_;

  // Map of interface entries keyed by ifName
  std::unordered_map<std::string, Interface> interfaceDb_{};

  // Hello packet send timers for each interface
  std::unordered_map<
      std::string /* ifName */,
      std::unique_ptr<folly::AsyncTimeout>>
      ifNameToHelloTimers_{};

  // heartbeat packet send timers for each interface
  std::unordered_map<
      std::string /* ifName */,
      std::unique_ptr<folly::AsyncTimeout>>
      ifNameToHeartbeatTimers_{};

  // number of active neighbors for each interface
  std::unordered_map<
      std::string /* ifName */,
      std::unordered_set<std::string> /* neighbors */>
      ifNameToActiveNeighbors_{};

  // ordered set to keep track of allocated labels
  std::set<int32_t> allocatedLabels_{};

  // to serdeser messages over ZMQ sockets
  apache::thrift::CompactSerializer serializer_;

  // The IO primitives provider; this is used for mocking
  // the IO during unit-tests. This could be shared with other
  // instances, hence the shared_ptr
  std::shared_ptr<IoProvider> ioProvider_{nullptr};

  // vector of BucketedTimeSeries to make sure we don't take too many
  // hello packets from any one iface, address pair
  std::vector<folly::BucketedTimeSeries<int64_t, std::chrono::steady_clock>>
      timeSeriesVector_{};

  // global openr config
  std::shared_ptr<const Config> config_{nullptr};

  // Timer for updating and submitting counters periodically
  std::unique_ptr<folly::AsyncTimeout> counterUpdateTimer_{nullptr};

  // Optional rate-limit on processing inbound Spark messages
  std::optional<uint32_t> maybeMaxAllowedPps_;

  // Whether to throw parsing errors upwards, or suppress.
  // Fuzzer needs to see exceptions.
  bool isThrowParserErrorsOn_ = false;
};
} // namespace openr
