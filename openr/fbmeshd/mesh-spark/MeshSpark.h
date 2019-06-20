/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Portability.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/StepDetector.h>
#include <openr/common/Types.h>
#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/kvstore/KvStoreClient.h>

class MeshSpark final {
  // This class should never be copied; remove default copy/move
  MeshSpark() = delete;
  MeshSpark(const MeshSpark&) = delete;
  MeshSpark(MeshSpark&&) = delete;
  MeshSpark& operator=(const MeshSpark&) = delete;
  MeshSpark& operator=(MeshSpark&&) = delete;

 public:
  MeshSpark(
      fbzmq::ZmqEventLoop& zmqLoop,
      openr::fbmeshd::Nl80211Handler& nlHandler,
      const std::string& ifName,
      const openr::KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
      const openr::KvStoreLocalPubUrl& kvStoreLocalPubUrl,
      fbzmq::Context& zmqContext);

 private:
  /**
   * bind/connect to openr sockets
   */
  void prepareSockets() noexcept;

  /**
   * connect to link monitor's cmdSocket.
   * put it in a separate function from prepareSockets as
   * it may be called again after initialization
   */
  void prepareLinkMonitorCmdSocket() noexcept;

  FOLLY_NODISCARD openr::thrift::SparkNeighbor createOpenrNeighbor(
      folly::MacAddress macAddr, const folly::IPAddressV4& ipv4);

  void addNeighbor(
      folly::MacAddress macAddr, const folly::IPAddressV4& ipv4Addr);

  void removeNeighbor(
      folly::MacAddress macAddr, const folly::IPAddressV4& ipv4Addr);

  // send neighbor up/down events to OpenR's link monitor
  void reportToOpenR(
      const openr::thrift::SparkNeighbor& originator,
      const openr::thrift::SparkNeighborEventType& event);

  /**
   * this function is periodically called to get and process 802.11s
   * peers list from nlHander and reporting changes to link monitor
   */
  void syncPeers();

  // process interfaceDb update from LinkMonitor
  // iface add/remove , join/leave iface for UDP mcasting
  void processInterfaceDbUpdate();

  void filterWhiteListedPeers(std::vector<folly::MacAddress>& peers);

  // ZmqEventLoop pointer for scheduling async events and socket callback
  // registration
  fbzmq::ZmqEventLoop& zmqLoop_;

  // ZMQ context for processing
  fbzmq::Context& zmqContext_;

  // netlink handler used to request metrics from the kernel
  openr::fbmeshd::Nl80211Handler& nlHandler_;

  // the mesh interface name
  const std::string ifName_;

  // peer mac address -> ipv4 address
  std::unordered_map<folly::MacAddress, folly::IPAddressV4> peers_;

  // polling interval for `syncPeers`
  const std::chrono::seconds syncPeersInterval_;

  std::unique_ptr<fbzmq::ZmqTimeout> syncPeersTimer_;

  apache::thrift::CompactSerializer serializer_;

  // socket for receiving commands on interface changes
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> cmdSocket_;

  // socket for sending neighbors list to consumers
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> reportSocket_;

  // Function to process prefix updates from kvstore
  void processPrefixDbUpdate();

  // kvStoreClient for getting prefixes
  openr::KvStoreClient kvStoreClient_;

  // node name -> ipv4 address
  std::unordered_map<folly::MacAddress, folly::IPAddressV4> kvStoreIPs_;

}; // MeshSpark
