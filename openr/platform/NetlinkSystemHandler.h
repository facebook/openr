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
#include <openr/nl/NetlinkRouteSocket.h>
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
      fbzmq::ZmqEventLoop* zmqEventLoop,
      std::shared_ptr<NetlinkRouteSocket> netlinkRouteSocket);

  ~NetlinkSystemHandler() override;

  NetlinkSystemHandler(const NetlinkSystemHandler&) = delete;
  NetlinkSystemHandler& operator=(const NetlinkSystemHandler&) = delete;

  folly::Future<std::unique_ptr<std::vector<thrift::Link>>> future_getAllLinks()
      override;

  folly::Future<std::unique_ptr<std::vector<thrift::NeighborEntry>>>
  future_getAllNeighbors() override;

  folly::Future<folly::Unit> future_addIfaceAddresses(
    std::unique_ptr<std::string> iface,
    std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) override;

  folly::Future<folly::Unit> future_removeIfaceAddresses(
    std::unique_ptr<std::string> iface,
    std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) override;

  folly::Future<folly::Unit> future_syncIfaceAddresses(
    std::unique_ptr<std::string> iface,
    int16_t family, int16_t scope,
    std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) override;

 private:
  void initNetlinkSystemHandler();

  void addIfaceAddrInternal(
    const std::string& ifName,
    const folly::CIDRNetwork& prefix);

  void removeIfaceAddrInternal(
    const std::string& ifName,
    const folly::CIDRNetwork& prefix);

  void syncIfaceAddrsInternal(
    const std::string& ifName,
    int16_t family,
    int16_t scope,
    const std::vector<::openr::thrift::IpPrefix>& addrs);

  // Implementation class for NetlinkSystemHandler internals
  class NLSubscriberImpl;
  std::unique_ptr<NLSubscriberImpl> nlImpl_;
  fbzmq::ZmqEventLoop* mainEventLoop_;
  std::shared_ptr<NetlinkRouteSocket> netlinkRouteSocket_;
};

} // namespace openr
