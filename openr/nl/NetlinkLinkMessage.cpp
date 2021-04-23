/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <linux/if_tunnel.h>
#include <openr/nl/NetlinkLinkMessage.h>

namespace {
const std::string kGreKind{"gre"};
const std::string kIp6GreKind{"ip6gre"};
} // namespace

namespace openr::fbnl {

NetlinkLinkMessage::NetlinkLinkMessage() : NetlinkMessageBase() {}

NetlinkLinkMessage::~NetlinkLinkMessage() {
  CHECK(linkPromise_.isFulfilled());
}

void
NetlinkLinkMessage::rcvdLink(Link&& link) {
  rcvdLinks_.emplace_back(std::move(link));
}

void
NetlinkLinkMessage::setReturnStatus(int status) {
  if (status == 0) {
    linkPromise_.setValue(std::move(rcvdLinks_));
  } else {
    linkPromise_.setValue(folly::makeUnexpected(status));
  }
  NetlinkMessageBase::setReturnStatus(status);
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

  if (type == RTM_NEWLINK) {
    // We create new link or replace existing
    msghdr_->nlmsg_flags |= NLM_F_CREATE;
    msghdr_->nlmsg_flags |= NLM_F_REPLACE;
  }

  // intialize the route link message header
  auto nlmsgAlen = NLMSG_ALIGN(sizeof(struct nlmsghdr));
  ifinfomsg_ = reinterpret_cast<struct ifinfomsg*>((char*)msghdr_ + nlmsgAlen);

  ifinfomsg_->ifi_flags = linkFlags;
  ifinfomsg_->ifi_change = 0xffffffff;
}

Link
NetlinkLinkMessage::parseMessage(const struct nlmsghdr* nlmsg) {
  LinkBuilder builder;
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
    } break;
    case IFLA_LINKINFO: {
      auto infoPair = parseLinkInfo(linkAttr);
      if (infoPair.first.has_value()) {
        builder = builder.setLinkKind(infoPair.first.value());
      }
      if (infoPair.second.has_value()) {
        builder = builder.setGreInfo(infoPair.second.value());
      }
    } break;
    }
  }
  auto link = builder.build();
  VLOG(3) << "Netlink parsed link message. " << link.str();
  return link;
}

std::pair<std::optional<std::string>, std::optional<GreInfo>>
NetlinkLinkMessage::parseLinkInfo(const struct rtattr* attr) {
  const struct rtattr* linkInfoAttr =
      reinterpret_cast<struct rtattr*> RTA_DATA(attr);
  int attrLen = RTA_PAYLOAD(attr);
  std::optional<std::string> linkKind;
  std::optional<GreInfo> greInfo;
  // track pointer to attr IFLA_INFO_DATA, linkKind is needed to parse ip
  const struct rtattr* infoDataAttr = nullptr;
  do {
    switch (linkInfoAttr->rta_type) {
    case IFLA_INFO_KIND: {
      const char* kind = reinterpret_cast<const char*>(RTA_DATA(linkInfoAttr));
      linkKind = std::string(kind);
    } break;
    case IFLA_INFO_DATA: {
      infoDataAttr = linkInfoAttr;
    } break;
    }
    linkInfoAttr = RTA_NEXT(linkInfoAttr, attrLen);
  } while (RTA_OK(linkInfoAttr, attrLen));

  if (infoDataAttr && linkKind) {
    if (linkKind == kGreKind) {
      greInfo = parseInfoData(infoDataAttr, AF_INET);
    } else if (linkKind == kIp6GreKind) {
      greInfo = parseInfoData(infoDataAttr, AF_INET6);
    }
  }
  return std::make_pair(linkKind, greInfo);
}

std::optional<GreInfo>
NetlinkLinkMessage::parseInfoData(
    const struct rtattr* attr, unsigned char family) {
  const struct rtattr* infoDataAttr =
      reinterpret_cast<struct rtattr*> RTA_DATA(attr);
  int attrLen = RTA_PAYLOAD(attr);

  std::optional<GreInfo> greInfo;
  std::optional<folly::IPAddress> localAddr;
  std::optional<folly::IPAddress> remoteAddr;
  std::optional<uint8_t> ttl;

  do {
    switch (infoDataAttr->rta_type) {
    case IFLA_GRE_LOCAL: {
      const auto ipExpected = parseIp(infoDataAttr, family);
      if (ipExpected.hasValue()) {
        localAddr = ipExpected.value();
      }
    } break;
    case IFLA_GRE_REMOTE: {
      const auto ipExpected = parseIp(infoDataAttr, family);
      if (ipExpected.hasValue()) {
        remoteAddr = ipExpected.value();
      }
    } break;
    case IFLA_GRE_TTL: {
      ttl = *(reinterpret_cast<int*> RTA_DATA(infoDataAttr));
    } break;
    }
    infoDataAttr = RTA_NEXT(infoDataAttr, attrLen);
  } while (RTA_OK(infoDataAttr, attrLen));

  if (localAddr and remoteAddr and ttl) {
    greInfo = GreInfo(localAddr.value(), remoteAddr.value(), ttl.value());
  }

  return greInfo;
}

int
NetlinkLinkMessage::addLink(const Link& link) {
  init(RTM_NEWLINK, link.getFlags());

  int status{0};
  if ((status = addAttributes(
           IFLA_IFNAME,
           link.getLinkName().c_str(),
           link.getLinkName().length()))) {
    return status;
  }
  if ((status = addLinkInfo(link))) {
    return status;
  }

  return 0;
}

int
NetlinkLinkMessage::addLinkInfo(const Link& link) {
  std::array<char, kMaxNlPayloadSize> linkInfo = {};
  int status{0};
  if (!link.getLinkKind().has_value()) {
    return 0;
  }
  struct rtattr* rta = reinterpret_cast<struct rtattr*>(linkInfo.data());
  // init linkinfo
  rta->rta_type = IFLA_LINKINFO;
  rta->rta_len = RTA_LENGTH(0);
  if ((status = addLinkInfoSubAttrs(linkInfo, link))) {
    return status;
  }

  const char* const data = reinterpret_cast<const char*>(
      RTA_DATA(reinterpret_cast<struct rtattr*>(linkInfo.data())));
  int payloadLen =
      RTA_PAYLOAD(reinterpret_cast<struct rtattr*>(linkInfo.data()));
  if ((status = addAttributes(IFLA_LINKINFO, data, payloadLen))) {
    return status;
  }

  return 0;
}

int
NetlinkLinkMessage::addLinkInfoSubAttrs(
    std::array<char, kMaxNlPayloadSize>& linkInfo, const Link& link) const {
  struct rtattr* rta = reinterpret_cast<struct rtattr*>(linkInfo.data());

  if (addSubAttributes(
          rta,
          IFLA_INFO_KIND,
          link.getLinkKind()->c_str(),
          link.getLinkKind()->length()) == nullptr) {
    return ENOBUFS;
  }

  const auto greInfo = link.getGreInfo();
  if (greInfo.has_value()) {
    // init sub attribute
    std::array<char, kMaxNlPayloadSize> linkInfoData = {};
    struct rtattr* subRta =
        reinterpret_cast<struct rtattr*>(linkInfoData.data());
    subRta->rta_type = IFLA_INFO_DATA;
    subRta->rta_len = RTA_LENGTH(0);

    const auto localAddr = greInfo->getLocalAddr();
    const auto remoteAddr = greInfo->getRemoteAddr();
    const auto ttl = greInfo->getTtl();
    if (addSubAttributes(
            subRta, IFLA_GRE_LOCAL, localAddr.bytes(), localAddr.byteCount()) ==
        nullptr) {
      return ENOBUFS;
    }
    if (addSubAttributes(
            subRta,
            IFLA_GRE_REMOTE,
            remoteAddr.bytes(),
            remoteAddr.byteCount()) == nullptr) {
      return ENOBUFS;
    }
    if (addSubAttributes(
            subRta,
            IFLA_GRE_TTL,
            reinterpret_cast<const char*>(&ttl),
            sizeof(uint8_t)) == nullptr) {
      return ENOBUFS;
    }

    // add to parent rta
    const char* const data = reinterpret_cast<const char*>(
        RTA_DATA(reinterpret_cast<struct rtattr*>(linkInfoData.data())));
    int payloadLen =
        RTA_PAYLOAD(reinterpret_cast<struct rtattr*>(linkInfoData.data()));
    if (addSubAttributes(rta, IFLA_INFO_DATA, data, payloadLen) == nullptr) {
      return ENOBUFS;
    }
  }

  return 0;
}

int
NetlinkLinkMessage::deleteLink(const Link& link) {
  init(RTM_DELLINK, 0);

  int status{0};
  if ((status = addAttributes(
           IFLA_IFNAME,
           link.getLinkName().c_str(),
           link.getLinkName().length()))) {
    return status;
  }

  return 0;
}

} // namespace openr::fbnl
