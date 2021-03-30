/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkNeighborMessage.h>

namespace openr::fbnl {

NetlinkNeighborMessage::NetlinkNeighborMessage() {
  // get pointer to NLMSG header
  msghdr_ = getMessagePtr();
}

NetlinkNeighborMessage::~NetlinkNeighborMessage() {
  CHECK(neighborPromise_.isFulfilled());
}

void
NetlinkNeighborMessage::rcvdNeighbor(Neighbor&& neighbor) {
  rcvdNeighbors_.emplace_back(std::move(neighbor));
}

void
NetlinkNeighborMessage::setReturnStatus(int status) {
  if (status == 0) {
    neighborPromise_.setValue(std::move(rcvdNeighbors_));
  } else {
    neighborPromise_.setValue(folly::makeUnexpected(status));
  }
  NetlinkMessageBase::setReturnStatus(status);
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

Neighbor
NetlinkNeighborMessage::parseMessage(const struct nlmsghdr* nlmsg) {
  NeighborBuilder builder;
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
  Neighbor neighbor = builder.build();
  VLOG(3) << "Netlink parsed neighbor message. " << neighbor.str();
  return neighbor;
}

} // namespace openr::fbnl
