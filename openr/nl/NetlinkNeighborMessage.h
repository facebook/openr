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

/**
 * Message specialization for NEIGHBOR object
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
