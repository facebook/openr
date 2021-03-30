/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkLinkMessage.h>

namespace openr::fbnl {

NetlinkLinkMessage::NetlinkLinkMessage() {
  // get pointer to NLMSG header
  msghdr_ = getMessagePtr();
}

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
    }
    }
  }
  auto link = builder.build();
  VLOG(3) << "Netlink parsed link message. " << link.str();
  return link;
}

} // namespace openr::fbnl
