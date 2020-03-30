/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkMessage.h>

namespace openr::fbnl {

NetlinkMessage::NetlinkMessage()
    : msghdr(reinterpret_cast<struct nlmsghdr*>(msg.data())),
      promise_(std::make_unique<folly::Promise<int>>()) {}

NetlinkMessage::NetlinkMessage(int type)
    : msghdr(reinterpret_cast<struct nlmsghdr*>(msg.data())),
      promise_(std::make_unique<folly::Promise<int>>()) {
  // initialize netlink header
  msghdr->nlmsg_len = NLMSG_LENGTH(0);
  msghdr->nlmsg_type = type;
}

struct nlmsghdr*
NetlinkMessage::getMessagePtr() {
  return msghdr;
}

void
NetlinkMessage::updateBytesReceived(uint16_t bytes) {
  size_ = bytes;
}

uint32_t
NetlinkMessage::getDataLength() const {
  return msghdr->nlmsg_len;
}

struct rtattr*
NetlinkMessage::addSubAttributes(
    struct rtattr* rta, int type, const void* data, uint32_t len) const {
  uint32_t subRtaLen = RTA_LENGTH(len);

  if (RTA_ALIGN(rta->rta_len) + RTA_ALIGN(subRtaLen) > size_) {
    VLOG(1) << "Space not available to add sub attribute type " << type;
    return nullptr;
  }

  VLOG(2) << "Sub attribute type : " << type << " Len: " << len;

  // add the subattribute
  struct rtattr* subrta =
      (struct rtattr*)(((char*)rta) + RTA_ALIGN(rta->rta_len));
  subrta->rta_type = type;
  subrta->rta_len = subRtaLen;
  if (data) {
    memcpy(RTA_DATA(subrta), data, len);
  }

  // update the RTA length
  rta->rta_len = NLMSG_ALIGN(rta->rta_len) + RTA_ALIGN(subRtaLen);
  return subrta;
}

ResultCode
NetlinkMessage::addAttributes(
    int type,
    const char* const data,
    uint32_t len,
    struct nlmsghdr* const msghdr) {
  uint32_t rtaLen = (RTA_LENGTH(len));
  uint32_t nlmsgAlen = NLMSG_ALIGN((msghdr)->nlmsg_len);

  if (nlmsgAlen + RTA_ALIGN(rtaLen) > size_) {
    VLOG(1) << "Space not available to add attribute type " << type;
    return ResultCode::NO_MESSAGE_BUFFER;
  }

  // set the pointer to the aligned location
  struct rtattr* rptr =
      reinterpret_cast<struct rtattr*>(((char*)(msghdr)) + nlmsgAlen);
  rptr->rta_type = type;
  rptr->rta_len = rtaLen;
  VLOG(2) << "Attribute type : " << type << " Len: " << rtaLen;
  if (data) {
    memcpy(RTA_DATA(rptr), data, len);
  }

  // update the length in NL MSG header
  msghdr->nlmsg_len = nlmsgAlen + RTA_ALIGN(rtaLen);
  return ResultCode::SUCCESS;
}

folly::Future<int>
NetlinkMessage::getFuture() {
  return promise_->getFuture();
}

void
NetlinkMessage::setReturnStatus(int status) {
  promise_->setValue(status);
}

// get Message Type
NetlinkMessage::MessageType
NetlinkMessage::getMessageType() const {
  return messageType_;
}

// set Message Type
void
NetlinkMessage::setMessageType(NetlinkMessage::MessageType type) {
  messageType_ = type;
}

} // namespace openr::fbnl
