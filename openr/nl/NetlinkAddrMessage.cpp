/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

#include <openr/nl/NetlinkAddrMessage.h>

namespace openr::fbnl {

NetlinkAddrMessage::NetlinkAddrMessage() : NetlinkMessageBase() {}

NetlinkAddrMessage::~NetlinkAddrMessage() {
  CHECK(addrPromise_.isFulfilled());
}

void
NetlinkAddrMessage::rcvdIfAddress(IfAddress&& ifAddr) {
  rcvdAddrs_.emplace_back(std::move(ifAddr));
}

void
NetlinkAddrMessage::setReturnStatus(int status) {
  if (status == 0) {
    addrPromise_.setValue(std::move(rcvdAddrs_));
  } else {
    addrPromise_.setValue(folly::makeUnexpected(status));
  }

  NetlinkMessageBase::setReturnStatus(status);
}

void
NetlinkAddrMessage::init(int type) {
  if (type != RTM_NEWADDR && type != RTM_DELADDR && type != RTM_GETADDR) {
    XLOG(ERR) << "Incorrect Netlink message type";
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

int
NetlinkAddrMessage::addOrDeleteIfAddress(
    const IfAddress& ifAddr, const int type) {
  if (type != RTM_NEWADDR and type != RTM_DELADDR) {
    XLOG(ERR) << "Incorrect Netlink message type. " << ifAddr.str();
    return EINVAL;
  } else if (ifAddr.getFamily() != AF_INET and ifAddr.getFamily() != AF_INET6) {
    XLOG(ERR) << "Invalid address family. " << ifAddr.str();
    return EINVAL;
  } else if (not ifAddr.getPrefix().has_value()) {
    // No IP address given
    XLOG(ERR) << "No interface address given. " << ifAddr.str();
    return EDESTADDRREQ;
  }

  init(type);
  // initialize netlink address fields
  auto ip = std::get<0>(ifAddr.getPrefix().value());
  uint8_t prefixLen = std::get<1>(ifAddr.getPrefix().value());
  ifaddrmsg_->ifa_family = ifAddr.getFamily();
  ifaddrmsg_->ifa_prefixlen = prefixLen;
  ifaddrmsg_->ifa_flags =
      (ifAddr.getFlags().has_value() ? ifAddr.getFlags().value() : 0);
  if (ifAddr.getScope().has_value()) {
    ifaddrmsg_->ifa_scope = ifAddr.getScope().value();
  }
  ifaddrmsg_->ifa_index = ifAddr.getIfIndex();

  const char* const ipptr = reinterpret_cast<const char*>(ip.bytes());
  int status = addAttributes(IFA_ADDRESS, ipptr, ip.byteCount());
  if (status) {
    return status;
  }
  // For IPv4, need to specify the ip address in IFA_LOCAL attribute as well
  // for point-to-point interfaces
  // For IPv6, the extra attribute has no effect
  return addAttributes(IFA_LOCAL, ipptr, ip.byteCount());
}

IfAddress
NetlinkAddrMessage::parseMessage(const struct nlmsghdr* nlmsg) {
  IfAddressBuilder builder;
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
          XLOG(ERR) << "Error parsing Netlink ADDR message";
        }
      }
    }
    }
  }
  auto addr = builder.build();
  XLOG(DBG3) << "Netlink parsed address message. " << addr.str();
  return addr;
}

} // namespace openr::fbnl
