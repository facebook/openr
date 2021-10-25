/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fmt/format.h>
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
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/spark/IoProvider.h>

namespace openr {

/*
 * [Sanity Check]
 *
 * This is the ENUM set used for Spark pkt sanity check.
 */
enum class PacketValidationResult {
  SUCCESS = 1,
  FAILURE = 2,
  SKIP = 3,
};

/*
 * [Spark Neighbor FSM]
 *
 * Define:
 *  1) SparkNeighState
 *  2) SparkNeighEvent
 *
 * This is used to define transition state for neighbors as part of the
 * Finite State Machine(FSM).
 */
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

/*
 * Spark is responsible of telling our peer of our existence and also tracking
 * the neighbor liveness.  It receives commands in form of "interface", on which
 * neighbor discovery will be performed. The discovered neighbors, aka, "Local
 * Topology" of the node, is fed into other modules for synchronization purpose
 * and SPF calculation.
 */

class Spark final : public OpenrEventBase {
  friend class SparkWrapper;

 public:
  Spark(
      // consumer Queue
      messaging::RQueue<InterfaceEvent> interfaceUpdatesQueue,
      // producer Queue
      messaging::ReplicateQueue<NeighborEvents>& nbrUpdatesQueue,
      // raw ptr of modules
      std::shared_ptr<IoProvider> ioProvider,
      std::shared_ptr<const Config> config,
      // lowest supported version + current version
      std::pair<uint32_t, uint32_t> version = std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
      // rate limit
      std::optional<uint32_t> maybeMaxAllowedPps = Constants::kMaxAllowedPps);

  ~Spark() override = default;

  void stop() override;

  /*
   * [Public API]
   *
   * Spark exposed multiple public API for external caller to be able to:
   *  1) retrieve neighbor information;
   *  2) retrieve neighbor state;
   *  3) etc.
   */

  folly::SemiFuture<folly::Unit> floodRestartingMsg();
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::SparkNeighbor>>>
  getNeighbors();
  folly::SemiFuture<std::optional<SparkNeighState>> getSparkNeighState(
      std::string const& ifName, std::string const& neighborName);

  // Util function to convert ENUM SparlNeighborState to string
  static std::string toStr(SparkNeighState state);

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
    operator!=(const Interface& interface) const {
      return ifIndex != interface.ifIndex or v4Network != interface.v4Network or
          v6LinkLocalNetwork != interface.v6LinkLocalNetwork;
    }

    int ifIndex{0};
    folly::CIDRNetwork v4Network;
    folly::CIDRNetwork v6LinkLocalNetwork;
  };

  // Spark is non-copyable
  Spark(Spark const&) = delete;
  Spark& operator=(Spark const&) = delete;

  // Initializes UDP socket for multicast neighbor discovery
  void prepareSocket() noexcept;

  // check neighbor's hello packet; return true if packet is valid and
  // passed the following checks:
  // (1) neighbor is not self (packet not looped back)
  // (2) interface is tracked interface
  PacketValidationResult sanityCheckMsg(
      std::string const& neighborName, std::string const& ifName);

  // Determine if we should process the next packte from this ifName, addr pair
  bool shouldProcessPacket(
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

  /*
   * [Interface Update/Initialization Event Management]
   *
   * Spark will be the reader of following event:
   *  1) Interface database update from LinkMonitor to appropriately
   *     enable/disable neighbor discovery;
   *  2) Open/R Initialization Event from LinkMonitor;
   */
  void processInterfaceUpdates(InterfaceDatabase&& interfaceUpdates);
  void processInitializationEvent(thrift::InitializationEvent&& event);

  // util function to delete interface in spark
  void deleteInterface(const std::vector<std::string>& toDel);

  // util function to add interface in spark
  void addInterface(
      const std::vector<std::string>& toAdd,
      const std::unordered_map<std::string, Interface>& newInterfaceDb);

  // util function to update interface in spark
  void updateInterface(
      const std::vector<std::string>& toUpdate,
      const std::unordered_map<std::string, Interface>& newInterfaceDb);

  // TODO: standardize Spark inline documentation
  // find an interface name in the interfaceDb given an ifIndex
  std::optional<std::string> findInterfaceFromIfindex(int ifIndex);

  // TODO: deprecate adj label generation from Spark
  // Utility function to generate a new label for neighbor on given interface.
  // If there is only one neighbor per interface then labels are expected to be
  // same across process-restarts
  int32_t getNewLabelForIface(
      std::string const& ifName, std::string const& areaId);

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
        bool enableFloodOptimization,
        uint32_t label,
        uint64_t seqNum,
        std::chrono::milliseconds const& samplingPeriod,
        std::function<void(const int64_t&)> rttChangeCb,
        const std::string& area);

    // util function to transfer to SparkNeighbor
    thrift::SparkNeighbor toThrift() const;

    // util function to unblock adjacency hold
    bool shouldResetAdjacencyHold(
        const thrift::SparkHeartbeatMsg& heartbeatMsg);

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

    // flag to indicate if flood-optimization is supported or NOT
    bool enableFloodOptimization{false};

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

    // flag to indicate if Spark will hold on reporting neighbor adjacency
    bool isAdjacencyOnHold{false};
  };

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
      NeighborEventType type, thrift::SparkNeighbor const& info);

  // callback function for rtt change
  void processRttChange(
      std::string const& ifName,
      std::string const& neighborName,
      int64_t const newRtt);

  // rounding the RTT for metric calculation
  std::chrono::microseconds rttRounding(int64_t const rtt);

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

  // Interval that Spark holds before publishing discovered neighbors in OpenR
  // initialization procedure. It is set as '3 * fastInitHelloTime_ +
  // handshakeTime_' to make sure Spark has enough time to process fast neighbor
  // discovery among all received interfaces.
  const std::chrono::milliseconds initializationHoldTime_{0};

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

  // This flag indicates that we will enable v4 over v6 nexthop
  const bool v4OverV6Nexthop_{false};

  // This flag indicates that if DUAL flood-optimization is supported or NOT
  const bool enableFloodOptimization_{false};

  // the next sequence number to be used on any interface for outgoing hellos
  // NOTE: we increment this on hello sent out of any interfaces
  uint64_t mySeqNum_{1};

  // the multicast socket we use
  int mcastFd_{-1};

  // state transition matrix for Finite-State-Machine
  static const std::vector<std::vector<std::optional<SparkNeighState>>>
      stateMap_;

  // Queue to publish neighbor events
  messaging::ReplicateQueue<NeighborEvents>& neighborUpdatesQueue_;

  // this is used to inform peers about my kvstore tcp ports
  const uint16_t kOpenrCtrlThriftPort_{0};

  // current version and supported version
  const thrift::OpenrVersions kVersion_;

  // Map of interface entries keyed by ifName
  std::unordered_map<std::string, Interface> interfaceDb_{};

  std::unordered_map<
      std::string /* ifName */,
      std::unordered_map<std::string /* neighborName */, SparkNeighbor>>
      sparkNeighbors_{};

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

  // ser/deser messages over sockets
  apache::thrift::CompactSerializer serializer_;

  // The IO primitives provider; this is used for mocking
  // the IO during unit-tests. This could be shared with other
  // instances, hence the shared_ptr
  std::shared_ptr<IoProvider> ioProvider_{nullptr};

  // vector of BucketedTimeSeries to make sure we don't take too many
  // hello packets from any one iface, address pair
  std::vector<folly::BucketedTimeSeries<int64_t, std::chrono::steady_clock>>
      timeSeriesVector_{};

  // flag to indicate if ordered publication is enabled
  bool enableOrderedAdjPublication{false};

  // global openr config
  std::shared_ptr<const Config> config_{nullptr};

  // Timer for updating and submitting counters periodically
  std::unique_ptr<folly::AsyncTimeout> counterUpdateTimer_{nullptr};

  // Timer for collecting neighbors successfully discovered and publishing them
  // to neighborUpdatesQueue_ in OpenR initialization procedure.
  std::unique_ptr<folly::AsyncTimeout> initializationHoldTimer_{nullptr};

  // Boolean flag indicating whether initial interfaces are received during
  // Open/R initialization procedure.
  bool initialInterfacesReceived_{false};

  // Boolean flag indicating whether adjacency database is synced during
  // Open/R initialization procedure.
  bool adjacencyDbSynced_{false};

  // Optional rate-limit on processing inbound Spark messages
  std::optional<uint32_t> maybeMaxAllowedPps_;

  // Whether to throw parsing errors upwards, or suppress.
  // Fuzzer needs to see exceptions.
  bool isThrowParserErrorsOn_ = false;
};
} // namespace openr
