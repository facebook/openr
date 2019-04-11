/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <linux/lwtunnel.h>
#include <linux/mpls.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>

#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/nl/NetlinkMessage.h>
#include <openr/nl/NetlinkTypes.h>

#ifndef MPLS_IPTUNNEL_DST
#define MPLS_IPTUNNEL_DST 1
#endif

namespace openr {
namespace Netlink {
constexpr uint16_t kMaxLabels{4};
constexpr uint32_t kLabelBosShift{8};
constexpr uint32_t kLabelShift{12};
constexpr uint32_t kLabelMask{0xFFFFF000};
constexpr uint32_t kLabelSizeBits{20};

class NetlinkRouteMessage final : public NetlinkMessage {
 public:
  NetlinkRouteMessage();

  // initiallize with default params
  void init(int type, uint32_t flags, const openr::fbnl::Route& route);

  friend std::ostream&
  operator<<(std::ostream& out, NetlinkRouteMessage const& msg) {
    out << "\nMessage type:     " << msg.msghdr_->nlmsg_type
        << "\nMessage length:   " << msg.msghdr_->nlmsg_len
        << "\nMessage flags:    " << std::hex << msg.msghdr_->nlmsg_flags
        << "\nMessage sequence: " << msg.msghdr_->nlmsg_seq
        << "\nMessage pid:      " << msg.msghdr_->nlmsg_pid << std::endl;
    return out;
  }

  // add a unicast route
  ResultCode addRoute(const openr::fbnl::Route& route);

  // delete a route
  ResultCode deleteRoute(const openr::fbnl::Route& route);

  // add label route
  ResultCode addLabelRoute(const openr::fbnl::Route& route);

  // delete label route
  ResultCode deleteLabelRoute(const openr::fbnl::Route& route);

  // encode MPLS label, returns in network order
  uint32_t encodeLabel(uint32_t label, bool bos) const;

 private:
  // print ancillary data
  void showRtmMsg(const struct rtmsg* const hdr) const;

  // print route attribute
  void showRouteAttribute(const struct rtattr* const hdr) const;

  // print multi path attributes
  void showMultiPathAttribues(const struct rtattr* const rta) const;

  // process netlink route message
  void parseMessage() const;

  // pointer to route meesage header
  struct rtmsg* rtmsg_{nullptr};

  // add set of nexthops
  ResultCode addNextHops(const openr::fbnl::Route& route);

  // Add ECMP paths
  ResultCode addMultiPathNexthop(
      std::array<char, kMaxNhopPayloadSize>& nhop,
      const openr::fbnl::Route& route) const;

  // Add label encap
  ResultCode addLabelNexthop(
      struct rtattr* rta,
      struct rtnexthop* rtnh,
      const openr::fbnl::NextHop& path) const;

  // swap or PHP
  ResultCode addSwapOrPHPNexthop(
      struct rtattr* rta,
      struct rtnexthop* rtnh,
      const openr::fbnl::NextHop& path) const;

  // POP - sends to lo I/F
  ResultCode addPopNexthop(
      struct rtattr* rta,
      struct rtnexthop* rtnh,
      const openr::fbnl::NextHop& path) const;

  // POP - sends to lo I/F
  ResultCode addIpNexthop(
      struct rtattr* rta,
      struct rtnexthop* rtnh,
      const openr::fbnl::NextHop& path,
      const openr::fbnl::Route& route) const;

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};

  // for via nexthop
  struct nextHop {
    uint16_t addrFamily;
    char ip[16];
  } __attribute__((__packed__));

  struct nextHopV4 {
    uint16_t addrFamily;
    char ip[4];
  } __attribute__((__packed__));
};

} // namespace Netlink
} // namespace openr
