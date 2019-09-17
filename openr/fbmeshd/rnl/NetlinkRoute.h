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
#include <net/if_arp.h>
#include <netinet/ether.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>

#include <openr/fbmeshd/rnl/NetlinkMessage.h>
#include <openr/fbmeshd/rnl/NetlinkTypes.h>
#include <openr/if/gen-cpp2/Network_types.h>

#ifndef MPLS_IPTUNNEL_DST
#define MPLS_IPTUNNEL_DST 1
#endif

namespace openr {
namespace fbnl {

constexpr uint16_t kMaxLabels{16};
constexpr uint32_t kLabelBosShift{8};
constexpr uint32_t kLabelShift{12};
constexpr uint32_t kLabelMask{0xFFFFF000};
constexpr uint32_t kLabelSizeBits{20};

class NetlinkRouteMessage final : public NetlinkMessage {
 public:
  NetlinkRouteMessage();

  // initiallize route message with default params
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

  // process netlink route message
  fbnl::Route parseMessage(const struct nlmsghdr* nlmsg) const;

 private:
  // print ancillary data
  void showRtmMsg(const struct rtmsg* const hdr) const;

  // print route attribute
  void showRouteAttribute(const struct rtattr* const hdr) const;

  // print multi path attributes
  void showMultiPathAttribues(const struct rtattr* const rta) const;

  // parse IP address
  folly::Expected<folly::IPAddress, folly::IPAddressFormatError> parseIp(
      const struct rtattr* ipAttr, unsigned char family) const;

  // process netlink next hops
  std::vector<fbnl::NextHop> parseNextHops(
      const struct rtattr* routeAttrMultipath, unsigned char family) const;

  // parse NextHop Attributes
  void parseNextHopAttribute(
      const struct rtattr* routeAttr,
      unsigned char family,
      fbnl::NextHopBuilder& nhBuilder) const;

  // parse MPLS labels
  folly::Optional<std::vector<int32_t>> parseMplsLabels(
      const struct rtattr* routeAttr) const;

  // set mpls action based on nexthop fields
  void setMplsAction(
      fbnl::NextHopBuilder& nhBuilder, unsigned char family) const;

  // pointer to route message header
  struct rtmsg* rtmsg_{nullptr};

  // add set of nexthops
  ResultCode addNextHops(const openr::fbnl::Route& route);

  // Add ECMP paths
  ResultCode addMultiPathNexthop(
      std::array<char, kMaxNlPayloadSize>& nhop,
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
  struct NextHop {
    uint16_t addrFamily;
    char ip[16];
  } __attribute__((__packed__));

  struct NextHopV4 {
    uint16_t addrFamily;
    char ip[4];
  } __attribute__((__packed__));
};

class NetlinkLinkMessage final : public NetlinkMessage {
 public:
  NetlinkLinkMessage();

  // initiallize link message with default params
  void init(int type, uint32_t flags);

  // parse Netlink Link message
  fbnl::Link parseMessage(const struct nlmsghdr* nlh) const;

 private:
  // pointer to link message header
  struct ifinfomsg* ifinfomsg_{nullptr};

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};
};

class NetlinkAddrMessage final : public NetlinkMessage {
 public:
  NetlinkAddrMessage();

  // initiallize address message with default params
  void init(int type);

  // parse Netlink Address message
  fbnl::IfAddress parseMessage(const struct nlmsghdr* nlh) const;

  // create netlink message to add/delete interface address
  // type - RTM_NEWADDR or RTM_DELADDR
  ResultCode addOrDeleteIfAddress(
      const openr::fbnl::IfAddress& ifAddr, const int type);

 private:
  // pointer to interface message header
  struct ifaddrmsg* ifaddrmsg_{nullptr};

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};
};

class NetlinkNeighborMessage final : public NetlinkMessage {
 public:
  NetlinkNeighborMessage();

  // initiallize neighbor message with default params
  void init(int type, uint32_t flags);

  // parse Netlink Neighbor message
  fbnl::Neighbor parseMessage(const struct nlmsghdr* nlh) const;

 private:
  // pointer to neighbor message header
  struct ndmsg* ndmsg_{nullptr};

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};
};

} // namespace fbnl
} // namespace openr
