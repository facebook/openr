/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <queue>

#include <limits.h>
#include <linux/lwtunnel.h>
#include <linux/mpls.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <folly/futures/Future.h>

namespace openr::fbnl {

constexpr uint16_t kMaxNlPayloadSize{4096};

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

/**
 * TODO: Document this class
 */
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

  /**
   * Netlink MessageType denotes the type of request sent to the kernel, so that
   * when we receive a response from the kernel (matched by sequence number), we
   * can process them accordingly based on the request. For example, when we get
   * a RTM_NEWADDR packet, it could correspond to GET_ALL_ADDRS or
   * ADD_ADDR and NetlinkProtocolSocket will invoke the address callback
   * only for ADD_ADDR
   */
  // TODO: Rename this to `Type` .. `NetlinkMessage::Type` is intuitive enough
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

} // namespace openr::fbnl
