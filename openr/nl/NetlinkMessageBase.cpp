/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkMessageBase.h>

namespace openr::fbnl {

NetlinkMessage::NetlinkMessage()
    : msghdr(reinterpret_cast<struct nlmsghdr*>(msg.data())) {}

NetlinkMessage::NetlinkMessage(int type)
    : msghdr(reinterpret_cast<struct nlmsghdr*>(msg.data())) {
  // initialize netlink header
  msghdr->nlmsg_len = NLMSG_LENGTH(0);
  msghdr->nlmsg_type = type;
}

NetlinkMessage::~NetlinkMessage() {
  CHECK(promise_.isFulfilled());
}

struct nlmsghdr*
NetlinkMessage::getMessagePtr() {
  return msghdr;
}

uint16_t
NetlinkMessage::getMessageType() const {
  return msghdr->nlmsg_type;
}

uint32_t
NetlinkMessage::getDataLength() const {
  return msghdr->nlmsg_len;
}

struct rtattr*
NetlinkMessage::addSubAttributes(
    struct rtattr* rta, int type, const void* data, uint32_t len) const {
  struct rtattr* subrta{nullptr};
  uint32_t subRtaLen = RTA_LENGTH(len);

  if (RTA_ALIGN(rta->rta_len) + RTA_ALIGN(subRtaLen) > kMaxNlPayloadSize) {
    LOG(ERROR) << "No buffer for adding attr: " << type << " length: " << len;
    return subrta;
  }

  VLOG(3) << "Adding sub attribute. type=" << type << ", len=" << len;

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
NetlinkMessage::addAttributes(
    int type,
    const char* const data,
    uint32_t len,
    struct nlmsghdr* const msghdr) {
  uint32_t rtaLen = (RTA_LENGTH(len));
  uint32_t nlmsgAlen = NLMSG_ALIGN((msghdr)->nlmsg_len);

  if (nlmsgAlen + RTA_ALIGN(rtaLen) > kMaxNlPayloadSize) {
    LOG(ERROR) << "Space not available to add attribute type " << type;
    return ENOBUFS;
  }

  // set the pointer to the aligned location
  struct rtattr* rptr =
      reinterpret_cast<struct rtattr*>(((char*)(msghdr)) + nlmsgAlen);
  rptr->rta_type = type;
  rptr->rta_len = rtaLen;
  VLOG(3) << "Adding attribute. type=" << type << ", len=" << rtaLen;
  if (data) {
    memcpy(RTA_DATA(rptr), data, len);
  }

  // update the length in NL MSG header
  msghdr->nlmsg_len = nlmsgAlen + RTA_ALIGN(rtaLen);
  return 0;
}

folly::SemiFuture<int>
NetlinkMessage::getSemiFuture() {
  return promise_.getSemiFuture();
}

void
NetlinkMessage::setReturnStatus(int status) {
  VLOG(3) << "Netlink request completed. retval=" << status << ", "
          << folly::errnoStr(std::abs(status));
  promise_.setValue(status);
}

} // namespace openr::fbnl
