/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UDPRoutingPacketTransport.h"

using namespace openr::fbmeshd;

UDPRoutingPacketTransport::UDPRoutingPacketTransport(
    folly::EventBase* evb, int32_t tos)
    : evb_{evb}, clientSocket_{evb_} {
  evb_->runInEventBaseThread([this, tos]() {
    clientSocket_.bind(folly::SocketAddress("::", 0));
    clientSocket_.setTrafficClass(tos);
  });
}

void
UDPRoutingPacketTransport::sendPacket(
    folly::MacAddress da, std::unique_ptr<folly::IOBuf> buf) {
  evb_->runInEventBaseThread([this, da, buf = std::move(buf)]() {
    const auto destSockAddr = folly::SocketAddress{
        da.isBroadcast()
            ? folly::IPAddressV6{"ff02::1%mesh0"}
            : folly::IPAddressV6{folly::IPAddressV6{
                                     folly::IPAddressV6::LINK_LOCAL, da}
                                     .str() +
                                 "%mesh0"},
        6668};
    clientSocket_.write(destSockAddr, std::move(buf));
  });
}
