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

#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/FibService.h>

namespace openr {

// nextHop => local interface and nextHop IP.
using NextHops = std::unordered_set<std::pair<std::string, folly::IPAddress>>;

// Route => prefix and its possible nextHops
using UnicastRoutes = std::unordered_map<folly::CIDRNetwork, NextHops>;

/**
 * This class implements Netlink Platform thrift interface for programming
 * NetlinkEvent Publisher as well as Fib Service on linux platform.
 */

class MockNetlinkFibHandler final : public thrift::FibServiceSvIf {
 public:
  MockNetlinkFibHandler();

  ~MockNetlinkFibHandler() override = default;

  MockNetlinkFibHandler(const MockNetlinkFibHandler&) = delete;
  MockNetlinkFibHandler& operator=(const MockNetlinkFibHandler&) = delete;

  void addUnicastRoute(
      int16_t clientId,
      std::unique_ptr<openr::thrift::UnicastRoute> route) override;

  void deleteUnicastRoute(
      int16_t clientId,
      std::unique_ptr<openr::thrift::IpPrefix> prefix) override;

  void addUnicastRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<openr::thrift::UnicastRoute>> routes)
      override;

  void deleteUnicastRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<openr::thrift::IpPrefix>> prefixes) override;

  void syncFib(
      int16_t clientId,
      std::unique_ptr<std::vector<openr::thrift::UnicastRoute>> routes)
      override;

  int64_t aliveSince() override;

  void getRouteTableByClient(
      std::vector<openr::thrift::UnicastRoute>& routes,
      int16_t clientId) override;

  int64_t getFibSyncCount();
  int64_t getAddRoutesCount();
  int64_t getDelRoutesCount();

  void stop();

  void restart();

 private:
  // Time when service started, in number of seconds, since epoch
  folly::Synchronized<int64_t> startTime_{0};

  // Abstract route Db to hide kernel level routing details from Fib
  folly::Synchronized<UnicastRoutes> unicastRouteDb_{};

  // Number of times Fib syncs with this agent
  folly::Synchronized<int64_t> countSync_{0};

  folly::Synchronized<int64_t> countAddRoutes_{0};
  folly::Synchronized<int64_t> countDelRoutes_{0};
};

} // namespace openr
