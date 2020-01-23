/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <queue>

#include <limits.h>
#include <linux/lwtunnel.h>
#include <linux/mpls.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/futures/Future.h>

#include <openr/nl/NetlinkTypes.h>

namespace openr::fbnl {
class NetlinkSocket;

constexpr uint16_t kMaxNlPayloadSize{4096};
constexpr uint32_t kNetlinkSockRecvBuf{1 * 1024 * 1024};

constexpr uint32_t kMaxNlMessageQueue{126001};
constexpr size_t kMaxIovMsg{500};
constexpr std::chrono::milliseconds kNlMessageAckTimer{1000};
constexpr std::chrono::milliseconds kNlRequestTimeout{30000};

enum class ResultCode {
  SUCCESS = 0,
  FAIL,
  TIMEOUT,
  SYSERR,
  NO_MESSAGE_BUFFER,
  SENDMSG_FAILED,
  INVALID_ADDRESS_FAMILY,
  NO_LABEL,
  NO_NEXTHOP_IP,
  NO_LOOPBACK_INDEX,
  UNKNOWN_LABEL_ACTION,
  NO_IP
};

class NetlinkMessage {
 public:
  NetlinkMessage();

  virtual ~NetlinkMessage() = default;

  // construct message with type
  NetlinkMessage(int type);

  // get pointer to NLMSG Header
  struct nlmsghdr* getMessagePtr();

  // get current length
  uint32_t getDataLength() const;

  // Buffer to create message
  std::array<char, kMaxNlPayloadSize> msg = {};

  // update size of message received
  void updateBytesReceived(uint16_t bytes);

  // set status value (in promise)
  void setReturnStatus(int status);

  folly::Future<int> getFuture();

  /* Netlink MessageType denotes the type of request sent to the kernel, so that
   * when we receive a response from the kernel (matched by sequence number), we
   * can process them accordingly based on the request. For example, when we get
   a RTM_NEWADDR packet, it could correspond to GET_ALL_ADDRS or
   ADD_ADDR and NetlinkProtocolSocket will invoke the address callback
   only for ADD_ADDR */
  enum class MessageType {
    GET_ALL_LINKS,
    GET_ALL_ADDRS,
    GET_ADDR,
    ADD_ADDR,
    DEL_ADDR,
    GET_ALL_NEIGHBORS,
    GET_ALL_ROUTES,
    GET_ROUTE,
    ADD_ROUTE,
    DEL_ROUTE
  } messageType_;

  // get Message Type
  NetlinkMessage::MessageType getMessageType() const;

  // set Message Type
  void setMessageType(NetlinkMessage::MessageType type);

 protected:
  // add TLV attributes, specify the length and size of data
  // returns false if enough buffer is not available. Also updates the
  // length field in NLMSG header
  ResultCode addAttributes(
      int type,
      const char* const data,
      uint32_t len,
      struct nlmsghdr* const msghdr);

  // add a sub RTA inside an RTA. The length of sub RTA will not be added into
  // the NLMSG header, but will be added to the parent RTA.
  struct rtattr* addSubAttributes(
      struct rtattr* rta, int type, const void* data, uint32_t len) const;

 private:
  // disable copy, assign constructores
  NetlinkMessage(NetlinkMessage const&) = delete;
  NetlinkMessage& operator=(NetlinkMessage const&) = delete;

  // pointer to the netlink message header
  struct nlmsghdr* const msghdr{nullptr};

  // size available for adding messages,
  // in case of rx message, it contains bytes received
  uint32_t size_{kMaxNlPayloadSize};

  // Promise to relay the status code received from kernel
  std::unique_ptr<folly::Promise<int>> promise_{nullptr};
};

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
