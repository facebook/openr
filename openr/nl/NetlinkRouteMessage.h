/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/nl/NetlinkMessageBase.h>
#include <openr/nl/NetlinkTypes.h>

extern "C" {
#include <linux/lwtunnel.h>
#include <linux/mpls.h>
#include <linux/rtnetlink.h>
}

#ifndef MPLS_IPTUNNEL_DST
#define MPLS_IPTUNNEL_DST 1
#endif

namespace openr::fbnl {

constexpr uint16_t kMaxLabels{16};
constexpr uint32_t kLabelBosShift{8};
constexpr uint32_t kLabelShift{12};
constexpr uint32_t kLabelSizeBits{20};

/**
 * Message specialization for rtnetlink ROUTE type
 *
 * For reference: https://man7.org/linux/man-pages/man7/rtnetlink.7.html
 *
 * RTM_NEWROUTE, RTM_DELROUTE, RTM_GETROUTE
 *    Create, remove, or receive information about a network
 *    route.  These messages contain an rtmsg structure with an
 *    optional sequence of rtattr structures following.  For
 *    RTM_GETROUTE, setting rtm_dst_len and rtm_src_len to 0
 *    means you get all entries for the specified routing table.
 *    For the other fields, except rtm_table and rtm_protocol, 0
 *    is the wildcard.
 *
 *    struct rtmsg {
 *        unsigned char rtm_family;   // Address family of route
 *        unsigned char rtm_dst_len;  // Length of destination
 *        unsigned char rtm_src_len;  // Length of source
 *        unsigned char rtm_tos;      // TOS filter
 *        unsigned char rtm_table;    // Routing table ID
 *        unsigned char rtm_protocol; // Routing protocol
 *        unsigned char rtm_scope;
 *        unsigned char rtm_type;
 *        unsigned int  rtm_flags;
 *    };
 */
class NetlinkRouteMessage final : public NetlinkMessageBase {
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
  // inherited class implementation
  void rcvdRoute(Route&& route) override;

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

  // util function to check if it is v4 route over v6 nexthops
  static bool isV4RouteOverV6Nexthop(const Route& route, const NextHop& nh);

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

  // DO NOT REMOVE - debugging util functions
  static void showRtmMsg(const struct rtmsg* const hdr);
  static void showMultiPathAttributes(const struct rtattr* const rta);

  //
  // Private variables for rtnetlink msg exchange
  //

  // pointer to route message header
  struct rtmsg* rtmsg_{nullptr};

  // for RTA_VIA nexthop
  struct _NextHop {
    uint16_t addrFamily;
    char ip[16];
  } __attribute__((__packed__));

  struct _NextHopV4 {
    uint16_t addrFamily;
    char ip[4];
  } __attribute__((__packed__));

  struct {
    uint8_t table{0};
    uint8_t protocol{0};
    uint8_t type{0};
  } filters_;

  // promise to be fulfilled when receiving kernel reply
  folly::Promise<folly::Expected<std::vector<Route>, int>> routePromise_;
  std::vector<Route> rcvdRoutes_;
};

} // namespace openr::fbnl
