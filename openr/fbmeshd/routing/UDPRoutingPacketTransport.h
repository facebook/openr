/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/MacAddress.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncUDPServerSocket.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBase.h>

namespace openr {
namespace fbmeshd {

class UDPRoutingPacketTransport : public folly::AsyncUDPServerSocket::Callback {
 public:
  UDPRoutingPacketTransport(folly::EventBase* evb, uint16_t port, int32_t tos);

  void sendPacket(folly::MacAddress da, std::unique_ptr<folly::IOBuf> buf);

  void setReceivePacketCallback(
      std::function<void(folly::MacAddress, std::unique_ptr<folly::IOBuf>)> cb);
  void resetReceivePacketCallback();

 private:
  virtual void
  onListenStarted() noexcept override {}

  virtual void
  onListenStopped() noexcept override {}

  virtual void onDataAvailable(
      std::shared_ptr<folly::AsyncUDPSocket> socket,
      const folly::SocketAddress& client,
      std::unique_ptr<folly::IOBuf> data,
      bool truncated) noexcept override;

  folly::EventBase* evb_;

  folly::AsyncUDPServerSocket serverSocket_;

  folly::AsyncUDPSocket clientSocket_;

  folly::Optional<
      std::function<void(folly::MacAddress, std::unique_ptr<folly::IOBuf>)>>
      receivePacketCallback_;
};

} // namespace fbmeshd
} // namespace openr
