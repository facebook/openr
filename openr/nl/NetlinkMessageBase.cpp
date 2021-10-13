/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

#include <openr/nl/NetlinkMessageBase.h>

namespace openr::fbnl {

NetlinkMessageBase::NetlinkMessageBase()
    : msghdr_(reinterpret_cast<struct nlmsghdr*>(msg.data())) {}

NetlinkMessageBase::NetlinkMessageBase(int type)
    : msghdr_(reinterpret_cast<struct nlmsghdr*>(msg.data())) {
  // initialize netlink header
  msghdr_->nlmsg_len = NLMSG_LENGTH(0);
  msghdr_->nlmsg_type = type;
}

NetlinkMessageBase::~NetlinkMessageBase() {
  CHECK(promise_.isFulfilled());
}

struct nlmsghdr*
NetlinkMessageBase::getMessagePtr() {
  return msghdr_;
}

uint16_t
NetlinkMessageBase::getMessageType() const {
  return msghdr_->nlmsg_type;
}

uint32_t
NetlinkMessageBase::getDataLength() const {
  return msghdr_->nlmsg_len;
}

struct rtattr*
NetlinkMessageBase::addSubAttributes(
    struct rtattr* rta, int type, const void* data, uint32_t len) const {
  struct rtattr* subrta{nullptr};
  uint32_t subRtaLen = RTA_LENGTH(len);

  if (RTA_ALIGN(rta->rta_len) + RTA_ALIGN(subRtaLen) > kMaxNlPayloadSize) {
    XLOG(ERR) << "No buffer for adding attr: " << type << " length: " << len;
    return subrta;
  }

  XLOG(DBG3) << "Adding sub attribute. type=" << type << ", len=" << len;

  // add the subattribute
  subrta = (struct rtattr*)(((char*)rta) + RTA_ALIGN(rta->rta_len));
  subrta->rta_type = type;
  subrta->rta_len = subRtaLen;
  if (data) {
    memcpy(RTA_DATA(subrta), data, len);
  }

  // update the RTA length
  rta->rta_len = NLMSG_ALIGN(rta->rta_len) + RTA_ALIGN(subRtaLen);
  return subrta;
}

int
NetlinkMessageBase::addAttributes(
    int type, const char* const data, uint32_t len) {
  uint32_t rtaLen = (RTA_LENGTH(len));
  uint32_t nlmsgAlen = NLMSG_ALIGN((msghdr_)->nlmsg_len);

  if (nlmsgAlen + RTA_ALIGN(rtaLen) > kMaxNlPayloadSize) {
    XLOG(ERR) << "Space not available to add attribute type " << type;
    return ENOBUFS;
  }

  // set the pointer to the aligned location
  struct rtattr* rptr =
      reinterpret_cast<struct rtattr*>(((char*)(msghdr_)) + nlmsgAlen);
  rptr->rta_type = type;
  rptr->rta_len = rtaLen;
  XLOG(DBG3) << "Adding attribute. type=" << type << ", len=" << rtaLen;
  if (data) {
    memcpy(RTA_DATA(rptr), data, len);
  }

  // update the length in NL MSG header
  msghdr_->nlmsg_len = nlmsgAlen + RTA_ALIGN(rtaLen);
  return 0;
}

folly::SemiFuture<int>
NetlinkMessageBase::getSemiFuture() {
  return promise_.getSemiFuture();
}

void
NetlinkMessageBase::setReturnStatus(int status) {
  XLOG(DBG3) << "Netlink request completed. retval=" << status << ", "
             << folly::errnoStr(std::abs(status));
  promise_.setValue(status);
}

folly::Expected<folly::IPAddress, folly::IPAddressFormatError>
NetlinkMessageBase::parseIp(const struct rtattr* ipAttr, unsigned char family) {
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

} // namespace openr::fbnl
