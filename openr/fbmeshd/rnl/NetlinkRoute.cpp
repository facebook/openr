/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/fbmeshd/rnl/NetlinkRoute.h"
#include "openr/fbmeshd/rnl/NetlinkMessage.h"

namespace openr {
namespace rnl {

NetlinkRouteMessage::NetlinkRouteMessage() {
  // get pointer to NLMSG header
  msghdr_ = getMessagePtr();
}

void
NetlinkRouteMessage::init(
    int type, uint32_t rtFlags, const openr::rnl::Route& route) {
  if (type != RTM_NEWROUTE && type != RTM_DELROUTE && type != RTM_GETROUTE) {
    LOG(ERROR) << "Incorrect Netlink message type";
    return;
  }
  // initialize netlink header
  msghdr_->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
  msghdr_->nlmsg_type = type;
  msghdr_->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

  if (type == RTM_GETROUTE) {
    // Get all routes
    msghdr_->nlmsg_flags |= NLM_F_DUMP;
  }

  if (type != RTM_DELROUTE) {
    msghdr_->nlmsg_flags |= NLM_F_CREATE;
  }

  if (route.getType() != RTN_MULTICAST) {
    msghdr_->nlmsg_flags |= NLM_F_REPLACE;
  }

  // intialize the route meesage header
  auto nlmsgAlen = NLMSG_ALIGN(sizeof(struct nlmsghdr));
  rtmsg_ = reinterpret_cast<struct rtmsg*>((char*)msghdr_ + nlmsgAlen);

  rtmsg_->rtm_table = RT_TABLE_MAIN;
  rtmsg_->rtm_protocol = route.getProtocolId();
  rtmsg_->rtm_scope = RT_SCOPE_UNIVERSE;
  rtmsg_->rtm_type = route.getType();
  rtmsg_->rtm_src_len = 0;
  rtmsg_->rtm_tos = 0;
  rtmsg_->rtm_flags = rtFlags;

  auto rtFlag = route.getFlags();
  if (rtFlag.hasValue()) {
    rtmsg_->rtm_flags |= rtFlag.value();
  }
}

void
NetlinkRouteMessage::showRtmMsg(const struct rtmsg* const hdr) const {
  LOG(INFO) << "Route message data"
            << "\nrtm_family:   " << +hdr->rtm_family
            << "\nrtm_dst_len:  " << +hdr->rtm_dst_len
            << "\nrtm_src_len:  " << +hdr->rtm_src_len
            << "\nrtm_tos:      " << +hdr->rtm_tos
            << "\nrtm_table:    " << +hdr->rtm_table
            << "\nrtm_protocol: " << +hdr->rtm_protocol
            << "\nrtm_scope:    " << +hdr->rtm_scope
            << "\nrtm_type:     " << +hdr->rtm_type
            << "\nrtm_flags:    " << std::hex << hdr->rtm_flags;
}

void
NetlinkRouteMessage::showRouteAttribute(const struct rtattr* const hdr) const {
  LOG(INFO) << "Route attributes"
            << "\nrta_len       " << hdr->rta_len << "\nrta_type      "
            << hdr->rta_type;
}

uint32_t
NetlinkRouteMessage::encodeLabel(uint32_t label, bool bos) const {
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

ResultCode
NetlinkRouteMessage::addIpNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const openr::rnl::NextHop& path,
    const openr::rnl::Route& route) const {
  rtnh->rtnh_len = sizeof(*rtnh);
  if (path.getIfIndex().has_value()) {
    rtnh->rtnh_ifindex = path.getIfIndex().value();
  }
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  // RTA_GATEWAY
  auto const via = path.getGateway();
  if (!via.hasValue()) {
    if (route.getType() == RTN_MULTICAST || route.getScope() == RT_SCOPE_LINK) {
      return ResultCode::SUCCESS;
    }
    LOG(ERROR) << "Nexthop IP not provided";
    return ResultCode::NO_NEXTHOP_IP;
  }
  if (addSubAttributes(
          rta, RTA_GATEWAY, via.value().bytes(), via.value().byteCount()) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  };

  // update length in rtnexthop
  rtnh->rtnh_len += via.value().byteCount() + sizeof(struct rtattr);
  return ResultCode::SUCCESS;
}

ResultCode
NetlinkRouteMessage::addSwapOrPHPNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const openr::rnl::NextHop& path) const {
  rtnh->rtnh_len = sizeof(*rtnh);
  rtnh->rtnh_ifindex = path.getIfIndex().value();
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  // add the following subattributes within RTA_MULTIPATH
  size_t prevLen = rta->rta_len;

  // labels.size() = 0 implies PHP
  auto maybeLabel = path.getSwapLabel();
  if (maybeLabel.hasValue()) {
    struct mpls_label swapLabel;
    swapLabel.entry = encodeLabel(maybeLabel.value(), true);

    if (addSubAttributes(
            rta,
            RTA_NEWDST,
            reinterpret_cast<const char*>(&swapLabel),
            sizeof(swapLabel)) == nullptr) {
      return ResultCode::NO_MESSAGE_BUFFER;
    }
  }
  // update rtnh len
  rtnh->rtnh_len += rta->rta_len - prevLen;

  // RTA_VIA
  struct NextHop via;
  via.addrFamily = path.getFamily();
  auto gw = path.getGateway().value();
  int viaLen{sizeof(struct NextHop)};
  if (via.addrFamily == AF_INET) {
    viaLen = sizeof(struct NextHopV4);
  }
  memcpy(via.ip, reinterpret_cast<const char*>(gw.bytes()), gw.byteCount());
  if (addSubAttributes(
          rta, RTA_VIA, reinterpret_cast<const char*>(&via), viaLen) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  }

  // update length in rtnexthop
  rtnh->rtnh_len += viaLen + sizeof(struct rtattr);
  return ResultCode::SUCCESS;
}

ResultCode
NetlinkRouteMessage::addPopNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const openr::rnl::NextHop& path) const {
  // for each next hop add the ENCAP
  rtnh->rtnh_len = sizeof(*rtnh);
  if (!path.getIfIndex().hasValue()) {
    LOG(ERROR) << "Loopback interface index not provided for POP";
    return ResultCode::NO_LOOPBACK_INDEX;
  }
  rtnh->rtnh_ifindex = path.getIfIndex().value();
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  int oif = rtnh->rtnh_ifindex;
  if (addSubAttributes(
          rta, RTA_OIF, reinterpret_cast<const char*>(&oif), sizeof(oif)) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  }

  // update length in rtnexthop
  rtnh->rtnh_len += sizeof(oif) + sizeof(struct rtattr);
  return ResultCode::SUCCESS;
}

ResultCode
NetlinkRouteMessage::addLabelNexthop(
    struct rtattr* rta,
    struct rtnexthop* rtnh,
    const openr::rnl::NextHop& path) const {
  // fill the OIF
  rtnh->rtnh_len = sizeof(*rtnh);
  rtnh->rtnh_ifindex = path.getIfIndex().value(); /* interface index */
  rtnh->rtnh_flags = 0;
  rtnh->rtnh_hops = 0;

  // add the following subattributes within RTA_MULTIPATH
  size_t prevLen = rta->rta_len;

  // RTA_ENCAP sub attribute
  struct rtattr* rtaEncap = addSubAttributes(rta, RTA_ENCAP, nullptr, 0);

  if (rtaEncap == nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  }

  // MPLS_IP_TUNNEL_DST sub attribute
  std::array<struct mpls_label, kMaxLabels> mplsLabel;
  size_t i = 0;
  auto labels = path.getPushLabels();
  if (!labels.hasValue()) {
    LOG(ERROR) << "Labels not provided for PUSH action";
    return ResultCode::NO_LABEL;
  }
  // abort immediately to bring attention
  CHECK(labels.value().size() <= kMaxLabels);
  for (auto label : labels.value()) {
    VLOG(2) << "Pushing label: " << label;
    bool bos = i == labels.value().size() - 1 ? true : false;
    mplsLabel[i++].entry = encodeLabel(label, bos);
  }
  size_t totalSize = labels.value().size() * sizeof(struct mpls_label);
  if (addSubAttributes(rta, MPLS_IPTUNNEL_DST, &mplsLabel, totalSize) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  };

  // update RTA ENCAP sub attribute length
  rtaEncap->rta_len = RTA_ALIGN(rta->rta_len) - prevLen;

  // RTA_ENCAP_TYPE sub attribute
  uint16_t encapType = LWTUNNEL_ENCAP_MPLS;
  if (addSubAttributes(rta, RTA_ENCAP_TYPE, &encapType, sizeof(encapType)) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  };

  // update rtnh len
  rtnh->rtnh_len += rta->rta_len - prevLen;

  // RTA_GATEWAY
  auto const via = path.getGateway();
  if (!via.hasValue()) {
    LOG(ERROR) << "Nexthop IP not provided";
    return ResultCode::NO_NEXTHOP_IP;
  }
  if (addSubAttributes(
          rta, RTA_GATEWAY, via.value().bytes(), via.value().byteCount()) ==
      nullptr) {
    return ResultCode::NO_MESSAGE_BUFFER;
  };

  // update length in rtnexthop
  rtnh->rtnh_len += via.value().byteCount() + sizeof(struct rtattr);
  return ResultCode::SUCCESS;
}

ResultCode
NetlinkRouteMessage::addNextHops(const openr::rnl::Route& route) {
  ResultCode status{ResultCode::SUCCESS};
  std::array<char, kMaxNlPayloadSize> nhop = {};
  if (route.getNextHops().size()) {
    if ((status = addMultiPathNexthop(nhop, route)) != ResultCode::SUCCESS) {
      return status;
    };

    // copy the encap info into NLMSG payload
    const char* const data = reinterpret_cast<const char*>(
        RTA_DATA(reinterpret_cast<struct rtattr*>(nhop.data())));
    int payloadLen = RTA_PAYLOAD(reinterpret_cast<struct rtattr*>(nhop.data()));
    if ((status = addAttributes(RTA_MULTIPATH, data, payloadLen, msghdr_)) !=
        ResultCode::SUCCESS) {
      return status;
    };
  }
  return status;
}

ResultCode
NetlinkRouteMessage::addMultiPathNexthop(
    std::array<char, kMaxNlPayloadSize>& nhop,
    const openr::rnl::Route& route) const {
  // Add [RTA_MULTIPATH - label, via, dev][RTA_ENCAP][RTA_ENCAP_TYPE]
  struct rtattr* rta = reinterpret_cast<struct rtattr*>(nhop.data());

  // MULTIPATH
  rta->rta_type = RTA_MULTIPATH;
  rta->rta_len = RTA_LENGTH(0);
  struct rtnexthop* rtnh = reinterpret_cast<struct rtnexthop*>(RTA_DATA(rta));
  ResultCode result{ResultCode::SUCCESS};

  const auto& paths = route.getNextHops();
  for (const auto& path : paths) {
    VLOG(3) << path.str();
    rtnh->rtnh_len = sizeof(*rtnh);
    rta->rta_len += rtnh->rtnh_len;
    auto action = path.getLabelAction();

    if (action.hasValue()) {
      switch (action.value()) {
      case openr::fbmeshd::thrift::MplsActionCode::PUSH:
        result = addLabelNexthop(rta, rtnh, path);
        break;

      case openr::fbmeshd::thrift::MplsActionCode::SWAP:
      case openr::fbmeshd::thrift::MplsActionCode::PHP:
        result = addSwapOrPHPNexthop(rta, rtnh, path);
        break;

      case openr::fbmeshd::thrift::MplsActionCode::POP_AND_LOOKUP:
        result = addPopNexthop(rta, rtnh, path);
        break;

      default:
        LOG(ERROR) << "Unknown action ";
        return ResultCode::UNKNOWN_LABEL_ACTION;
      }
    } else {
      result = addIpNexthop(rta, rtnh, path, route);
    }

    if (result != ResultCode::SUCCESS) {
      return result;
    }
    rtnh = RTNH_NEXT(rtnh);
  }
  return result;
}

void
NetlinkRouteMessage::showMultiPathAttribues(
    const struct rtattr* const rta) const {
  const struct rtnexthop* const rtnh =
      reinterpret_cast<struct rtnexthop*>(RTA_DATA(rta));
  LOG(INFO) << "len: " << rtnh->rtnh_len << " flags: " << rtnh->rtnh_flags;
  LOG(INFO) << "hop: " << rtnh->rtnh_hops << " ifindex: " << rtnh->rtnh_ifindex;

  const struct rtattr* subrta = RTNH_DATA(rtnh);
  int len = rtnh->rtnh_len;

  do {
    if (!RTA_OK(subrta, len)) {
      break;
    }
    showRouteAttribute(subrta);
  } while ((subrta = RTA_NEXT(subrta, len)));
}

folly::Expected<folly::IPAddress, folly::IPAddressFormatError>
NetlinkRouteMessage::parseIp(
    const struct rtattr* ipAttr, unsigned char family) const {
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

folly::Optional<std::vector<int32_t>>
NetlinkRouteMessage::parseMplsLabels(const struct rtattr* routeAttr) const {
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
  return folly::none;
}

void
NetlinkRouteMessage::parseNextHopAttribute(
    const struct rtattr* routeAttr,
    unsigned char family,
    rnl::NextHopBuilder& nhBuilder) const {
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
      struct NextHop* via =
          reinterpret_cast<struct NextHop*> RTA_DATA(routeAttr);
      ipAddress = folly::IPAddressV6::tryFromBinary(
          folly::ByteRange(reinterpret_cast<const uint8_t*>(via->ip), 16));
    } else {
      // IPv4 nexthop address
      struct NextHopV4* via =
          reinterpret_cast<struct NextHopV4*> RTA_DATA(routeAttr);
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
    if (pushLabels.hasValue()) {
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
    rnl::NextHopBuilder& nhBuilder, unsigned char family) const {
  // Inferring MPLS action from nexthop fields
  if (nhBuilder.getPushLabels() != folly::none) {
    nhBuilder.setLabelAction(openr::fbmeshd::thrift::MplsActionCode::PUSH);
  } else if (family == AF_MPLS) {
    if (nhBuilder.getGateway() != folly::none &&
        nhBuilder.getSwapLabel() != folly::none) {
      nhBuilder.setLabelAction(openr::fbmeshd::thrift::MplsActionCode::SWAP);
    } else if (
        nhBuilder.getGateway() != folly::none &&
        nhBuilder.getSwapLabel() == folly::none) {
      nhBuilder.setLabelAction(openr::fbmeshd::thrift::MplsActionCode::PHP);
    } else {
      nhBuilder.setLabelAction(
          openr::fbmeshd::thrift::MplsActionCode::POP_AND_LOOKUP);
    }
  }
}

rnl::Route
NetlinkRouteMessage::parseMessage(const struct nlmsghdr* nlmsg) const {
  rnl::RouteBuilder routeBuilder;
  // For single next hop in the route
  rnl::NextHopBuilder nhBuilder;
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
        routeBuilder.addNextHop(nh);
      }
    } break;
    }
  }

  if (singleNextHopFlag) {
    setMplsAction(nhBuilder, routeEntry->rtm_family);
    // add single nexthop
    routeBuilder.addNextHop(nhBuilder.build());
  }

  auto route = routeBuilder.build();
  VLOG(2) << route.str();
  return route;
}

std::vector<rnl::NextHop>
NetlinkRouteMessage::parseNextHops(
    const struct rtattr* routeAttrMP, unsigned char family) const {
  std::vector<rnl::NextHop> nextHops;
  struct rtnexthop* nh =
      reinterpret_cast<struct rtnexthop*> RTA_DATA(routeAttrMP);

  int nhLen = RTA_PAYLOAD(routeAttrMP);
  do {
    rnl::NextHopBuilder nhBuilder;
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
    setMplsAction(nhBuilder, family);
    auto nexthop = nhBuilder.build();
    VLOG(2) << nexthop.str();
    nextHops.emplace_back(nexthop);
    nhLen -= NLMSG_ALIGN(nh->rtnh_len);
    nh = RTNH_NEXT(nh);
  } while (RTNH_OK(nh, nhLen));
  return nextHops;
}

ResultCode
NetlinkRouteMessage::addRoute(const openr::rnl::Route& route) {
  auto const& pfix = route.getDestination();
  auto ip = std::get<0>(pfix);
  auto plen = std::get<1>(pfix);
  auto addressFamily = route.getFamily();

  VLOG(1) << "Adding route: " << route.str();

  if (addressFamily != AF_INET && addressFamily != AF_INET6) {
    LOG(ERROR) << "Address family is not AF_INET or AF_INET6";
    return ResultCode::INVALID_ADDRESS_FAMILY;
  }

  init(RTM_NEWROUTE, 0, route);

  rtmsg_->rtm_family = addressFamily;
  rtmsg_->rtm_dst_len = plen; /* netmask */
  const char* const ipptr = reinterpret_cast<const char*>(ip.bytes());
  ResultCode status{ResultCode::SUCCESS};
  if ((status = addAttributes(RTA_DST, ipptr, ip.byteCount(), msghdr_)) !=
      ResultCode::SUCCESS) {
    return status;
  };

  // set up admin distance if priority.
  if (route.getPriority()) {
    const uint32_t adminDistance = route.getPriority().value();
    const char* const adPtr = reinterpret_cast<const char*>(&adminDistance);
    if ((status =
             addAttributes(RTA_PRIORITY, adPtr, sizeof(uint32_t), msghdr_)) !=
        ResultCode::SUCCESS) {
      return status;
    };
  }

  return addNextHops(route);
}

ResultCode
NetlinkRouteMessage::deleteRoute(const openr::rnl::Route& route) {
  auto const& pfix = route.getDestination();
  auto addressFamily = route.getFamily();
  VLOG(1) << "Deleting route: " << route.str();

  if (addressFamily != AF_INET && addressFamily != AF_INET6) {
    return ResultCode::INVALID_ADDRESS_FAMILY;
  }
  init(RTM_DELROUTE, 0, route);

  auto plen = std::get<1>(pfix);
  auto ip = std::get<0>(pfix);
  rtmsg_->rtm_family = addressFamily;
  rtmsg_->rtm_dst_len = plen; /* netmask */
  const char* const ipptr = reinterpret_cast<const char*>(ip.bytes());
  ResultCode status{ResultCode::SUCCESS};
  if ((status = addAttributes(RTA_DST, ipptr, ip.byteCount(), msghdr_)) !=
      ResultCode::SUCCESS) {
    return status;
  };
  return status;
}

ResultCode
NetlinkRouteMessage::addLabelRoute(const openr::rnl::Route& route) {
  init(RTM_NEWROUTE, 0, route);
  rtmsg_->rtm_family = AF_MPLS;
  rtmsg_->rtm_dst_len = kLabelSizeBits;
  rtmsg_->rtm_flags = 0;
  struct mpls_label mlabel;

  VLOG(1) << "Adding MPLS route " << route.str();
  if (route.getFamily() != AF_MPLS) {
    return ResultCode::INVALID_ADDRESS_FAMILY;
  }

  auto label = route.getMplsLabel();
  if (!label.hasValue()) {
    return ResultCode::NO_LABEL;
  }

  mlabel.entry = encodeLabel(label.value(), true);
  ResultCode status{ResultCode::SUCCESS};
  if ((status = addAttributes(
           RTA_DST,
           reinterpret_cast<const char*>(&mlabel),
           sizeof(mpls_label),
           msghdr_)) != ResultCode::SUCCESS) {
    return status;
  };

  return addNextHops(route);
}

ResultCode
NetlinkRouteMessage::deleteLabelRoute(const openr::rnl::Route& route) {
  init(RTM_DELROUTE, 0, route);
  rtmsg_->rtm_family = AF_MPLS;
  rtmsg_->rtm_dst_len = kLabelSizeBits;
  rtmsg_->rtm_flags = 0;
  struct mpls_label mlabel;
  auto label = route.getMplsLabel();
  if (!label.hasValue()) {
    LOG(ERROR) << "Label not provided";
    return ResultCode::NO_LABEL;
  }
  VLOG(1) << "Deleting label: " << route.str();
  mlabel.entry = encodeLabel(label.value(), true);
  ResultCode status{ResultCode::SUCCESS};
  if ((status = addAttributes(
           RTA_DST,
           reinterpret_cast<const char*>(&mlabel),
           sizeof(mpls_label),
           msghdr_)) != ResultCode::SUCCESS) {
    return status;
  };

  return status;
}

NetlinkLinkMessage::NetlinkLinkMessage() {
  // get pointer to NLMSG header
  msghdr_ = getMessagePtr();
}

void
NetlinkLinkMessage::init(int type, uint32_t linkFlags) {
  if (type != RTM_NEWLINK && type != RTM_DELLINK && type != RTM_GETLINK) {
    LOG(ERROR) << "Incorrect Netlink message type";
    return;
  }
  // initialize netlink header
  msghdr_->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
  msghdr_->nlmsg_type = type;
  msghdr_->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

  if (type == RTM_GETLINK) {
    // Get all links
    msghdr_->nlmsg_flags |= NLM_F_DUMP;
  }

  // intialize the route link message header
  auto nlmsgAlen = NLMSG_ALIGN(sizeof(struct nlmsghdr));
  ifinfomsg_ = reinterpret_cast<struct ifinfomsg*>((char*)msghdr_ + nlmsgAlen);

  ifinfomsg_->ifi_flags = linkFlags;
  ifinfomsg_->ifi_change = 0xffffffff;
}

rnl::Link
NetlinkLinkMessage::parseMessage(const struct nlmsghdr* nlmsg) const {
  rnl::LinkBuilder builder;
  const struct ifinfomsg* const linkEntry =
      reinterpret_cast<struct ifinfomsg*>(NLMSG_DATA(nlmsg));

  builder =
      builder.setIfIndex(linkEntry->ifi_index).setFlags(linkEntry->ifi_flags);

  const struct rtattr* linkAttr;
  auto linkAttrLen = IFLA_PAYLOAD(nlmsg);
  // process all link attributes
  for (linkAttr = IFLA_RTA(linkEntry); RTA_OK(linkAttr, linkAttrLen);
       linkAttr = RTA_NEXT(linkAttr, linkAttrLen)) {
    switch (linkAttr->rta_type) {
    case IFLA_IFNAME: {
      const char* name = reinterpret_cast<const char*>(RTA_DATA(linkAttr));
      builder = builder.setLinkName(std::string(name));
    }
    }
  }
  auto link = builder.build();
  VLOG(2) << link.str();
  return link;
}

NetlinkAddrMessage::NetlinkAddrMessage() {
  // get pointer to NLMSG header
  msghdr_ = getMessagePtr();
}

void
NetlinkAddrMessage::init(int type) {
  if (type != RTM_NEWADDR && type != RTM_DELADDR && type != RTM_GETADDR) {
    LOG(ERROR) << "Incorrect Netlink message type";
    return;
  }
  // initialize netlink header
  msghdr_->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
  msghdr_->nlmsg_type = type;
  msghdr_->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

  if (type == RTM_GETADDR) {
    // Get all addresses
    msghdr_->nlmsg_flags |= NLM_F_DUMP;
  }

  if (type == RTM_NEWADDR) {
    msghdr_->nlmsg_flags |= NLM_F_CREATE;
  }

  // intialize the route address message header
  auto nlmsgAlen = NLMSG_ALIGN(sizeof(struct nlmsghdr));
  ifaddrmsg_ = reinterpret_cast<struct ifaddrmsg*>((char*)msghdr_ + nlmsgAlen);
}

ResultCode
NetlinkAddrMessage::addOrDeleteIfAddress(
    const openr::rnl::IfAddress& ifAddr, const int type) {
  if (type != RTM_NEWADDR && type != RTM_DELADDR) {
    LOG(ERROR) << "Incorrect Netlink message type";
    return ResultCode::FAIL;
  } else if (ifAddr.getFamily() != AF_INET && ifAddr.getFamily() != AF_INET6) {
    LOG(ERROR) << "Invalid address family" << ifAddr.str();
    return ResultCode::INVALID_ADDRESS_FAMILY;
  } else if (!ifAddr.getPrefix().hasValue()) {
    // No IP address given
    LOG(ERROR) << "No IP address given to add " << ifAddr.str();
    return ResultCode::NO_IP;
  }

  LOG(INFO) << (type == RTM_NEWADDR ? "Adding" : "Deleting") << " IP address "
            << ifAddr.str();
  init(type);
  // initialize netlink address fields
  auto ip = std::get<0>(ifAddr.getPrefix().value());
  uint8_t prefixLen = std::get<1>(ifAddr.getPrefix().value());
  ifaddrmsg_->ifa_family = ifAddr.getFamily();
  ifaddrmsg_->ifa_prefixlen = prefixLen;
  ifaddrmsg_->ifa_flags =
      (ifAddr.getFlags().hasValue() ? ifAddr.getFlags().value() : 0);
  if (ifAddr.getScope().hasValue()) {
    ifaddrmsg_->ifa_scope = ifAddr.getScope().value();
  }
  ifaddrmsg_->ifa_index = ifAddr.getIfIndex();

  const char* const ipptr = reinterpret_cast<const char*>(ip.bytes());
  ResultCode status{ResultCode::SUCCESS};
  status = addAttributes(IFA_ADDRESS, ipptr, ip.byteCount(), msghdr_);
  if (status != ResultCode::SUCCESS) {
    return status;
  }
  // For IPv4, need to specify the ip address in IFA_LOCAL attribute as well
  // for point-to-point interfaces
  // For IPv6, the extra attribute has no effect
  status = addAttributes(IFA_LOCAL, ipptr, ip.byteCount(), msghdr_);
  return status;
}

rnl::IfAddress
NetlinkAddrMessage::parseMessage(const struct nlmsghdr* nlmsg) const {
  rnl::IfAddressBuilder builder;
  const struct ifaddrmsg* const addrEntry =
      reinterpret_cast<struct ifaddrmsg*>(NLMSG_DATA(nlmsg));

  const bool isValid = nlmsg->nlmsg_type == RTM_NEWADDR ? true : false;
  builder = builder.setIfIndex(addrEntry->ifa_index)
                .setScope(addrEntry->ifa_scope)
                .setValid(isValid);

  const struct rtattr* addrAttr;
  auto addrAttrLen = IFA_PAYLOAD(nlmsg);
  // process all address attributes
  for (addrAttr = IFA_RTA(addrEntry); RTA_OK(addrAttr, addrAttrLen);
       addrAttr = RTA_NEXT(addrAttr, addrAttrLen)) {
    switch (addrAttr->rta_type) {
    case IFA_ADDRESS: {
      if (addrEntry->ifa_family == AF_INET) {
        struct in_addr* addr4 = reinterpret_cast<in_addr*> RTA_DATA(addrAttr);
        auto ipAddress = folly::IPAddressV4::fromLong(addr4->s_addr);
        folly::CIDRNetwork prefix =
            std::make_pair(ipAddress, (uint8_t)addrEntry->ifa_prefixlen);
        builder = builder.setPrefix(prefix);
      } else if (addrEntry->ifa_family == AF_INET6) {
        struct in6_addr* addr6 = reinterpret_cast<in6_addr*> RTA_DATA(addrAttr);
        auto ipAddress = folly::IPAddressV6::tryFromBinary(folly::ByteRange(
            reinterpret_cast<const uint8_t*>(addr6->s6_addr), 16));
        if (ipAddress.hasValue()) {
          folly::CIDRNetwork prefix = std::make_pair(
              ipAddress.value(), (uint8_t)addrEntry->ifa_prefixlen);
          builder = builder.setPrefix(prefix);
        } else {
          LOG(ERROR) << "Error parsing Netlink ADDR message";
        }
      }
    }
    }
  }
  auto addr = builder.build();
  VLOG(2) << addr.str();
  return addr;
}

NetlinkNeighborMessage::NetlinkNeighborMessage() {
  // get pointer to NLMSG header
  msghdr_ = getMessagePtr();
}

void
NetlinkNeighborMessage::init(int type, uint32_t neighFlags) {
  if (type != RTM_NEWNEIGH && type != RTM_DELNEIGH && type != RTM_GETNEIGH) {
    LOG(ERROR) << "Incorrect Netlink message type";
    return;
  }
  // initialize netlink header
  msghdr_->nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
  msghdr_->nlmsg_type = type;
  msghdr_->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

  if (type == RTM_GETNEIGH) {
    // Get all neighbors
    msghdr_->nlmsg_flags |= NLM_F_DUMP;
  }

  // intialize the route neighbor message header
  auto nlmsgAlen = NLMSG_ALIGN(sizeof(struct nlmsghdr));
  ndmsg_ = reinterpret_cast<struct ndmsg*>((char*)msghdr_ + nlmsgAlen);
  ndmsg_->ndm_flags = neighFlags;
}

rnl::Neighbor
NetlinkNeighborMessage::parseMessage(const struct nlmsghdr* nlmsg) const {
  rnl::NeighborBuilder builder;
  const struct ndmsg* const neighEntry =
      reinterpret_cast<struct ndmsg*>(NLMSG_DATA(nlmsg));

  // Construct neighbor from Netlink message
  bool isDeleted = (nlmsg->nlmsg_type == RTM_DELNEIGH);
  builder = builder.setIfIndex(neighEntry->ndm_ifindex)
                .setState(neighEntry->ndm_state, isDeleted);
  const struct rtattr* neighAttr;
  auto neighAttrLen = RTM_PAYLOAD(nlmsg);
  // process all neighbor attributes
  for (neighAttr = RTM_RTA(neighEntry); RTA_OK(neighAttr, neighAttrLen);
       neighAttr = RTA_NEXT(neighAttr, neighAttrLen)) {
    switch (neighAttr->rta_type) {
    case NDA_DST: {
      if (neighEntry->ndm_family == AF_INET) {
        // IPv4 address
        struct in_addr* addr4 = reinterpret_cast<in_addr*> RTA_DATA(neighAttr);
        auto ipAddress = folly::IPAddressV4::fromLong(addr4->s_addr);
        builder = builder.setDestination(ipAddress);
      } else if (neighEntry->ndm_family == AF_INET6) {
        // IPv6 Address
        struct in6_addr* addr6 =
            reinterpret_cast<in6_addr*> RTA_DATA(neighAttr);
        auto ipAddress = folly::IPAddressV6::tryFromBinary(folly::ByteRange(
            reinterpret_cast<const uint8_t*>(addr6->s6_addr), 16));
        if (ipAddress.hasValue()) {
          builder = builder.setDestination(ipAddress.value());
        } else {
          LOG(ERROR) << "Error parsing Netlink NEIGH message";
        }
      }
    } break;

    case NDA_LLADDR: {
      auto macAddress = folly::MacAddress::fromBinary(folly::ByteRange(
          reinterpret_cast<const uint8_t*>(RTA_DATA(neighAttr)), ETH_ALEN));
      builder.setLinkAddress(macAddress);
    } break;
    }
  }
  rnl::Neighbor neighbor = builder.build();
  VLOG(2) << neighbor.str();
  return neighbor;
}

} // namespace rnl
} // namespace openr
