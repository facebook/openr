/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>

#include <openr/nl/NetlinkMessage.h>
#include <openr/nl/NetlinkRoute.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr::fbnl {

constexpr uint32_t kNetlinkSockRecvBuf{1 * 1024 * 1024};
constexpr uint32_t kMaxNlMessageQueue{126001};
constexpr size_t kMaxIovMsg{500};
constexpr std::chrono::milliseconds kNlMessageAckTimer{1000};
constexpr std::chrono::milliseconds kNlRequestTimeout{30000};

/**
 * TODO: Document this class
 * TODO: Move to another file
 */
class NetlinkProtocolSocket {
 public:
  explicit NetlinkProtocolSocket(fbzmq::ZmqEventLoop* evl);

  // create socket and add to eventloop
  void init();

  // receive messages from netlink socket
  void recvNetlinkMessage();

  // send message to netlink socket
  void sendNetlinkMessage();

  ~NetlinkProtocolSocket();

  // Set netlinkSocket Link event callback
  void setLinkEventCB(std::function<void(fbnl::Link, bool)> linkEventCB);

  // Set netlinkSocket Addr event callback
  void setAddrEventCB(std::function<void(fbnl::IfAddress, bool)> addrEventCB);

  // Set netlinkSocket Addr event callback
  void setNeighborEventCB(
      std::function<void(fbnl::Neighbor, bool)> neighborEventCB);

  // process message
  void processMessage(
      const std::array<char, kMaxNlPayloadSize>& rxMsg, uint32_t bytesRead);

  // synchronous add route and nexthop paths
  ResultCode addRoute(const openr::fbnl::Route& route);

  // synchronous delete route
  ResultCode deleteRoute(const openr::fbnl::Route& route);

  // synchronous add label route
  ResultCode addLabelRoute(const openr::fbnl::Route& route);

  // synchronous delete label route
  ResultCode deleteLabelRoute(const openr::fbnl::Route& route);

  // synchronous add given list of IP or label routes and their nexthop paths
  ResultCode addRoutes(const std::vector<openr::fbnl::Route> routes);

  // synchronous delete a list of given IP or label routes
  ResultCode deleteRoutes(const std::vector<openr::fbnl::Route> routes);

  // synchronous add interface address
  ResultCode addIfAddress(const openr::fbnl::IfAddress& ifAddr);

  // synchronous delete interface address
  ResultCode deleteIfAddress(const openr::fbnl::IfAddress& ifAddr);

  // add netlink message to the queue
  void addNetlinkMessage(std::vector<std::unique_ptr<NetlinkMessage>> nlmsg);

  // get netlink request statuses
  ResultCode getReturnStatus(
      std::vector<folly::Future<int>>& futures,
      std::unordered_set<int> ignoredErrors,
      std::chrono::milliseconds timeout = kNlMessageAckTimer);

  // error count
  // TODO: protect these variables with atomic to ensure thread safety
  uint32_t getErrorCount() const;

  // ack count
  uint32_t getAckCount() const;

  // get all link interfaces from kernel using Netlink
  std::vector<fbnl::Link> getAllLinks();

  // get all interface addresses from kernel using Netlink
  std::vector<fbnl::IfAddress> getAllIfAddresses();

  // get all neighbors from kernel using Netlink
  std::vector<fbnl::Neighbor> getAllNeighbors();

  // get all routes from kernel using Netlink
  std::vector<fbnl::Route> getAllRoutes();

 private:
  NetlinkProtocolSocket(NetlinkProtocolSocket const&) = delete;
  NetlinkProtocolSocket& operator=(NetlinkProtocolSocket const&) = delete;

  fbzmq::ZmqEventLoop* evl_{nullptr};

  // Event callbacks
  std::function<void(fbnl::Link, bool)> linkEventCB_;

  std::function<void(fbnl::IfAddress, bool)> addrEventCB_;

  std::function<void(fbnl::Neighbor, bool)> neighborEventCB_;

  // netlink message queue
  std::queue<std::unique_ptr<NetlinkMessage>> msgQueue_;

  // timer to send a burst of netlink messages
  std::unique_ptr<fbzmq::ZmqTimeout> nlMessageTimer_{nullptr};

  // process ack message
  void processAck(uint32_t ack);

  // netlink socket
  int nlSock_{-1};

  // PID Of the endpoint
  uint32_t pid_{UINT_MAX};

  // source addr
  struct sockaddr_nl saddr_;

  // capture error counts
  uint32_t errors_{0};

  // NLMSG acks
  uint32_t acks_{0};

  // last sent sequence number
  uint32_t lastSeqNo_;

  // Sequence number -> NetlinkMesage request Map
  std::unordered_map<uint32_t, std::shared_ptr<NetlinkMessage>> nlSeqNoMap_;

  // Set ack status value to promise in the netlink request message
  void setReturnStatusValue(uint32_t seq, int ackStatus);

  /**
   * We maintain a temporary cache of Link, Address, Neighbor and Routes from
   * the kernel, which are solely used for the getAll... methods. These caches
   * are cleared when we invoke a new getAllLinks/Addresses/Neighbors/Routes
   */
  std::vector<fbnl::Link> linkCache_{};
  std::vector<fbnl::IfAddress> addressCache_{};
  std::vector<fbnl::Neighbor> neighborCache_{};
  std::vector<fbnl::Route> routeCache_{};
};

} // namespace openr::fbnl
