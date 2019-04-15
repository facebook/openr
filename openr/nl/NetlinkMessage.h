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

#include <openr/nl/NetlinkTypes.h>

namespace openr {
namespace Netlink {

constexpr uint16_t kMaxNlPayloadSize{4096};
constexpr uint32_t kNetlinkSockRecvBuf{1 * 1024 * 1024};

constexpr uint32_t kMaxNlMessageQueue{126001};
constexpr size_t kMaxIovMsg{500};
constexpr std::chrono::milliseconds kNlMessageSendTimer{10};
constexpr std::chrono::milliseconds kNlMessageAckTimer{2000};

enum class ResultCode {
  SUCCESS = 0,
  FAIL,
  NO_MESSAGE_BUFFER,
  SENDMSG_FAILED,
  INVALID_ADDRESS_FAMILY,
  NO_LABEL,
  NO_NEXTHOP_IP,
  NO_LOOPBACK_INDEX,
  UNKNOWN_LABEL_ACTION,
};

class NetlinkMessage {
 public:
  NetlinkMessage();

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
};

class NetlinkProtocolSocket {
 public:
  NetlinkProtocolSocket(fbzmq::ZmqEventLoop* evl, int pid);

  // create socket and add to eventloop
  void init();

  // receive messages from netlink socket
  void recvNetlinkMessage();

  // send message to netlink socket
  void sendNetlinkMessage();

  ~NetlinkProtocolSocket();

  // process message
  void processMessage(
      const std::array<char, kMaxNlPayloadSize>& rxMsg, uint32_t bytesRead);

  // add route and nexthop paths
  ResultCode addRoute(const openr::fbnl::Route& route);

  // delete route
  ResultCode deleteRoute(const openr::fbnl::Route& route);

  // add label route
  ResultCode addLabelRoute(const openr::fbnl::Route& route);

  // delete label route
  ResultCode deleteLabelRoute(const openr::fbnl::Route& route);

  // add given list of IP or label routes and their nexthop paths
  ResultCode addRoutes(const std::vector<openr::fbnl::Route> routes);

  // delete a list of given IP or label routes
  ResultCode deleteRoutes(const std::vector<openr::fbnl::Route> routes);

  // add netlink message to the queue
  void addNetlinkMessage(std::vector<std::unique_ptr<NetlinkMessage>> nlmsg);

  // error count
  uint32_t getErrorCount() const;

  // ack count
  uint32_t getAckCount() const;

 private:
  NetlinkProtocolSocket(NetlinkProtocolSocket const&) = delete;
  NetlinkProtocolSocket& operator=(NetlinkProtocolSocket const&) = delete;

  fbzmq::ZmqEventLoop* evl_{nullptr};

  // send last msghdr saved in LastMessage to netlink socket
  ResultCode sendMessageHeader();

  void resetLastMessage();

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

  // last sent message data
  struct lastMessage {
    // pointers for the last set of messages
    std::queue<std::unique_ptr<NetlinkMessage>> msgq;

    // netlink message
    std::unique_ptr<struct msghdr> msg;

    // last
    uint64_t seq;

    // timer for receving ack
    std::unique_ptr<fbzmq::ZmqTimeout> ackTimer{nullptr};
  } lastMessage_;
};
} // namespace Netlink
} // namespace openr
