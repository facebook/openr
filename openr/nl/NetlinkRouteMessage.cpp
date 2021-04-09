/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkRouteMessage.h>

namespace openr::fbnl {

NetlinkRouteMessage::NetlinkRouteMessage() : NetlinkMessageBase() {}

NetlinkRouteMessage::~NetlinkRouteMessage() {
  CHECK(routePromise_.isFulfilled());
}

void
NetlinkRouteMessage::rcvdRoute(Route&& route) {
  //
  // Implement application side filters for table and protocol if specified
  //

  if (filters_.table && filters_.table != route.getRouteTable()) {
    return; // ignore the route
  }

  if (filters_.protocol && filters_.protocol != route.getProtocolId()) {
    return; // ignore the route
  }

  if (filters_.type && filters_.type != route.getType()) {
    return; // ignore the route
  }

  NextHopSet reversedMplsLabelNhs;
  bool reverted = false;
  for (auto nh : route.getNextHops()) {
    auto pushLabels = nh.getPushLabels();
    // reverse the push labels becasue thrift API definition.
    if (pushLabels.has_value()) {
      reverted = true;
      std::reverse(pushLabels.value().begin(), pushLabels.value().end());
      nh.setPushLabels(pushLabels.value());
    }
    reversedMplsLabelNhs.insert(nh);
  }
  if (reverted) {
    route.setNextHops(reversedMplsLabelNhs);
  }

  rcvdRoutes_.emplace_back(std::move(route));
}

void
NetlinkRouteMessage::setReturnStatus(int status) {
  if (status == 0) {
    routePromise_.setValue(std::move(rcvdRoutes_));
  } else {
    routePromise_.setValue(folly::makeUnexpected(status));
  }

  NetlinkMessageBase::setReturnStatus(status);
}

void
NetlinkRouteMessage::init(int type, uint32_t rtFlags, const Route& route) {
  if (type != RTM_NEWROUTE && type != RTM_DELROUTE && type != RTM_GETROUTE) {
    LOG(ERROR) << "Incorrect netlink message type";
    return;
  }
  // initialize netlink header
  msghdr_->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
  msghdr_->nlmsg_type = type;
  msghdr_->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

  if (type == RTM_GETROUTE) {
    // Get routes matching subsequent criteria specified below
    msghdr_->nlmsg_flags |= NLM_F_DUMP;
    // NOTE - Only `rtmsg_->rtm_family` will be used by kernel as filter
    // parameter. Other parameters such as table, protocol, scope, type will
    // need to be filtered on user side.
    filters_.table = route.getRouteTable();
    filters_.type = route.getType();
    filters_.protocol = route.getProtocolId();

    VLOG(3) << "RTM_GETROUTE "
            << " table=" << static_cast<int>(filters_.table)
            << " type=" << static_cast<int>(filters_.type)
            << " protocol=" << static_cast<int>(filters_.protocol)
            << " family=" << static_cast<int>(route.getFamily());
  }

  if (type == RTM_NEWROUTE) {
    // We create new route or replace existing
    msghdr_->nlmsg_flags |= NLM_F_CREATE;
    msghdr_->nlmsg_flags |= NLM_F_REPLACE;
  }

  // intialize the route meesage header
  auto nlmsgAlen = NLMSG_ALIGN(sizeof(struct nlmsghdr));
  rtmsg_ = reinterpret_cast<struct rtmsg*>((char*)msghdr_ + nlmsgAlen);

  // Initialize values from route object or function params
  rtmsg_->rtm_table = route.getRouteTable();
  rtmsg_->rtm_protocol = route.getProtocolId();
  rtmsg_->rtm_scope = route.getScope();
  rtmsg_->rtm_type = route.getType();
  rtmsg_->rtm_family = route.getFamily();
  rtmsg_->rtm_src_len = 0;
  rtmsg_->rtm_tos = 0;
  rtmsg_->rtm_flags = rtFlags;

  auto rtFlag = route.getFlags();
  if (rtFlag.has_value()) {
    rtmsg_->rtm_flags |= rtFlag.value();
  }
}

uint32_t
NetlinkRouteMessage::encodeLabel(uint32_t label, bool bos) {
  if (label > 0xFFFFF) {
    LOG(ERROR) << "Invalid label 0x" << std::hex << label;
    label = 0;
  }
  uint32_t encodeLabel = htonl(label << kLabelShift);
  if (bos) {
    encodeLabel |= htonl(1 << kLabelBosShift);
  }
  return encodeLabel;
}

/*
 * Util function to add IPv4/IPv6 nexthops for route.
 *
 * rtnetlink.h defines "struct rtnexthop" to describe all necessary nexthop
 * information, i.e. parameters of path to a destination via this nexthop.
 *
 *  struct rtnexthop {
 *    unsigned short      rtnh_len;
 *    unsigned char       rtnh_flags;
 *    unsigned char       rtnh_hops;
 *    int                 rtnh_ifindex;
 *  };
 */
int
NetlinkRouteMessage::addIpNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const NextHop& path,
    const Route& route) const {
  rtnh->rtnh_len = sizeof(*rtnh);

  // specify rtnh_ifindex
  if (path.getIfIndex().has_value()) {
    rtnh->rtnh_ifindex = path.getIfIndex().value();
  }

  // specify rtnh_flags
  //
  //    RTNH_F_DEAD         1   /* Nexthop is dead (used by multipath)  */
  //    RTNH_F_PERVASIVE    2   /* Do recursive gateway lookup  */
  //    RTNH_F_ONLINK       4   /* Gateway is forced on link    */
  //
  rtnh->rtnh_flags = 0;

  // Set weight if specified - This is only applicable for IP nexthops
  rtnh->rtnh_hops = 0;
  if (path.getWeight()) {
    rtnh->rtnh_hops = path.getWeight() - 1;
  }

  // speficy RTA_VIA or RTA_GATEWAY
  auto const via = path.getGateway();
  if (not via.has_value()) {
    if (route.getType() == RTN_MULTICAST || route.getScope() == RT_SCOPE_LINK) {
      return 0;
    }
    LOG(ERROR) << "Nexthop address not provided";
    return EINVAL;
  }

  // In case of route family is different from the NH gateway family,
  // it requires to specify `RTA_VIA` field instead of `RTA_GATEWAY`.
  const auto& gw = via.value();
  if (isV4RouteOverV6Nexthop(route, path)) {
    // RTA_VIA
    struct _NextHop nh;
    nh.addrFamily = path.getFamily();
    uint32_t nhLen = nh.addrFamily == AF_INET ? sizeof(struct _NextHopV4)
                                              : sizeof(struct _NextHop);
    memcpy(nh.ip, reinterpret_cast<const char*>(gw.bytes()), gw.byteCount());
    if (addSubAttributes(
            rta, RTA_VIA, reinterpret_cast<const char*>(&nh), nhLen) ==
        nullptr) {
      return ENOBUFS;
    }

    // update length in rtnexthop
    rtnh->rtnh_len += nhLen + sizeof(struct rtattr);
  } else {
    // RTA_GATEWAY
    if (addSubAttributes(rta, RTA_GATEWAY, gw.bytes(), gw.byteCount()) ==
        nullptr) {
      return ENOBUFS;
    }

    // update length in rtnexthop
    rtnh->rtnh_len += gw.byteCount() + sizeof(struct rtattr);
  }
  return 0;
}

int
NetlinkRouteMessage::addSwapOrPHPNexthop(
    struct rtattr* rta, struct rtnexthop* rtnh, const NextHop& path) const {
  rtnh->rtnh_len = sizeof(*rtnh);
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  // Weights are not supported for MPLS routes
  if (path.getWeight()) {
    LOG(ERROR) << "Weight is not supported for MPLS PHP or SWAP next-hop";
    return -EINVAL;
  }

  // Set interface index if available, else let the kernel resolve it
  if (path.getIfIndex().has_value()) {
    rtnh->rtnh_ifindex = path.getIfIndex().value();
  }

  // add the following subattributes within RTA_MULTIPATH
  size_t prevLen = rta->rta_len;

  // labels.size() = 0 implies PHP
  auto maybeLabel = path.getSwapLabel();
  if (maybeLabel.has_value()) {
    struct mpls_label swapLabel;
    swapLabel.entry = encodeLabel(maybeLabel.value(), true);

    if (addSubAttributes(
            rta,
            RTA_NEWDST,
            reinterpret_cast<const char*>(&swapLabel),
            sizeof(swapLabel)) == nullptr) {
      return ENOBUFS;
    }
  }
  // update rtnh len
  rtnh->rtnh_len += rta->rta_len - prevLen;

  // RTA_VIA
  struct _NextHop via;
  via.addrFamily = path.getFamily();
  auto gw = path.getGateway().value();
  int viaLen{sizeof(struct _NextHop)};
  if (via.addrFamily == AF_INET) {
    viaLen = sizeof(struct _NextHopV4);
  }
  memcpy(via.ip, reinterpret_cast<const char*>(gw.bytes()), gw.byteCount());
  if (addSubAttributes(
          rta, RTA_VIA, reinterpret_cast<const char*>(&via), viaLen) ==
      nullptr) {
    return ENOBUFS;
  }

  // update length in rtnexthop
  rtnh->rtnh_len += viaLen + sizeof(struct rtattr);
  return 0;
}

int
NetlinkRouteMessage::addPopNexthop(
    struct rtattr* rta, struct rtnexthop* rtnh, const NextHop& path) const {
  // for each next hop add the ENCAP
  rtnh->rtnh_len = sizeof(*rtnh);
  if (!path.getIfIndex().has_value()) {
    LOG(ERROR) << "Loopback interface index not provided for POP";
    return -EINVAL;
  }
  // Weights are not supported for MPLS routes
  if (path.getWeight()) {
    LOG(ERROR) << "Weight is not supported for MPLS POP next-hop";
    return -EINVAL;
  }

  rtnh->rtnh_ifindex = path.getIfIndex().value();
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  int oif = rtnh->rtnh_ifindex;
  if (addSubAttributes(
          rta, RTA_OIF, reinterpret_cast<const char*>(&oif), sizeof(oif)) ==
      nullptr) {
    return ENOBUFS;
  }

  // update length in rtnexthop
  rtnh->rtnh_len += sizeof(oif) + sizeof(struct rtattr);
  return 0;
}

int
NetlinkRouteMessage::addPushNexthop(
    struct rtattr* rta, struct rtnexthop* rtnh, const NextHop& path) const {
  // fill the OIF
  rtnh->rtnh_len = sizeof(*rtnh);
  rtnh->rtnh_flags = 0;

  // Set weight if specified - This is only applicable for IP nexthops
  rtnh->rtnh_hops = 0;
  if (path.getWeight()) {
    rtnh->rtnh_hops = path.getWeight() - 1;
  }

  // Set interface index if available, else let the kernel resolve it
  if (path.getIfIndex().has_value()) {
    rtnh->rtnh_ifindex = path.getIfIndex().value(); /* interface index */
  }

  // add the following subattributes within RTA_MULTIPATH
  size_t prevLen = rta->rta_len;

  // RTA_ENCAP sub attribute
  struct rtattr* rtaEncap = addSubAttributes(rta, RTA_ENCAP, nullptr, 0);
  if (rtaEncap == nullptr) {
    return ENOBUFS;
  }

  // MPLS_IP_TUNNEL_DST sub attribute
  std::array<struct mpls_label, kMaxLabels> mplsLabel;
  size_t i = 0;
  auto labels = path.getPushLabels();
  if (!labels.has_value()) {
    LOG(ERROR) << "Labels not provided for PUSH action";
    return EINVAL;
  }
  // abort immediately to bring attention
  CHECK_LE(labels.value().size(), kMaxLabels);
  std::reverse(labels.value().begin(), labels.value().end());
  for (auto label : labels.value()) {
    bool bos = i == labels.value().size() - 1 ? true : false;
    mplsLabel[i++].entry = encodeLabel(label, bos);
  }
  size_t totalSize = labels.value().size() * sizeof(struct mpls_label);
  if (addSubAttributes(rta, MPLS_IPTUNNEL_DST, &mplsLabel, totalSize) ==
      nullptr) {
    return ENOBUFS;
  };

  // update RTA ENCAP sub attribute length
  rtaEncap->rta_len = RTA_ALIGN(rta->rta_len) - prevLen;

  // RTA_ENCAP_TYPE sub attribute
  uint16_t encapType = LWTUNNEL_ENCAP_MPLS;
  if (addSubAttributes(rta, RTA_ENCAP_TYPE, &encapType, sizeof(encapType)) ==
      nullptr) {
    return ENOBUFS;
  };

  // update rtnh len
  rtnh->rtnh_len += rta->rta_len - prevLen;

  // RTA_GATEWAY
  auto const via = path.getGateway();
  if (!via.has_value()) {
    LOG(ERROR) << "Nexthop IP not provided";
    return EINVAL;
  }
  if (addSubAttributes(
          rta, RTA_GATEWAY, via.value().bytes(), via.value().byteCount()) ==
      nullptr) {
    return ENOBUFS;
  };

  // update length in rtnexthop
  rtnh->rtnh_len += via.value().byteCount() + sizeof(struct rtattr);
  return 0;
}

int
NetlinkRouteMessage::addNextHops(const Route& route) {
  std::array<char, kMaxNlPayloadSize> nhop = {};
  int status{0};
  if (route.getNextHops().size()) {
    if ((status = addMultiPathNexthop(nhop, route))) {
      return status;
    }

    // copy the encap info into NLMSG payload
    const char* const data = reinterpret_cast<const char*>(
        RTA_DATA(reinterpret_cast<struct rtattr*>(nhop.data())));
    int payloadLen = RTA_PAYLOAD(reinterpret_cast<struct rtattr*>(nhop.data()));
    if ((status = addAttributes(RTA_MULTIPATH, data, payloadLen))) {
      return status;
    }
    // print attributes when log level is enabled
    showMultiPathAttributes(reinterpret_cast<struct rtattr*>(nhop.data()));
  }

  // print attributes when log level is enabled
  showRtmMsg(rtmsg_);

  return 0;
}

int
NetlinkRouteMessage::addMultiPathNexthop(
    std::array<char, kMaxNlPayloadSize>& nhop, const Route& route) const {
  // Add [RTA_MULTIPATH - label, via, dev][RTA_ENCAP][RTA_ENCAP_TYPE]
  struct rtattr* rta = reinterpret_cast<struct rtattr*>(nhop.data());

  // MULTIPATH
  rta->rta_type = RTA_MULTIPATH;
  rta->rta_len = RTA_LENGTH(0);
  struct rtnexthop* rtnh = reinterpret_cast<struct rtnexthop*>(RTA_DATA(rta));

  int result{0};
  const auto& paths = route.getNextHops();
  for (const auto& path : paths) {
    rtnh->rtnh_len = sizeof(*rtnh);
    rta->rta_len += rtnh->rtnh_len;
    auto action = path.getLabelAction();

    if (action.has_value()) {
      switch (action.value()) {
      case thrift::MplsActionCode::PUSH:
        result = addPushNexthop(rta, rtnh, path);
        break;

      case thrift::MplsActionCode::SWAP:
      case thrift::MplsActionCode::PHP:
        result = addSwapOrPHPNexthop(rta, rtnh, path);
        break;

      case thrift::MplsActionCode::POP_AND_LOOKUP:
        result = addPopNexthop(rta, rtnh, path);
        break;

      default:
        LOG(ERROR) << "Unknown action";
        return EINVAL;
      }
    } else {
      result = addIpNexthop(rta, rtnh, path, route);
    }

    if (result) {
      return result;
    }
    rtnh = RTNH_NEXT(rtnh);
  }
  return result;
}

folly::Expected<folly::IPAddress, folly::IPAddressFormatError>
NetlinkRouteMessage::parseIp(
    const struct rtattr* ipAttr, unsigned char family) {
  if (family == AF_INET) {
    struct in_addr* addr4 = reinterpret_cast<in_addr*> RTA_DATA(ipAttr);
    return folly::IPAddressV4::fromLong(addr4->s_addr);
  } else if (family == AF_INET6) {
    struct in6_addr* addr6 = reinterpret_cast<in6_addr*> RTA_DATA(ipAttr);
    return folly::IPAddressV6::tryFromBinary(
        folly::ByteRange(reinterpret_cast<const uint8_t*>(addr6->s6_addr), 16));
  } else {
    return makeUnexpected(folly::IPAddressFormatError::UNSUPPORTED_ADDR_FAMILY);
  }
}

std::optional<std::vector<int32_t>>
NetlinkRouteMessage::parseMplsLabels(const struct rtattr* routeAttr) {
  const struct rtattr* mplsAttr =
      reinterpret_cast<struct rtattr*> RTA_DATA(routeAttr);
  int mplsAttrLen = RTA_PAYLOAD(routeAttr);
  do {
    switch (mplsAttr->rta_type) {
    case MPLS_IPTUNNEL_DST: {
      std::vector<int32_t> pushLabels{};
      const int32_t* mplsLabels = reinterpret_cast<int32_t*> RTA_DATA(mplsAttr);
      // each mpls label entry is 32 bits (20 bit label, and other fields)
      int numLabels = RTA_PAYLOAD(mplsAttr) / sizeof(struct mpls_label);
      // Check if number of labels does not exceed max label count
      CHECK_LE(numLabels, kMaxLabels);
      for (int i = 0; i < numLabels; i++) {
        // decode mpls label
        pushLabels.emplace_back(ntohl(mplsLabels[i]) >> kLabelShift);
      }
      return pushLabels;
    } break;
    }
    mplsAttr = RTA_NEXT(mplsAttr, mplsAttrLen);
  } while (RTA_OK(mplsAttr, mplsAttrLen));
  return std::nullopt;
}

void
NetlinkRouteMessage::parseNextHopAttribute(
    const struct rtattr* routeAttr,
    unsigned char family,
    NextHopBuilder& nhBuilder) {
  switch (routeAttr->rta_type) {
  case RTA_GATEWAY: {
    // Gateway address
    auto ipAddress = parseIp(routeAttr, family);
    if (ipAddress.hasValue()) {
      nhBuilder.setGateway(ipAddress.value());
    }
  } break;

  // via nexthop used for MPLS PHP or SWAP
  case RTA_VIA: {
    folly::Expected<folly::IPAddress, folly::IPAddressFormatError> ipAddress;
    if (routeAttr->rta_len > 16) {
      // IPv6 nexthop address
      struct _NextHop* via =
          reinterpret_cast<struct _NextHop*> RTA_DATA(routeAttr);
      ipAddress = folly::IPAddressV6::tryFromBinary(
          folly::ByteRange(reinterpret_cast<const uint8_t*>(via->ip), 16));
    } else {
      // IPv4 nexthop address
      struct _NextHopV4* via =
          reinterpret_cast<struct _NextHopV4*> RTA_DATA(routeAttr);
      ipAddress = folly::IPAddressV4::tryFromBinary(
          folly::ByteRange(reinterpret_cast<const uint8_t*>(via->ip), 4));
    }
    if (ipAddress.hasValue()) {
      nhBuilder.setGateway(ipAddress.value());
    }
  } break;

  case RTA_OIF: {
    // Output interface index
    nhBuilder.setIfIndex(*(reinterpret_cast<int*> RTA_DATA(routeAttr)));
  } break;

  case RTA_ENCAP: {
    // MPLS Labels
    auto pushLabels = parseMplsLabels(routeAttr);
    if (pushLabels.has_value()) {
      nhBuilder.setPushLabels(pushLabels.value());
    }
  } break;

  case RTA_NEWDST: {
    // Swap Label
    const auto swapLabel =
        reinterpret_cast<struct mpls_label*> RTA_DATA(routeAttr);
    // Decode swap label
    nhBuilder.setSwapLabel(ntohl(swapLabel->entry) >> kLabelShift);
  } break;
  }
}

void
NetlinkRouteMessage::setMplsAction(
    NextHopBuilder& nhBuilder, unsigned char family) {
  // Inferring MPLS action from nexthop fields
  if (nhBuilder.getPushLabels() != std::nullopt) {
    nhBuilder.setLabelAction(thrift::MplsActionCode::PUSH);
  } else if (family == AF_MPLS) {
    if (nhBuilder.getGateway() != std::nullopt &&
        nhBuilder.getSwapLabel() != std::nullopt) {
      nhBuilder.setLabelAction(thrift::MplsActionCode::SWAP);
    } else if (
        nhBuilder.getGateway() != std::nullopt &&
        nhBuilder.getSwapLabel() == std::nullopt) {
      nhBuilder.setLabelAction(thrift::MplsActionCode::PHP);
    } else {
      nhBuilder.setLabelAction(thrift::MplsActionCode::POP_AND_LOOKUP);
    }
  }
}

bool
NetlinkRouteMessage::isV4RouteOverV6Nexthop(
    const Route& route, const NextHop& nh) {
  return static_cast<int>(route.getFamily()) == AF_INET and
      static_cast<int>(nh.getFamily()) == AF_INET6;
}

Route
NetlinkRouteMessage::parseMessage(const struct nlmsghdr* nlmsg) {
  RouteBuilder routeBuilder;
  // For single next hop in the route
  NextHopBuilder nhBuilder;
  bool singleNextHopFlag{true};

  const struct rtmsg* const routeEntry =
      reinterpret_cast<struct rtmsg*>(NLMSG_DATA(nlmsg));

  const bool isValid = nlmsg->nlmsg_type == RTM_NEWROUTE ? true : false;
  routeBuilder.setRouteTable(routeEntry->rtm_table)
      .setFlags(routeEntry->rtm_flags)
      .setProtocolId(routeEntry->rtm_protocol)
      .setType(routeEntry->rtm_type)
      .setScope(routeEntry->rtm_scope)
      .setValid(isValid);

  const struct rtattr* routeAttr;
  auto routeAttrLen = RTM_PAYLOAD(nlmsg);
  // process all route attributes
  for (routeAttr = RTM_RTA(routeEntry); RTA_OK(routeAttr, routeAttrLen);
       routeAttr = RTA_NEXT(routeAttr, routeAttrLen)) {
    switch (routeAttr->rta_type) {
    case RTA_DST: {
      if (routeEntry->rtm_family == AF_MPLS) {
        // Parse MPLS label
        const auto mplsLabel =
            reinterpret_cast<struct mpls_label*> RTA_DATA(routeAttr);
        // decode label
        routeBuilder.setMplsLabel(ntohl(mplsLabel->entry) >> kLabelShift);
      } else {
        // Parse destination IP address
        auto ipAddress = parseIp(routeAttr, routeEntry->rtm_family);
        if (ipAddress.hasValue()) {
          folly::CIDRNetwork prefix = std::make_pair(
              ipAddress.value(), (uint8_t)routeEntry->rtm_dst_len);
          routeBuilder.setDestination(prefix);
        }
      }
    } break;

    case RTA_PRIORITY: {
      // parse route priority
      routeBuilder.setPriority(*(reinterpret_cast<int*> RTA_DATA(routeAttr)));
    } break;

    // Nexthop attributes
    case RTA_GATEWAY:
    case RTA_OIF:
    case RTA_VIA:
    case RTA_ENCAP:
    case RTA_NEWDST: {
      parseNextHopAttribute(routeAttr, routeEntry->rtm_family, nhBuilder);
    } break;

    // If there are multiple nexthops in the route, the nexthop attributes
    // are subattributes in RTA_MULTIPATH
    case RTA_MULTIPATH: {
      singleNextHopFlag = false;
      auto nextHops = parseNextHops(routeAttr, routeEntry->rtm_family);
      for (auto& nh : nextHops) {
        // don't add empty nexthop
        if (nh.getGateway().has_value() || nh.getIfIndex().has_value()) {
          routeBuilder.addNextHop(nh);
        }
      }
    } break;
    }
  }

  if (singleNextHopFlag) {
    setMplsAction(nhBuilder, routeEntry->rtm_family);
    // add single nexthop
    auto nh = nhBuilder.build();
    // don't add empty nexthop
    if (nh.getGateway().has_value() || nh.getIfIndex().has_value()) {
      routeBuilder.addNextHop(nh);
    }
  }

  // Default route might be missing RTA_DST attribute. So explicitly set
  // destination here.
  if (routeEntry->rtm_dst_len == 0 && routeBuilder.getFamily() == AF_UNSPEC) {
    if (routeEntry->rtm_family == AF_INET) {
      routeBuilder.setDestination({folly::IPAddressV4("0.0.0.0"), 0});
    } else if (routeEntry->rtm_family == AF_INET6) {
      routeBuilder.setDestination({folly::IPAddressV6("::"), 0});
    }
  }

  auto route = routeBuilder.build();
  VLOG(3) << "Netlink parsed route message. " << route.str();
  return route;
}

std::vector<NextHop>
NetlinkRouteMessage::parseNextHops(
    const struct rtattr* routeAttrMP, unsigned char family) {
  std::vector<NextHop> nextHops;
  struct rtnexthop* nh =
      reinterpret_cast<struct rtnexthop*> RTA_DATA(routeAttrMP);

  int nhLen = RTA_PAYLOAD(routeAttrMP);
  do {
    NextHopBuilder nhBuilder;
    nhBuilder.setIfIndex(nh->rtnh_ifindex);
    const struct rtattr* routeAttr;
    auto routeAttrLen = nh->rtnh_len - sizeof(*nh);
    // process all route attributes
    for (routeAttr = RTNH_DATA(nh); RTA_OK(routeAttr, routeAttrLen);
         routeAttr = RTA_NEXT(routeAttr, routeAttrLen)) {
      switch (routeAttr->rta_type) {
      // Nexthop attributes
      case RTA_GATEWAY:
      case RTA_OIF:
      case RTA_VIA:
      case RTA_ENCAP:
      case RTA_NEWDST: {
        parseNextHopAttribute(routeAttr, family, nhBuilder);
      } break;
      }
    }
    // Set the next-hop weight if available
    if (family != AF_MPLS) {
      nhBuilder.setWeight(nh->rtnh_hops + 1);
    }
    setMplsAction(nhBuilder, family);
    auto nexthop = nhBuilder.build();
    nextHops.emplace_back(nexthop);
    nhLen -= NLMSG_ALIGN(nh->rtnh_len);
    nh = RTNH_NEXT(nh);
  } while (RTNH_OK(nh, nhLen));
  return nextHops;
}

int
NetlinkRouteMessage::addRoute(const Route& route) {
  auto const& pfix = route.getDestination();
  auto ip = std::get<0>(pfix);
  auto plen = std::get<1>(pfix);
  auto addressFamily = route.getFamily();

  if (addressFamily != AF_INET && addressFamily != AF_INET6) {
    LOG(ERROR) << "Address family is not AF_INET or AF_INET6";
    return EINVAL;
  }

  init(RTM_NEWROUTE, 0, route);

  rtmsg_->rtm_family = addressFamily;
  rtmsg_->rtm_dst_len = plen; /* netmask */
  const char* const ipptr = reinterpret_cast<const char*>(ip.bytes());
  int status{0};
  if ((status = addAttributes(RTA_DST, ipptr, ip.byteCount()))) {
    return status;
  }

  // set up admin distance if priority.
  if (route.getPriority()) {
    const uint32_t adminDistance = route.getPriority().value();
    const char* const adPtr = reinterpret_cast<const char*>(&adminDistance);
    if ((status = addAttributes(RTA_PRIORITY, adPtr, sizeof(uint32_t)))) {
      return status;
    }
  }

  return addNextHops(route);
}

int
NetlinkRouteMessage::deleteRoute(const Route& route) {
  auto const& pfix = route.getDestination();
  auto addressFamily = route.getFamily();
  if (addressFamily != AF_INET && addressFamily != AF_INET6) {
    LOG(ERROR) << "Invalid address family. Expected AF_INET or AF_INET6";
    return EINVAL;
  }
  init(RTM_DELROUTE, 0, route);

  auto plen = std::get<1>(pfix);
  auto ip = std::get<0>(pfix);
  rtmsg_->rtm_family = addressFamily;
  rtmsg_->rtm_dst_len = plen; /* netmask */
  const char* const ipptr = reinterpret_cast<const char*>(ip.bytes());
  return addAttributes(RTA_DST, ipptr, ip.byteCount());
}

int
NetlinkRouteMessage::addLabelRoute(const Route& route) {
  init(RTM_NEWROUTE, 0, route);
  rtmsg_->rtm_family = AF_MPLS;
  rtmsg_->rtm_dst_len = kLabelSizeBits;
  rtmsg_->rtm_flags = 0;
  struct mpls_label mlabel;

  if (route.getFamily() != AF_MPLS) {
    LOG(ERROR) << "Invalid address family. Expected AF_MPLS";
    return EINVAL;
  }

  auto label = route.getMplsLabel();
  if (!label.has_value()) {
    LOG(ERROR) << "Missing label";
    return EINVAL;
  }

  mlabel.entry = encodeLabel(label.value(), true);
  int status{0};
  if ((status = addAttributes(
           RTA_DST,
           reinterpret_cast<const char*>(&mlabel),
           sizeof(mpls_label)))) {
    return status;
  }

  return addNextHops(route);
}

int
NetlinkRouteMessage::deleteLabelRoute(const Route& route) {
  init(RTM_DELROUTE, 0, route);
  rtmsg_->rtm_family = AF_MPLS;
  rtmsg_->rtm_dst_len = kLabelSizeBits;
  rtmsg_->rtm_flags = 0;
  struct mpls_label mlabel;
  auto label = route.getMplsLabel();
  if (!label.has_value()) {
    LOG(ERROR) << "Label not provided";
    return EINVAL;
  }
  mlabel.entry = encodeLabel(label.value(), true);
  return addAttributes(
      RTA_DST, reinterpret_cast<const char*>(&mlabel), sizeof(mpls_label));
}

/* ATTN: debugging util function. DO NOT REMOVE */
void
NetlinkRouteMessage::showRtmMsg(const struct rtmsg* const hdr) {
  VLOG(3) << "Route message data"
          << "\n\trtm_family    =>  " << +hdr->rtm_family
          << "\n\trtm_dst_len   =>  " << +hdr->rtm_dst_len
          << "\n\trtm_src_len   =>  " << +hdr->rtm_src_len
          << "\n\trtm_tos       =>  " << +hdr->rtm_tos
          << "\n\trtm_table     =>  " << +hdr->rtm_table
          << "\n\trtm_protocol  =>  " << +hdr->rtm_protocol
          << "\n\trtm_scope     =>  " << +hdr->rtm_scope
          << "\n\trtm_type      =>  " << +hdr->rtm_type
          << "\n\trtm_flags     =>  " << std::hex << hdr->rtm_flags;
}

void
NetlinkRouteMessage::showMultiPathAttributes(const struct rtattr* const rta) {
  struct rtnexthop* rtnh = reinterpret_cast<struct rtnexthop*>(RTA_DATA(rta));
  int nhLen = RTA_PAYLOAD(rta);

  VLOG(3) << "Multi-path nexthop data"
          << "\n\trtnh_len      => " << rtnh->rtnh_len
          << "\n\trtnh_flags    => " << static_cast<size_t>(rtnh->rtnh_flags)
          << "\n\trtnh_hops     => " << static_cast<size_t>(rtnh->rtnh_hops)
          << "\n\trtnh_ifindex  => " << rtnh->rtnh_ifindex;

  do {
    const struct rtattr* attr;
    auto attrLen = rtnh->rtnh_len;
    for (attr = RTNH_DATA(rtnh); RTA_OK(attr, attrLen);
         attr = RTA_NEXT(attr, attrLen)) {
      VLOG(3) << "Nexthop attributes:"
              << "\n\trta_len       => " << attr->rta_len
              << "\n\trta_type      => " << attr->rta_type;
    }
    nhLen -= NLMSG_ALIGN(rtnh->rtnh_len);
    rtnh = RTNH_NEXT(rtnh);
  } while (RTNH_OK(rtnh, nhLen));
}

} // namespace openr::fbnl
