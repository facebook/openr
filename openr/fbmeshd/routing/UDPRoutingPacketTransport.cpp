/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UDPRoutingPacketTransport.h"

#include <folly/Format.h>

using namespace openr::fbmeshd;

UDPRoutingPacketTransport::UDPRoutingPacketTransport(
    folly::EventBase* evb,
    const std::string& interface,
    uint16_t port,
    int32_t tos)
    : evb_{evb},
      interface_{interface},
      serverSocket_{evb_},
      clientSocket_{evb_} {
  evb_->runInEventBaseThread([this, port, tos]() {
    serverSocket_.bind(folly::SocketAddress{"::", port});
    serverSocket_.addListener(evb_, this);
    serverSocket_.listen();

    clientSocket_.bind(folly::SocketAddress("::", 0));
    clientSocket_.setTrafficClass(tos);
  });
}

void
UDPRoutingPacketTransport::onDataAvailable(
    std::shared_ptr<folly::AsyncUDPSocket> /* socket */,
    const folly::SocketAddress& client,
    std::unique_ptr<folly::IOBuf> data,
    bool /* truncated */) noexcept {
  if (receivePacketCallback_) {
    (*receivePacketCallback_)(
        *client.getIPAddress().asV6().getMacAddressFromLinkLocal(),
        std::move(data));
  }
}

void
UDPRoutingPacketTransport::sendPacket(
    folly::MacAddress da, std::unique_ptr<folly::IOBuf> buf) {
  evb_->runInEventBaseThread([this, da, buf = std::move(buf)]() {
    const auto destSockAddr = folly::SocketAddress{
        da.isBroadcast()
            ? folly::IPAddressV6{folly::sformat("ff02::1%{}", interface_)}
            : folly::IPAddressV6{folly::sformat(
                  "{}%{}",
                  folly::IPAddressV6{folly::IPAddressV6::LINK_LOCAL, da}.str(),
                  interface_)},
        6668};
    clientSocket_.write(destSockAddr, std::move(buf));
  });
}

void
UDPRoutingPacketTransport::setReceivePacketCallback(
    std::function<void(folly::MacAddress, std::unique_ptr<folly::IOBuf>)> cb) {
  if (evb_->isRunning()) {
    evb_->runImmediatelyOrRunInEventBaseThreadAndWait(
        [this, cb = std::move(cb)]() { receivePacketCallback_ = cb; });
  } else {
    receivePacketCallback_ = cb;
  }
}

void
UDPRoutingPacketTransport::resetReceivePacketCallback() {
  if (evb_->isRunning()) {
    evb_->runImmediatelyOrRunInEventBaseThreadAndWait(
        [this]() { receivePacketCallback_.reset(); });
  } else {
    receivePacketCallback_.reset();
  }
}
