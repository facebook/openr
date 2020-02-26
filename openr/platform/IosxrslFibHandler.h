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
#include <openr/if/gen-cpp2/FibService.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/iosxrsl/ServiceLayerRshuttle.h>

namespace openr {

extern const uint8_t kAqRouteProtoId;

using IosxrslServer = std::unordered_set<std::pair<std::string, std::string>>;

/**
 * This class implements OpenR's Platform.FibService thrit interface for
 * programming routes on Linux platform for packet routing in kernel
 */
class IosxrslFibHandler final : public thrift::FibServiceSvIf {
 public:
  explicit IosxrslFibHandler(fbzmq::ZmqEventLoop* zmqEventLoop,
                             std::vector<VrfData> vrfSet,
                             std::shared_ptr<grpc::Channel> Channel);
  ~IosxrslFibHandler() override {}

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

  void getCounters(std::map<std::string, int64_t>& counters) override;

  void setVrfContext(std::string);
  folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
  future_getRouteTableByClient(int16_t clientId) override;
//  folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
//  future_getIosxrRouteTable() override;

 private:
  IosxrslFibHandler(const IosxrslFibHandler&) = delete;
  IosxrslFibHandler& operator=(const IosxrslFibHandler&) = delete;

  // Used to interact with the IOS-XR routing table
  std::unique_ptr<IosxrslRshuttle> iosxrslRshuttle_;

  // Monotonic ID from periodicKeepAlive
  // Fib push whenever the ID has not incremented since last pull...
  int64_t keepAliveId_{0};

  // Time when service started, in number of seconds, since epoch
  const int64_t startTime_{0};
};

} // namespace openr
