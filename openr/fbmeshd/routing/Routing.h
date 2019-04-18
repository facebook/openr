/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <queue>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <folly/IPAddressV6.h>
#include <folly/SocketAddress.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncUDPServerSocket.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBase.h>
#include <openr/nl/NetlinkSocket.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/routing/PeriodicPinger.h>

namespace openr {
namespace fbmeshd {

class Routing : public folly::EventBase,
                public folly::AsyncUDPServerSocket::Callback {
 public:
  /**
   * root mesh mode identifier
   *
   * @ROOTMODE_NO_ROOT: the mesh STA is not a root mesh STA (default)
   * @ROOTMODE_ROOT: the mesh STA is a root mesh STA if greater than
   *	this value
   * @PROACTIVE_PREQ_NO_PREP: the mesh STA is a root mesh STA supports
   *	the proactive PREQ with proactive PREP subfield set to 0
   * @PROACTIVE_PREQ_WITH_PREP: the mesh STA is a root mesh STA
   *	supports the proactive PREQ with proactive PREP subfield set to 1
   * @PROACTIVE_RANN: the mesh STA is a root mesh STA supports
   *	the proactive RANN
   */
  enum class RootModeIdentifier {
    ROOTMODE_NO_ROOT = 0,
    ROOTMODE_ROOT = 1,
    PROACTIVE_PREQ_NO_PREP = 2,
    PROACTIVE_PREQ_WITH_PREP = 3,
    PROACTIVE_RANN = 4,
  };

  /**
   * mesh path flags
   *
   * @MESH_PATH_ACTIVE: the mesh path can be used for forwarding
   * @MESH_PATH_RESOLVING: the discovery process is running for this mesh path
   * @MESH_PATH_SN_VALID: the mesh path contains a valid destination sequence
   *	number
   * @MESH_PATH_FIXED: the mesh path has been manually set and should not be
   *	modified
   * @MESH_PATH_RESOLVED: the mesh path can has been resolved
   * @MESH_PATH_REQ_QUEUED: there is an unsent path request for this destination
   *	already queued up, waiting for the discovery process to start.
   * @MESH_PATH_DELETED: the mesh path has been deleted and should no longer
   *	be used
   *
   * MESH_PATH_RESOLVED is used by the mesh path timer to
   * decide when to stop or cancel the mesh path discovery.
   */
  enum MeshPathFlags {
    MESH_PATH_ACTIVE = 0x1,
    MESH_PATH_RESOLVING = 0x2,
    MESH_PATH_SN_VALID = 0x4,
    MESH_PATH_FIXED = 0x8,
    MESH_PATH_RESOLVED = 0x10,
    MESH_PATH_REQ_QUEUED = 0x20,
    MESH_PATH_DELETED = 0x40,
  };

  /**
   * PREQ Queue Flags
   */
  enum PreqQueueFlags {
    PREQ_Q_F_START = 0x1,
    PREQ_Q_F_REFRESH = 0x2,
  };

  /**
   * PREQ element flags
   *
   * @PREQ_PROACTIVE_PREP_FLAG: proactive PREP subfield
   */
  enum PreqFlags {
    PREQ_PROACTIVE_PREP_FLAG = 1 << 2,
  };

  /**
   * PREQ element per target flags
   *
   * @PREQ_TO_FLAG: target only subfield
   * @PREQ_USN_FLAG: unknown target HWMP sequence number subfield
   */
  enum PreqTargetFlags {
    PREQ_TO_FLAG = 1 << 0,
    PREQ_USN_FLAG = 1 << 2,
  };

  /*
   * mesh path frame type
   */
  enum class MeshPathFrameType { PREQ = 0, PREP, PERR, RANN, PANN };

  /*
   * RANN flags
   */
  enum RANNFlags {
    RANN_FLAG_IS_GATE = 1 << 0,
  };

  /**
   * mesh path structure
   *
   * @dst: mesh path destination mac address
   * @nextHop: mesh neighbor to which frames for this destination will be
   *	forwarded
   * @timer: mesh path discovery timer
   * @sn: target sequence number
   * @metric: current metric to this destination
   * @hopCount: hops to destination
   * @expTime: when the path will expire or when it expired
   * @discoveryTimeout: timeout used for the last discovery retry
   * @discoveryRetries: number of discovery retries
   * @flags: mesh path flags, as specified on &enum mesh_path_flags
   * @rannSndAddr: the RANN sender address
   * @rannMetric: the aggregated path metric towards the root node
   * @lastPreqToRoot: Timestamp of last PREQ sent to root
   * @isRoot: the destination station of this path is a root node
   * @isGate: the destination station of this path is a mesh gate
   * @pathChangeCount: the number of path changes to destination
   *
   *
   * The dst address is unique in the mesh path table.
   */
  struct MeshPath {
    MeshPath(
        folly::MacAddress _dst, std::unique_ptr<folly::AsyncTimeout> _timer)
        : dst{_dst}, timer{std::move(_timer)} {}

    MeshPath(const MeshPath& other)
        : dst{other.dst},
          nextHop{other.nextHop},
          sn{other.sn},
          metric{other.metric},
          hopCount{other.hopCount},
          expTime{other.expTime},
          flags{other.flags},
          isRoot{other.isRoot},
          isGate{other.isGate} {}

    bool
    expired() const {
      return std::chrono::steady_clock::now() > expTime;
    }

    folly::MacAddress dst;
    folly::MacAddress nextHop{};
    std::unique_ptr<folly::AsyncTimeout> timer;
    uint64_t sn{0};
    uint32_t metric{0};
    uint8_t hopCount{0};
    std::chrono::steady_clock::time_point expTime{
        std::chrono::steady_clock::now()};
    std::chrono::milliseconds discoveryTimeout{0};
    uint8_t discoveryRetries{0};
    std::underlying_type_t<enum MeshPathFlags> flags{0};
    folly::MacAddress rannSndAddr{folly::MacAddress::BROADCAST};
    uint32_t rannMetric{0};
    std::chrono::steady_clock::time_point lastPreqToRoot{
        std::chrono::steady_clock::time_point::min()};
    bool isRoot{false};
    bool isGate{false};
    uint32_t pathChangeCount{0};
  };

 public:
  explicit Routing(
      Nl80211Handler& nlHandler,
      folly::SocketAddress addr,
      uint32_t elementTtl);

  Routing() = delete;
  ~Routing() override = default;
  Routing(const Routing&) = delete;
  Routing(Routing&&) = delete;
  Routing& operator=(const Routing&) = delete;
  Routing& operator=(Routing&&) = delete;

  void prepare();

  void setGatewayStatus(bool isGate);

  void
  onListenStarted() noexcept override {}

  void
  onListenStopped() noexcept override {}

  std::unordered_map<folly::MacAddress, MeshPath> dumpMpaths();

 private:
  void doSyncRoutes();

  void meshPathAddGate(MeshPath& mpath);

  MeshPath& getMeshPath(folly::MacAddress addr);

  uint32_t getAirtimeLinkMetric(const StationInfo& sta);

  uint32_t hwmpRouteInfoGet(
      folly::MacAddress sa,
      folly::MacAddress origAddr,
      uint64_t origSn,
      std::chrono::milliseconds origLifetime,
      uint32_t origMetric,
      uint8_t hopCount);

  /*
   * HWMP Timer callbacks
   */
  void doMeshHousekeeping();
  void doMeshPath();
  void doMeshPathRoot();

  /*
   * Transmit path / path discovery
   */

  void txFrame(
      MeshPathFrameType action,
      uint8_t flags,
      folly::MacAddress origAddr,
      uint64_t origSn,
      uint8_t targetFlags,
      folly::MacAddress target,
      uint64_t targetSn,
      folly::MacAddress da,
      uint8_t hopCount,
      uint8_t ttl,
      std::chrono::milliseconds lifetime,
      uint32_t metric,
      uint32_t preqId);

  void txPannFrame(
      folly::MacAddress da,
      folly::MacAddress origAddr,
      uint64_t origSn,
      uint8_t hopCount,
      uint8_t ttl,
      folly::MacAddress targetAddr,
      uint32_t metric,
      bool isGate,
      bool replyRequested);

  void txRootFrame();

  void meshQueuePreq(
      MeshPath& mpath, std::underlying_type_t<MeshPathFlags> flags);

  void meshPathStartDiscovery();

  /*
   * Receive path processing
   */

  void onDataAvailable(
      std::shared_ptr<folly::AsyncUDPSocket> socket,
      const folly::SocketAddress& client,
      std::unique_ptr<folly::IOBuf> data,
      bool truncated) noexcept override;

  bool isStationInTopKGates(folly::MacAddress mac);

  void hwmpPreqFrameProcess(
      folly::MacAddress sa,
      thrift::MeshPathFramePREQ preq,
      uint32_t pathMetric);
  void hwmpPrepFrameProcess(
      folly::MacAddress sa,
      thrift::MeshPathFramePREP prep,
      uint32_t pathMetric);
  void hwmpRannFrameProcess(
      folly::MacAddress sa, thrift::MeshPathFrameRANN rann);
  void hwmpPannFrameProcess(
      folly::MacAddress sa, thrift::MeshPathFramePANN rann);

  // netlink handler used to request mpath from the kernel
  Nl80211Handler& nlHandler_;

  folly::AsyncUDPServerSocket socket_;

  folly::AsyncUDPSocket clientSocket_;

  folly::SocketAddress addr_;

  uint32_t elementTtl_;

  apache::thrift::CompactSerializer serializer_;

  PeriodicPinger periodicPinger_;

  /*
   * L3 Routing state
   */
  double const gatewayChangeThresholdFactor_{2};
  folly::Optional<std::pair<folly::MacAddress, int32_t>> currentRoot_;
  fbzmq::ZmqEventLoop zmqEvl_;
  openr::fbnl::NetlinkSocket netlinkSocket_;
  std::thread zmqEvlThread_;
  std::unique_ptr<folly::AsyncTimeout> syncRoutesTimer_;
  std::unique_ptr<folly::AsyncTimeout> noLongerAGateRANNTimer_;

  std::unique_ptr<folly::AsyncTimeout> housekeepingTimer_;
  std::unique_ptr<folly::AsyncTimeout> meshPathTimer_;
  std::unique_ptr<folly::AsyncTimeout> meshPathRootTimer_;

  /* Local mesh Sequence Number */
  uint32_t sn_{0};
  uint32_t preqId_{0};
  /* Timestamp of last SN update */
  std::chrono::steady_clock::time_point lastSnUpdate_;
  /* Time when it's ok to send next PERR */
  std::chrono::steady_clock::time_point nextPerr_;
  /* Timestamp of last PREQ sent */
  std::chrono::steady_clock::time_point lastPreq_;
  std::queue<
      std::pair<folly::MacAddress, std::underlying_type_t<PreqQueueFlags>>>
      preqQueue_;

  /*
   * HWMP Configuration
   */
  uint8_t dot11MeshHWMPmaxPREQretries_{4};
  std::chrono::milliseconds minDiscoveryTimeout_{100};
  std::chrono::milliseconds dot11MeshHWMPactivePathTimeout_{30000};
  std::chrono::milliseconds dot11MeshHWMPpreqMinInterval_{10};
  std::chrono::milliseconds dot11MeshHWMPnetDiameterTraversalTime_{50};
  RootModeIdentifier dot11MeshHWMPRootMode_{
      RootModeIdentifier::ROOTMODE_NO_ROOT};
  std::chrono::milliseconds dot11MeshHWMPRannInterval_{3000};
  bool dot11MeshGateAnnouncementProtocol_{false};
  bool dot11MeshForwarding_{true};
  std::chrono::milliseconds dot11MeshHWMPactivePathToRootTimeout_{6000};
  std::chrono::milliseconds dot11MeshHWMProotInterval_{5000};
  std::chrono::milliseconds dot11MeshHWMPconfirmationInterval_{2000};

  /*
   * Path state
   */
  std::unordered_map<folly::MacAddress, MeshPath> meshPaths_;
  std::unordered_map<folly::MacAddress, MeshPath> mppPaths_;
};

} // namespace fbmeshd
} // namespace openr
