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
#include <openr/fbmeshd/routing/MetricManager.h>
#include <openr/fbmeshd/routing/PeriodicPinger.h>

namespace openr {
namespace fbmeshd {

class Routing {
 public:
  /*
   * mesh path frame type
   */
  enum class MeshPathFrameType { PANN = 0 };

  /**
   * mesh path structure
   *
   * @dst: mesh path destination mac address
   * @nextHop: mesh neighbor to which frames for this destination will be
   *	forwarded
   * @sn: target sequence number
   * @metric: current metric to this destination
   * @nextHopMetric: metric for the next hop link
   * @hopCount: hops to destination
   * @expTime: when the path will expire or when it expired
   * @isRoot: the destination station of this path is a root node
   * @isGate: the destination station of this path is a mesh gate
   *
   *
   * The dst address is unique in the mesh path table.
   */
  struct MeshPath {
    explicit MeshPath(folly::MacAddress _dst) : dst{_dst} {}

    MeshPath(const MeshPath& other)
        : dst{other.dst},
          nextHop{other.nextHop},
          sn{other.sn},
          metric{other.metric},
          nextHopMetric{other.nextHopMetric},
          hopCount{other.hopCount},
          expTime{other.expTime},
          isRoot{other.isRoot},
          isGate{other.isGate} {}

    bool
    expired() const {
      return std::chrono::steady_clock::now() > expTime;
    }

    folly::MacAddress dst;
    folly::MacAddress nextHop{};
    uint64_t sn{0};
    uint32_t metric{0};
    uint32_t nextHopMetric{0};
    uint8_t hopCount{0};
    std::chrono::steady_clock::time_point expTime{
        std::chrono::steady_clock::now()};
    bool isRoot{false};
    bool isGate{false};
  };

  explicit Routing(
      folly::EventBase* evb, Nl80211Handler& nlHandler, uint32_t elementTtl);

  Routing() = delete;
  ~Routing() = default;
  Routing(const Routing&) = delete;
  Routing(Routing&&) = delete;
  Routing& operator=(const Routing&) = delete;
  Routing& operator=(Routing&&) = delete;

  void setGatewayStatus(bool isGate);

  std::unordered_map<folly::MacAddress, MeshPath> dumpMpaths();

  void setSendPacketCallback(
      std::function<void(folly::MacAddress, std::unique_ptr<folly::IOBuf>)> cb);
  void resetSendPacketCallback();

  void receivePacket(folly::MacAddress sa, std::unique_ptr<folly::IOBuf> data);

 private:
  void prepare();

  void doSyncRoutes();

  void meshPathAddGate(MeshPath& mpath);

  MeshPath& getMeshPath(folly::MacAddress addr);

  /*
   * HWMP Timer callbacks
   */
  void doMeshHousekeeping();
  void doMeshPathRoot();

  /*
   * Transmit path / path discovery
   */

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

  bool isStationInTopKGates(folly::MacAddress mac);

  void hwmpPannFrameProcess(
      folly::MacAddress sa, thrift::MeshPathFramePANN rann);

  folly::EventBase* evb_;

  // netlink handler used to request mpath from the kernel
  Nl80211Handler& nlHandler_;

  uint32_t elementTtl_;

  apache::thrift::CompactSerializer serializer_;

  PeriodicPinger periodicPinger_;

  MetricManager metricManager_;

  folly::Optional<
      std::function<void(folly::MacAddress, std::unique_ptr<folly::IOBuf>)>>
      sendPacketCallback_;

  /*
   * L3 Routing state
   */
  double const gatewayChangeThresholdFactor_{2};
  folly::Optional<std::pair<folly::MacAddress, uint32_t>> currentGate_;
  fbzmq::ZmqEventLoop zmqEvl_;
  openr::fbnl::NetlinkSocket netlinkSocket_;
  std::thread zmqEvlThread_;
  std::unique_ptr<folly::AsyncTimeout> syncRoutesTimer_;
  std::unique_ptr<folly::AsyncTimeout> noLongerAGateRANNTimer_;

  std::unique_ptr<folly::AsyncTimeout> housekeepingTimer_;
  std::unique_ptr<folly::AsyncTimeout> meshPathRootTimer_;

  /* Local mesh Sequence Number */
  uint64_t sn_{0};

  /*
   * Protocol Parameters
   */
  std::chrono::milliseconds activePathTimeout_{30000};
  bool isRoot_{false};
  std::chrono::milliseconds rootPannInterval_{5000};
  bool isGate_{false};
  bool isGateBeforeRouteSync_{false};

  /*
   * Path state
   */
  std::unordered_map<folly::MacAddress, MeshPath> meshPaths_;
};

} // namespace fbmeshd
} // namespace openr
