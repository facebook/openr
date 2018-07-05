/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sys/socket.h>
#include <sys/types.h>

#include <set>
#include <string>
#include <unordered_map>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <folly/Range.h>
#include <folly/SocketAddress.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/HealthChecker_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreClient.h>

namespace openr {

class HealthChecker final : public fbzmq::ZmqEventLoop {
 public:
  HealthChecker(
      std::string const& myNodeName,
      thrift::HealthCheckOption healthCheckOption,
      uint32_t healthCheckPct,
      uint16_t udpPingPort,
      std::chrono::seconds pingInterval,
      folly::Optional<int> maybeIpTos,
      const AdjacencyDbMarker& adjacencyDbMarker,
      const PrefixDbMarker& prefixDbMarker,
      const KvStoreLocalCmdUrl& storeCmdUrl,
      const KvStoreLocalPubUrl& storePubUrl,
      const HealthCheckerCmdUrl& healthCheckerCmdUrl,
      const MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext);

  ~HealthChecker() override = default;

  // non-copyable
  HealthChecker(HealthChecker const&) = delete;
  HealthChecker& operator=(HealthChecker const&) = delete;

 private:
  // prepare sockets and timeouts
  void prepare() noexcept;

  /**
   * Helper functions to create/close ping socket on demand. Socket is created
   * or updated whenever a loopback address associated with current node
   * changes. All ping packets that originates use the local loopback address.
   */
  void createPingSocket() noexcept;
  void closePingSocket() noexcept;

  // called periodically to send pings to nodesToPing_
  void pingNodes();

  // called by kvStoreClient_ whenever for each key whenever a publication
  // is received
  void processKeyVal(
      std::string const& key, folly::Optional<thrift::Value> val) noexcept;

  void processAdjDb(thrift::AdjacencyDatabase const& adjDb);
  void processPrefixDb(thrift::PrefixDatabase const& prefixDb);
  void updateNodesToPing();
  void sendDatagram(
      const std::string& nodeName,
      folly::SocketAddress const& addr,
      thrift::HealthCheckerMessageType msgType,
      int64_t seqNum);
  void processMessage();
  void processRequest();
  void printInfo();
  void submitCounters();

  const std::string myNodeName_;

  const thrift::HealthCheckOption healthCheckOption_;

  const uint32_t healthCheckPct_{0};

  const uint16_t udpPingPort_{0};

  const std::chrono::seconds pingInterval_;

  // the prefix we use to find the adjacency database announcements
  const std::string adjacencyDbMarker_;
  // the prefix we use to find the prefix db key announcements
  const std::string prefixDbMarker_;

  const folly::Optional<int> maybeIpTos_;

  apache::thrift::CompactSerializer serializer_;

  folly::Optional<int> pingSocketFd_;

  // Command Sockets to listen from requests
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> repSock_;

  // clients to interact with monitor and local kvStore
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;
  std::unique_ptr<KvStoreClient> kvStoreClient_;

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};
  // Timer for pinging addresses periodically
  std::unique_ptr<fbzmq::ZmqTimeout> pingTimer_{nullptr};

  // list of nodes to send ping messages to in next round
  std::unordered_set<std::string /* NodeName */> nodesToPing_;

  std::unordered_map<std::string, thrift::NodeHealthInfo> nodeInfo_;

  // DS to hold local stats/counters
  fbzmq::ThreadData tData_;
};

} // namespace openr
