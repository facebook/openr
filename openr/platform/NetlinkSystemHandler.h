/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <syslog.h>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/futures/Future.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/SystemService.h>
#include <openr/platform/PlatformPublisher.h>

namespace openr {

/**
 * This class implements Netlink Platform thrift interface for programming
 * NetlinkEvent Publisher as well as System Service on linux platform.
 * We implement the futures API to allow for easy async activity within
 * the handlers
 */

class NetlinkSystemHandler final : public thrift::SystemServiceSvIf {
 public:
  NetlinkSystemHandler(
      fbzmq::Context& context,
      const PlatformPublisherUrl& platformPublisherUrl,
      fbzmq::ZmqEventLoop* zmqEventLoop);

  ~NetlinkSystemHandler() override;

  NetlinkSystemHandler(const NetlinkSystemHandler&) = delete;
  NetlinkSystemHandler& operator=(const NetlinkSystemHandler&) = delete;

  folly::Future<std::unique_ptr<std::vector<thrift::Link>>> future_getAllLinks()
      override;

  folly::Future<std::unique_ptr<std::vector<thrift::NeighborEntry>>>
  future_getAllNeighbors() override;

 private:
  void initNetlinkSystemHandler();

  // Implementation class for NetlinkSystemHandler internals
  class NLSubscriberImpl;
  std::unique_ptr<NLSubscriberImpl> nlImpl_;
};

} // namespace openr
