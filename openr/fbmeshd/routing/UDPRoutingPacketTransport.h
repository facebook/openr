/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/MacAddress.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBase.h>

namespace openr {
namespace fbmeshd {

class UDPRoutingPacketTransport : public folly::EventBase {
 public:
  UDPRoutingPacketTransport(folly::EventBase* evb, int32_t tos);

  void sendPacket(folly::MacAddress da, std::unique_ptr<folly::IOBuf> buf);

 private:
  folly::EventBase* evb_;

  folly::AsyncUDPSocket clientSocket_;
};

} // namespace fbmeshd
} // namespace openr
