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

extern "C" {
#include <linux/if_ether.h>
}

namespace openr::fbnl {

/*
 * Message specialization for rtnetlink NEIGHBOR type
 *
 * For reference: https://man7.org/linux/man-pages/man7/rtnetlink.7.html
 *
 * RTM_NEWNEIGH, RTM_DELNEIGH, RTM_GETNEIGH
 *    Add, remove, or receive information about a neighbor table
 *    entry (e.g., an ARP entry).  The message contains an ndmsg
 *    structure.
 *
 *    struct ndmsg {
 *        unsigned char ndm_family;
 *        int           ndm_ifindex;  // Interface index
 *        __u16         ndm_state;    // State
 *        __u8          ndm_flags;    // Flags
 *        __u8          ndm_type;
 *    };
 *
 *     ndm_state is a bit mask of the following states:
 *
 *    NUD_INCOMPLETE   a currently resolving cache entry
 *    NUD_REACHABLE    a confirmed working cache entry
 *    NUD_STALE        an expired cache entry
 *    NUD_DELAY        an entry waiting for a timer
 *    NUD_PROBE        a cache entry that is currently reprobed
 *    NUD_FAILED       an invalid cache entry
 *    NUD_NOARP        a device with no destination cache
 *    NUD_PERMANENT    a static entry
 *
 *    Valid ndm_flags are:
 *
 *    NTF_PROXY    a proxy arp entry
 *    NTF_ROUTER   an IPv6 router
 *
 *    The rtattr struct has the following meanings for the
 *    rta_type field:
 *
 *    NDA_UNSPEC      unknown type
 *    NDA_DST         a neighbor cache n/w layer destination address
 *    NDA_LLADDR      a neighbor cache link layer address
 *    NDA_CACHEINFO   cache statistics
 *
 *    If the rta_type field is NDA_CACHEINFO, then a struct
 *    nda_cacheinfo header follows.
 */
class NetlinkNeighborMessage final : public NetlinkMessageBase {
 public:
  NetlinkNeighborMessage();

  ~NetlinkNeighborMessage() override;

  // Override setReturnStatus. Set neighborPromise_ with rcvdNeighbors_
  void setReturnStatus(int status) override;

  // Get future for received neighbors in response to GET request
  folly::SemiFuture<folly::Expected<std::vector<Neighbor>, int>>
  getNeighborsSemiFuture() {
    return neighborPromise_.getSemiFuture();
  }

  // initiallize neighbor message with default params
  void init(int type, uint32_t flags);

  // parse Netlink Neighbor message
  static Neighbor parseMessage(const struct nlmsghdr* nlh);

 private:
  // inherited class implementation
  void rcvdNeighbor(Neighbor&& ifAddr) override;

  // pointer to neighbor message header
  struct ndmsg* ndmsg_{nullptr};

  // promise to be fulfilled when receiving kernel reply
  folly::Promise<folly::Expected<std::vector<Neighbor>, int>> neighborPromise_;
  std::vector<Neighbor> rcvdNeighbors_;
};

} // namespace openr::fbnl
