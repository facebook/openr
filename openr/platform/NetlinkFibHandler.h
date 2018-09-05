/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fbzmq/async/ZmqTimeout.h>
#include <folly/futures/Future.h>
#include <folly/Expected.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/FibService.h>
#include <openr/nl/NetlinkSocket.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr {
/**
 * This class implements OpenR's Platform.FibService thrit interface for
 * programming routes on Linux platform for packet routing in kernel
 */
class NetlinkFibHandler final : public thrift::FibServiceSvIf {
 public:
  explicit NetlinkFibHandler(
    fbzmq::ZmqEventLoop* zmqEventLoop,
    std::shared_ptr<fbnl::NetlinkSocket> netlinkSocket);
  ~NetlinkFibHandler() override {}

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

 private:
  NetlinkFibHandler(const NetlinkFibHandler&) = delete;
  NetlinkFibHandler& operator=(const NetlinkFibHandler&) = delete;

  template<class A>
  folly::Expected<int16_t, bool>
  getProtocol(folly::Promise<A>& promise, int16_t clientId);

  std::vector<thrift::UnicastRoute>
  toThriftUnicastRoutes(const fbnl::NlUnicastRoutes& routeDb);

  // Used to interact with Linux kernel routing table
  std::shared_ptr<fbnl::NetlinkSocket> netlinkSocket_;

  // Monotonic ID from periodicKeepAlive
  // Fib push whenever the ID has not incremented since last pull...
  int64_t keepAliveId_{0};

  // Time when service started, in number of seconds, since epoch
  const int64_t startTime_{0};

  // ZMQ Eventloop pointer
  fbzmq::ZmqEventLoop* evl_{nullptr};

  // Recent timepoint of aliveSince
  std::chrono::steady_clock::time_point recentKeepAliveTs_;

  // ZmqTimeout timer to check for openr aliveness
  std::unique_ptr<fbzmq::ZmqTimeout> keepAliveCheckTimer_;
};

} // namespace openr
