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

/*
 * Data structure representing a netlink message, either to be sent or received.
 * It wraps `struct nlmsghdr` and provides buffer for appending message payload.
 * Further message payload in turn can contain multiple attributes and
 * sub-attributes depending on the message type.
 *
 * Aim of the message is to faciliate serialization and deserialization of
 * C++ object (application) to/from bytes (kernel).
 *
 * Maximum size of message is limited by `kMaxNlPayloadSize` parameter.
 */
/*
 * For netlink reference:
 *
 * https://man7.org/linux/man-pages/man7/netlink.7.html
 *
 * Synopsis:
 *
 * netlink_socket = socket(AF_NETLINK, socket_type, netlink_family);
 *
 * Description:
 * Netlink is used to transfer information between the kernel and user-space
 * processes. It consists of a standard sockets-based interface for user space
 * processes and an internal kernel API for kernel modules.
 *
 * Netlink is a datagram-oriented service. Both `SOCK_RAW` and `SOCK_DGRAM`
 * are valid values for `socket_type`. However, the netlink protocol does not
 * distinguish between datagram and raw sockets.
 *
 * `netlink_family` selects the kernel module or netlink group to communicate
 * with. Especially for Open/R's interest:
 *
 * NETLINK_ROUTE
 *      Receives routing and link updates and may be used to
 *      modify the routing tables (IPv4,  IPv6 and MPLS), IP
 *      addresses, link parameters, neighbor setups, queueing
 *      disciplines, traffic classes, and packet classifiers.
 */
/*
 * For rtnetlink reference:
 *
 * https://man7.org/linux/man-pages/man7/rtnetlink.7.html
 *
 * Synopsis:
 *
 * rtnetlink_socket = socket(AF_NETLINK, socket_type, NETLINK_ROUTE);
 *
 * Description:
 * Rtnetlink allows the kernel's routing tables to be read and altered.
 *
 * Message Types:
 * - RTM_NEWLINK, RTM_DELLINK, RTM_GETLINK
 * - RTM_NEWADDR, RTM_DELADDR, RTM_GETADDR
 * - RTM_NEWROUTE, RTM_DELROUTE, RTM_GETROUTE
 * - RTM_NEWNEIGH, RTM_DELNEIGH, RTM_GETNEIGH
 * - etc.
 */
class NetlinkMessageBase {
 public:
  NetlinkMessageBase();

  virtual ~NetlinkMessageBase();

  // construct message with type
  explicit NetlinkMessageBase(int type);

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

  virtual void
  rcvdRule(Rule&& /* rule */) {
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

  // parse IP address
  static folly::Expected<folly::IPAddress, folly::IPAddressFormatError> parseIp(
      const struct rtattr* ipAttr, unsigned char family);

 protected:
  /*
   * Add TLV(Type-Length-Value) attributes and update the length field in
   * NLMSG header.
   *
   * @params: type => data type
   *          data => data value
   *          length => data length
   *
   * @return:
   *  - EVNOBUFS: if enough buffer is not available
   *  - 0: on success
   *  - System error code: if failed to add attributes
   */
  int addAttributes(int type, const char* const data, uint32_t len);

  /*
   * Add a sub RTA(Route Attribures) inside an RTA. The length of sub RTA will
   * not be added into the NLMSG header, but will be added to the parent RTA.
   *
   * @params: rta => route attribute struct ptr
   *          type => data type
   *          data => data value
   *          length => data length
   *
   * @return: route attribute struct ptr
   */
  struct rtattr* addSubAttributes(
      struct rtattr* rta, int type, const void* data, uint32_t len) const;

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};

 private:
  // disable copy, assign constructores
  NetlinkMessageBase(NetlinkMessageBase const&) = delete;
  NetlinkMessageBase& operator=(NetlinkMessageBase const&) = delete;

  // Promise to relay the status code received from kernel
  folly::Promise<int> promise_;

  // Timestamp when message object was created
  const std::chrono::steady_clock::time_point createTs_{
      std::chrono::steady_clock::now()};
};

} // namespace openr::fbnl
