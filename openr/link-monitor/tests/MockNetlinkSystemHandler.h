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
#include <openr/nl/NetlinkSubscriber.h>
#include <openr/platform/PlatformPublisher.h>

namespace openr {

/**
 * This class implements Netlink Platform thrift interface for programming
 * NetlinkEvent Publisher as well as System Service on linux platform.
 */

class MockNetlinkSystemHandler final : public thrift::SystemServiceSvIf {
 public:
  MockNetlinkSystemHandler(
      fbzmq::Context& context, const std::string& platformPublisherUrl);

  ~MockNetlinkSystemHandler() override = default;

  MockNetlinkSystemHandler(const MockNetlinkSystemHandler&) = delete;
  MockNetlinkSystemHandler& operator=(const MockNetlinkSystemHandler&) = delete;

  void getAllLinks(std::vector<thrift::Link>& linkDb) override;

  void getAllNeighbors(std::vector<thrift::NeighborEntry>& neighDb) override;

  void sendLinkEvent(
      const std::string& ifName, const uint64_t ifIndex, const bool isUp);

  void sendAddrEvent(
      const std::string& ifName, const std::string& prefix, const bool isValid);

  void stop();

 private:
  // Used to publish Netlink event
  std::unique_ptr<PlatformPublisher> platformPublisher_;

  // Interface/nextHop-IP => MacAddress mapping
  folly::Synchronized<Neighbors> neighborDb_{};

  // Interface/link name => link attributes mapping
  folly::Synchronized<Links> linkDb_{};
};

} // namespace openr
