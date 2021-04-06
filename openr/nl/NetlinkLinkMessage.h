/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/nl/NetlinkMessageBase.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr::fbnl {

/**
 * Message specialization for LINK object
 */
class NetlinkLinkMessage final : public NetlinkMessageBase {
 public:
  NetlinkLinkMessage();

  ~NetlinkLinkMessage() override;

  // Override setReturnStatus. Set linkPromise_ with rcvdLinks_
  void setReturnStatus(int status) override;

  // Get future for received links in response to GET request
  folly::SemiFuture<folly::Expected<std::vector<Link>, int>>
  getLinksSemiFuture() {
    return linkPromise_.getSemiFuture();
  }

  // initiallize link message with default params
  void init(int type, uint32_t flags);

  // parse Netlink Link message
  static Link parseMessage(const struct nlmsghdr* nlh);

 private:
  // inherited class implementation
  void rcvdLink(Link&& link) override;

  //
  // Private variables for rtnetlink msg exchange
  //

  // pointer to link message header
  struct ifinfomsg* ifinfomsg_{nullptr};

  // promise to be fulfilled when receiving kernel reply
  folly::Promise<folly::Expected<std::vector<Link>, int>> linkPromise_;
  std::vector<Link> rcvdLinks_;
};

} // namespace openr::fbnl
