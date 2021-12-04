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
 * Message specialization for rtnetlink ADDR type
 *
 * For reference: https://man7.org/linux/man-pages/man7/rtnetlink.7.html
 *
 * RTM_NEWADDR, RTM_DELADDR, RTM_GETADDR
 *    Add, remove, or receive information about an IP address
 *    associated with an interface.  In Linux 2.2, an interface
 *    can carry multiple IP addresses, this replaces the alias
 *    device concept in 2.0.  In Linux 2.2, these messages
 *    support IPv4 and IPv6 addresses.  They contain an
 *    `ifaddrmsg` structure, optionally followed by rtattr routing
 *    attributes.
 *
 * struct ifaddrmsg {
 *    unsigned char ifa_family;    // Address type
 *    unsigned char ifa_prefixlen; // Prefixlength of address
 *    unsigned char ifa_flags;     // Address flags
 *    unsigned char ifa_scope;     // Address scope
 *    unsigned int  ifa_index;     // Interface index
 * };
 */
class NetlinkAddrMessage final : public NetlinkMessageBase {
 public:
  NetlinkAddrMessage();

  ~NetlinkAddrMessage() override;

  // Override setReturnStatus. Set addrPromise_ with rcvdAddrs_
  void setReturnStatus(int status) override;

  // Get future for received addresses in response to GET request
  folly::SemiFuture<folly::Expected<std::vector<IfAddress>, int>>
  getAddrsSemiFuture() {
    return addrPromise_.getSemiFuture();
  }

  // initiallize address message with default params
  void init(int type);

  // create netlink message to add/delete interface address
  // type - RTM_NEWADDR or RTM_DELADDR
  int addOrDeleteIfAddress(const IfAddress& ifAddr, const int type);

  // parse Netlink Address message
  static IfAddress parseMessage(const struct nlmsghdr* nlh);

 private:
  // inherited class implementation
  void rcvdIfAddress(IfAddress&& ifAddr) override;

  int addCacheInfo(const IfAddress& ifAddr);

  //
  // Private variables for rtnetlink msg exchange
  //

  // pointer to interface message header
  struct ifaddrmsg* ifaddrmsg_{nullptr};

  // promise to be fulfilled when receiving kernel reply
  folly::Promise<folly::Expected<std::vector<IfAddress>, int>> addrPromise_;
  std::vector<IfAddress> rcvdAddrs_;
};

} // namespace openr::fbnl
