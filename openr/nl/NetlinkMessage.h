/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include <openr/nl/NetlinkTypes.h>

namespace openr::fbnl {

constexpr uint16_t kMaxNlPayloadSize{4096};

/**
 * Data structure representing a netlink message, either to be sent or received.
 * It wraps `struct nlmsghdr` and provides buffer for appending message payload.
 * Further message payload in turn can contains multiple attributes and
 * sub-attributes depending on the message type.
 *
 * Aim of the message is to faciliate serialization and deserialization of
 * C++ object (application) to/from bytes (kernel).
 *
 * Maximum size of message is limited by `kMaxNlPayloadSize` parameter.
 */
class NetlinkMessage {
 public:
  NetlinkMessage();

  virtual ~NetlinkMessage();

  // construct message with type
  NetlinkMessage(int type);

  // get pointer to NLMSG Header
  struct nlmsghdr* getMessagePtr();

  // get underlying nlmsg_type
  uint16_t getMessageType() const;

  // get current length
  uint32_t getDataLength() const;

  // Buffer to create message
  std::array<char, kMaxNlPayloadSize> msg = {};

  /**
   * APIs for accumulating objects of `GET_<>` request. These APIs are invoked
   * when an object is received from kernel in-response to this netlink-message.
   * Sub-classes must override them and define behavior for them depending on
   * the request type they make.
   *
   * e.g. GET_ROUTE request will invoke `rcvdRoute(..)` for each route received
   *      from kernel. At the end `setReturnStatus(..)` will be invoked.
   */

  virtual void
  rcvdRoute(Route&& /* route */) {
    CHECK(false) << "Must be implemented by subclass";
  }

  virtual void
  rcvdLink(Link&& /* link */) {
    CHECK(false) << "Must be implemented by subclass";
  }

  virtual void
  rcvdNeighbor(Neighbor&& /* neighbor */) {
    CHECK(false) << "Must be implemented by subclass";
  }

  virtual void
  rcvdIfAddress(IfAddress&& /* ifAddr */) {
    CHECK(false) << "Must be implemented by subclass";
  }

  /**
   * Get SemiFuture associated with the the associated netlink request. Upon
   * receipt of the ack from kernel, the value will be set.
   */
  folly::SemiFuture<int> getSemiFuture();

  /**
   * Set the return value of the netlink request. Invoke this on receipt of the
   * ack. This must be invoked before class is destroyed.
   *
   * Sub-classes can override this method to define more specific behavior
   * on completion of the request. For e.g. `GET_<OBJ>` requests on completion
   * can fulfil the `Promise<vector<OBJ>>`
   */
  virtual void setReturnStatus(int status);

  std::chrono::steady_clock::time_point
  getCreateTs() const {
    return createTs_;
  }

 protected:
  // Add TLV attributes, specify the length and size of data returns ENOBUFS
  // if enough buffer is not available. Also updates the length field in
  // NLMSG header.
  // @returns 0 on success else relevant system error code
  int addAttributes(
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

  // Promise to relay the status code received from kernel
  folly::Promise<int> promise_;

  // Timestamp when message object was created
  const std::chrono::steady_clock::time_point createTs_{
      std::chrono::steady_clock::now()};
};

} // namespace openr::fbnl
