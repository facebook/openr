/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netlink/netlink.h> // @manual

#include <glog/logging.h>

namespace openr {
namespace fbmeshd {

/*
 * Netlink message class which is a wrapper for a nl_msg*.
 */
class NetlinkMessage {
 public:
  NetlinkMessage() : msg_{nlmsg_alloc()} {
    CHECK_NOTNULL(msg_);
  }

  explicit NetlinkMessage(nl_msg* msg) : msg_{msg} {
    nlmsg_get(msg_);
  }

  NetlinkMessage(const NetlinkMessage&) = delete;
  NetlinkMessage& operator=(const NetlinkMessage&) = delete;

  ~NetlinkMessage() {
    nlmsg_free(msg_);
  }

  operator nl_msg*() const {
    return msg_;
  };

  nlmsghdr*
  getHeader() const {
    return nlmsg_hdr(msg_);
  }

 protected:
  nl_msg* msg_;
};

} // namespace fbmeshd
} // namespace openr
