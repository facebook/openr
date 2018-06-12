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
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/futures/Future.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/NetlinkService.h>
#include <openr/if/gen-cpp2/LinuxNetlinkService.h>
#include <openr/platform/PlatformPublisher.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/LinuxPlatform_types.h>
#include <openr/nl/NetlinkRouteSocket.h>

namespace openr {

  /**
   * This class implements NetlinkService thrift interface for programming
   * NetlinkEvent Publisher, System Service and Kernel Packet Routing
   * on linux platform. We implement the futures API to allow for easy async
   * activity within the handlers
   */
  class NetlinkServiceHandler
      final : public thrift::LinuxNetlinkServiceSvIf {
   public:
    NetlinkServiceHandler(
        fbzmq::Context& context,
        const PlatformPublisherUrl& platformPublisherUrl,
        fbzmq::ZmqEventLoop* zmqEventLoop);

    NetlinkServiceHandler(
        fbzmq::Context& contex,
        const PlatformPublisherUrl& platformPublisherUrl,
        fbzmq::ZmqEventLoop* fibEventLoop,
        fbzmq::ZmqEventLoop* systemEventLoop);

    ~NetlinkServiceHandler() override;

    NetlinkServiceHandler(const NetlinkServiceHandler&) = delete;

    NetlinkServiceHandler&
    operator=(const NetlinkServiceHandler&) = delete;

    folly::Future<std::unique_ptr<std::vector<thrift::Link>>>
    future_getAllLinks() override;

    folly::Future<std::unique_ptr<std::vector<thrift::NeighborEntry>>>
    future_getAllNeighbors() override;

    folly::Future<folly::Unit> future_addUnicastRoute(
        int16_t clientId, std::unique_ptr<thrift::UnicastRoute> route) override;

    folly::Future<folly::Unit> future_deleteUnicastRoute(
        int16_t clientId, std::unique_ptr<thrift::IpPrefix> prefix) override;

    folly::Future<folly::Unit> future_addUnicastRoutes(
        int16_t clientId,
        std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) override;

    folly::Future<folly::Unit> future_deleteUnicastRoutes(
        int16_t clientId,
        std::unique_ptr<std::vector<thrift::IpPrefix>> prefixes) override;

    folly::Future<folly::Unit> future_syncFib(
        int16_t clientId,
        std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) override;

    folly::Future<int64_t> future_periodicKeepAlive(int16_t clientId) override;

    int64_t aliveSince() override;

    thrift::ServiceStatus getStatus() override;

    void getCounters(std::map<std::string, int64_t>& counters) override;

    folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
    future_getRouteTableByClient(int16_t clientId) override;

    folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
    future_getKernelRouteTable() override;

  private:
    void initHandler();

  private:
    // Used to interact with Linux kernel routing table
    std::unique_ptr<NetlinkRouteSocket> netlinkSocket_;

    // Monotonic ID from periodicKeepAlive
    // Fib push whenever the ID has not incremented since last pull...
    int64_t keepAliveId_{0};

    // Time when service started, in number of seconds, since epoch
    const int64_t startTime_{0};

    // ZMQ Eventloop pointer for Fib
    fbzmq::ZmqEventLoop* fibEvl_{nullptr};

    // Recent timepoint of aliveSince
    std::chrono::steady_clock::time_point recentKeepAliveTs_;

    // ZmqTimeout timer to check for openr aliveness
    std::unique_ptr<fbzmq::ZmqTimeout> keepAliveCheckTimer_;

    // Implementation class for NetlinkSubscriber internals
    class NLSubscriberImpl;
    std::unique_ptr<NLSubscriberImpl> nlImpl_;
  };
} // namespce openr
