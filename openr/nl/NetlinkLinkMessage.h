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

/*
 * Message specialization for rtnetlink LINK type
 *
 * For reference: https://man7.org/linux/man-pages/man7/rtnetlink.7.html
 *
 * RTM_NEWLINK, RTM_DELLINK, RTM_GETLINK
 *    Create, remove, or get information about a specific
 *    network interface.  These messages contain an ifinfomsg
 *    structure followed by a series of rtattr structures.
 *
 *    struct ifinfomsg {
 *      unsigned char  ifi_family;  // AF_UNSPEC
 *      unsigned short ifi_type;    // Device type
 *      int ifi_index;              // Interface index
 *      unsigned int ifi_flags;     // Device flags
 *      unsigned int ifi_change;    // change mask
 *    };
 *
 *    ifi_flags contains the device flags, see netdevice(7);
 *    ifi_index is the unique interface index(since Linux 3.7,
 *    it is possible to feed a nonzero value with the
 *    RTM_NEWLINK message, thus creating a link with the given
 *    ifindex);
 *    ifi_change is reserved for future use and should
 *    be always set to 0xFFFFFFFF.
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
