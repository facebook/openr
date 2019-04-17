/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <folly/Expected.h>
#include <folly/MacAddress.h>
#include <folly/Optional.h>
#include <folly/Unit.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/mesh-spark/MeshSpark.h>
#include <openr/prefix-manager/PrefixManagerClient.h>

namespace openr {
namespace fbmeshd {

class Separa final : public fbzmq::ZmqEventLoop {
 public:
  Separa(
      uint16_t const udpHelloPort,
      std::chrono::seconds const broadcastInterval,
      std::chrono::seconds const domainLockdownPeriod,
      double const domainChangeThresholdFactor,
      const bool backwardsCompatibilityMode,
      Nl80211Handler& nlHandler,
      MeshSpark& meshSpark,
      const PrefixManagerLocalCmdUrl& prefixManagerCmdUrl,
      const openr::DecisionCmdUrl& decisionCmdUrl,
      const openr::KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
      const openr::KvStoreLocalPubUrl& kvStoreLocalPubUrl,
      const openr::MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext);

  ~Separa() override = default;

 private:
  // Separa is non-copyable
  Separa(Separa const&) = delete;
  Separa& operator=(Separa const&) = delete;

  // Initializes ZMQ sockets
  void prepare() noexcept;

  // connect to decision cmdSocket.
  void prepareDecisionCmdSocket() noexcept;

  // process hello packet received from neighbors
  void processHelloPacket();

  // send Separa hello packet
  void sendHelloPacket();

  // get the node which acts as the gateway node
  folly::Expected<std::pair<folly::MacAddress, int>, folly::Unit> getGateway();

  // UDP port for send/recv of broadcasts
  const uint16_t udpHelloPort_;

  // how often to send connectivity information broadcasts
  const std::chrono::seconds broadcastInterval_;

  std::unique_ptr<fbzmq::ZmqTimeout> broadcastTimer_;

  // domain lock timer
  folly::Optional<std::chrono::steady_clock::time_point> domainLockUntil_;

  const std::chrono::seconds domainLockdownPeriod_;

  const double domainChangeThresholdFactor_;

  // the socket we use for send/recv of hello packets
  int socketFd_{-1};

  const bool backwardsCompatibilityMode_;

  // netlink handler used to request metrics from the kernel
  Nl80211Handler& nlHandler_;

  MeshSpark& meshSpark_;

  openr::PrefixManagerClient prefixManagerClient_;

  const std::string decisionCmdUrl_;
  std::unique_ptr<fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>> decisionCmdSock_;

  apache::thrift::CompactSerializer serializer_;

  // kvStoreClient for getting prefixes
  openr::KvStoreClient kvStoreClient_;

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_;

  // DS to keep track of stats
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  fbzmq::ZmqMonitorClient zmqMonitorClient_;

  // ZMQ context for processing
  fbzmq::Context& zmqContext_;

}; // Separa

} // namespace fbmeshd
} // namespace openr
