/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include <folly/IPAddress.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/nl/NetlinkMessageBase.h>
#include <openr/nl/NetlinkTypes.h>

#ifndef MPLS_IPTUNNEL_DST
#define MPLS_IPTUNNEL_DST 1
#endif

namespace openr::fbnl {

constexpr uint16_t kMaxLabels{16};
constexpr uint32_t kLabelBosShift{8};
constexpr uint32_t kLabelShift{12};
constexpr uint32_t kLabelSizeBits{20};

/**
 * Message specialization for ROUTE object
 */
class NetlinkRouteMessage final : public NetlinkMessage {
 public:
  NetlinkRouteMessage();

  ~NetlinkRouteMessage() override;

  // Override setReturnStatus. Set routePromise_ with rcvdRoutes_
  void setReturnStatus(int status) override;

  // Get future for received links in response to GET request
  folly::SemiFuture<folly::Expected<std::vector<Route>, int>>
  getRoutesSemiFuture() {
    return routePromise_.getSemiFuture();
  }

  // initiallize route message with default params
  void init(int type, uint32_t flags, const Route& route);

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
  int addRoute(const Route& route);

  // delete a route
  int deleteRoute(const Route& route);

  // add label route
  int addLabelRoute(const Route& route);

  // delete label route
  int deleteLabelRoute(const Route& route);

  // encode MPLS label, returns in network order
  static uint32_t encodeLabel(uint32_t label, bool bos);

  // process netlink route message
  static Route parseMessage(const struct nlmsghdr* nlmsg);

 private:
  // print ancillary data
  void showRtmMsg(const struct rtmsg* const hdr) const;

  // print route attribute
  void showRouteAttribute(const struct rtattr* const hdr) const;

  // print multi path attributes
  void showMultiPathAttribues(const struct rtattr* const rta) const;

  // parse IP address
  static folly::Expected<folly::IPAddress, folly::IPAddressFormatError> parseIp(
      const struct rtattr* ipAttr, unsigned char family);

  // process netlink next hops
  static std::vector<NextHop> parseNextHops(
      const struct rtattr* routeAttrMultipath, unsigned char family);

  // parse NextHop Attributes
  static void parseNextHopAttribute(
      const struct rtattr* routeAttr,
      unsigned char family,
      NextHopBuilder& nhBuilder);

  // parse MPLS labels
  static std::optional<std::vector<int32_t>> parseMplsLabels(
      const struct rtattr* routeAttr);

  // set mpls action based on nexthop fields
  static void setMplsAction(NextHopBuilder& nhBuilder, unsigned char family);

  // pointer to route message header
  struct rtmsg* rtmsg_{nullptr};

  // add set of nexthops
  int addNextHops(const Route& route);

  // Add ECMP paths
  int addMultiPathNexthop(
      std::array<char, kMaxNlPayloadSize>& nhop, const Route& route) const;

  // Add label encap
  int addPushNexthop(
      struct rtattr* rta, struct rtnexthop* rtnh, const NextHop& path) const;

  // swap or PHP
  int addSwapOrPHPNexthop(
      struct rtattr* rta, struct rtnexthop* rtnh, const NextHop& path) const;

  // POP - sends to lo I/F
  int addPopNexthop(
      struct rtattr* rta, struct rtnexthop* rtnh, const NextHop& path) const;

  // POP - sends to lo I/F
  int addIpNexthop(
      struct rtattr* rta,
      struct rtnexthop* rtnh,
      const NextHop& path,
      const Route& route) const;

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};

  // for via nexthop
  struct _NextHop {
    uint16_t addrFamily;
    char ip[16];
  } __attribute__((__packed__));

  struct _NextHopV4 {
    uint16_t addrFamily;
    char ip[4];
  } __attribute__((__packed__));

 private:
  void rcvdRoute(Route&& route) override;

  struct {
    uint8_t table{0};
    uint8_t protocol{0};
    uint8_t type{0};
  } filters_;

  folly::Promise<folly::Expected<std::vector<Route>, int>> routePromise_;
  std::vector<Route> rcvdRoutes_;
};

/**
 * Message specialization for LINK object
 */
class NetlinkLinkMessage final : public NetlinkMessage {
 public:
  NetlinkLinkMessage();

  ~NetlinkLinkMessage() override;

  // Override setReturnStatus. Set linkPromise_ with rcvdLinks_
  void setReturnStatus(int status) override;

  // Get future for received links in response to GET request
  folly::SemiFuture<folly::Expected<std::vector<Link>, int>>
  getLinksSemiFuture() {
    return linkPromise_.getSemiFuture();
  }

  // initiallize link message with default params
  void init(int type, uint32_t flags);

  // parse Netlink Link message
  static Link parseMessage(const struct nlmsghdr* nlh);

 private:
  // pointer to link message header
  struct ifinfomsg* ifinfomsg_{nullptr};

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};

  void rcvdLink(Link&& link) override;

  folly::Promise<folly::Expected<std::vector<Link>, int>> linkPromise_;
  std::vector<Link> rcvdLinks_;
};

/**
 * Message specialization for ADDR object
 */
class NetlinkAddrMessage final : public NetlinkMessage {
 public:
  NetlinkAddrMessage();

  ~NetlinkAddrMessage() override;

  // Override setReturnStatus. Set addrPromise_ with rcvdAddrs_
  void setReturnStatus(int status) override;

  // Get future for received addresses in response to GET request
  folly::SemiFuture<folly::Expected<std::vector<IfAddress>, int>>
  getAddrsSemiFuture() {
    return addrPromise_.getSemiFuture();
  }

  // initiallize address message with default params
  void init(int type);

  // parse Netlink Address message
  static IfAddress parseMessage(const struct nlmsghdr* nlh);

  // create netlink message to add/delete interface address
  // type - RTM_NEWADDR or RTM_DELADDR
  int addOrDeleteIfAddress(const IfAddress& ifAddr, const int type);

 private:
  // pointer to interface message header
  struct ifaddrmsg* ifaddrmsg_{nullptr};

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};

  void rcvdIfAddress(IfAddress&& ifAddr) override;

  folly::Promise<folly::Expected<std::vector<IfAddress>, int>> addrPromise_;
  std::vector<IfAddress> rcvdAddrs_;
};

/**
 * Message specialization for NEIGHBOR object
 */
class NetlinkNeighborMessage final : public NetlinkMessage {
 public:
  NetlinkNeighborMessage();

  ~NetlinkNeighborMessage() override;

  // Override setReturnStatus. Set neighborPromise_ with rcvdNeighbors_
  void setReturnStatus(int status) override;

  // Get future for received neighbors in response to GET request
  folly::SemiFuture<folly::Expected<std::vector<Neighbor>, int>>
  getNeighborsSemiFuture() {
    return neighborPromise_.getSemiFuture();
  }

  // initiallize neighbor message with default params
  void init(int type, uint32_t flags);

  // parse Netlink Neighbor message
  static Neighbor parseMessage(const struct nlmsghdr* nlh);

 private:
  // pointer to neighbor message header
  struct ndmsg* ndmsg_{nullptr};

  // pointer to the netlink message header
  struct nlmsghdr* msghdr_{nullptr};

  void rcvdNeighbor(Neighbor&& ifAddr) override;

  folly::Promise<folly::Expected<std::vector<Neighbor>, int>> neighborPromise_;
  std::vector<Neighbor> rcvdNeighbors_;
};

} // namespace openr::fbnl
