/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <folly/futures/Future.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/LinuxFibService.h>
#include <openr/if/gen-cpp2/LinuxPlatform_types.h>
#include <openr/nl/NetlinkRouteSocket.h>

namespace openr {
/**
 * This class implements OpenR's Platform.FibService thrit interface for
 * programming routes on Linux platform for packet routing in kernel
 */
class NetlinkFibHandler final : public thrift::LinuxFibServiceSvIf {
 public:
  explicit NetlinkFibHandler(fbzmq::ZmqEventLoop* zmqEventLoop);
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

  folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
  future_getRouteTableByClient(int16_t clientId) override;
  folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
  future_getKernelRouteTable() override;

 private:
  NetlinkFibHandler(const NetlinkFibHandler&) = delete;
  NetlinkFibHandler& operator=(const NetlinkFibHandler&) = delete;

  // Used to interact with Linux kernel routing table
  std::unique_ptr<NetlinkRouteSocket> netlinkSocket_;

  // Monotonic ID from periodicKeepAlive
  // Fib push whenever the ID has not incremented since last pull...
  int64_t keepAliveId_{0};

  // Time when service started, in number of seconds, since epoch
  const int64_t startTime_{0};
};

} // namespace openr
