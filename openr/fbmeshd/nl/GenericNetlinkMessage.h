/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netlink/genl/genl.h> // @manual
#include <netlink/netlink.h> // @manual

#include <openr/fbmeshd/nl/GenericNetlinkFamily.h>
#include <openr/fbmeshd/nl/NetlinkMessage.h>
#include <openr/fbmeshd/nl/TabularNetlinkAttribute.h>

namespace openr {
namespace fbmeshd {

/*
 * Generic Netlink message
 */
class GenericNetlinkMessage : public NetlinkMessage {
 public:
  explicit GenericNetlinkMessage(nl_msg* msg) : NetlinkMessage(msg) {}

  explicit GenericNetlinkMessage(
      const GenericNetlinkFamily& family, uint8_t cmd, int flags = 0)
      : NetlinkMessage() {
    genlmsg_put(
        msg_,
        NL_AUTO_PORT,
        NL_AUTO_SEQ,
        static_cast<int>(family),
        0,
        flags,
        cmd,
        0);
  }

  GenericNetlinkMessage(const GenericNetlinkMessage&) = delete;
  GenericNetlinkMessage& operator=(const GenericNetlinkMessage&) = delete;

  genlmsghdr*
  getGenericHeader() const {
    return genlmsg_hdr(getHeader());
  }

  template <size_t size>
  auto
  getAttributes() const {
    const auto gnlh = getGenericHeader();
    return TabularNetlinkAttribute<size>{genlmsg_attrdata(gnlh, 0),
                                         genlmsg_attrlen(gnlh, 0)};
  }
};

} // namespace fbmeshd
} // namespace openr
