/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>

#include <openr/if/gen-cpp2/FibService.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/messaging/Queue.h>

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

  void addMplsRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<openr::thrift::MplsRoute>> routes) override;

  void deleteMplsRoutes(
      int16_t clientId, std::unique_ptr<std::vector<int32_t>> labels) override;

  void syncMplsFib(
      int16_t clientId,
      std::unique_ptr<std::vector<openr::thrift::MplsRoute>> routes) override;

  // Wait for adding/deleting routes to complete
  void waitForUpdateUnicastRoutes();
  void waitForDeleteUnicastRoutes();
  void waitForSyncFib();
  void waitForUpdateMplsRoutes();
  void waitForDeleteMplsRoutes();
  void waitForSyncMplsFib();
  void waitForUnhealthyException(size_t count = 1);

  int64_t aliveSince() override;

  void getRouteTableByClient(
      std::vector<openr::thrift::UnicastRoute>& routes,
      int16_t clientId) override;

  void getMplsRouteTableByClient(
      std::vector<::openr::thrift::MplsRoute>& routes,
      int16_t clientId) override;

  size_t
  getFibSyncCount() {
    return fibSyncCount_;
  }
  size_t
  getAddRoutesCount() {
    return addRoutesCount_;
  }

  size_t
  getDelRoutesCount() {
    return delRoutesCount_;
  }
  size_t
  getFibMplsSyncCount() {
    return fibMplsSyncCount_;
  }
  size_t
  getAddMplsRoutesCount() {
    return addMplsRoutesCount_;
  }
  size_t
  getDelMplsRoutesCount() {
    return delMplsRoutesCount_;
  }

  void
  setHandlerHealthyState(bool isHealthy) {
    isHealthy_ = isHealthy;
  }

  /**
   * Marks following prefixes as dirty. This means any subsequent updates about
   * these prefixes would fail to program. Deletion may succeed.
   */
  void setDirtyState(
      std::vector<folly::CIDRNetwork> const& dirtyPrefixes,
      std::vector<int32_t> const& dirtyLabels);

  void stop();

  void restart();

 private:
  // Make sure the FibHandler is in healthy state. Else throw exception
  void ensureHealthy();

  // Time when service started, in number of seconds, since epoch
  folly::Synchronized<int64_t> startTime_{0};

  // Abstract route Db to hide kernel level routing details from Fib
  folly::Synchronized<UnicastRoutes> unicastRouteDb_{};

  // Mpls Route db
  folly::Synchronized<
      std::unordered_map<int32_t, std::vector<thrift::NextHopThrift>>>
      mplsRouteDb_;

  // Dirty prefixes & labels in HW, and also won't be accepted from clients
  folly::Synchronized<std::unordered_set<folly::CIDRNetwork>> dirtyPrefixes_;
  folly::Synchronized<std::unordered_set<int32_t>> dirtyLabels_;

  // Stats
  std::atomic<size_t> fibSyncCount_{0};
  std::atomic<size_t> addRoutesCount_{0};
  std::atomic<size_t> delRoutesCount_{0};
  std::atomic<size_t> fibMplsSyncCount_{0};
  std::atomic<size_t> addMplsRoutesCount_{0};
  std::atomic<size_t> delMplsRoutesCount_{0};
  std::atomic<bool> isHealthy_{true};

  // A baton for synchronization
  folly::Baton<> updateUnicastRoutesBaton_;
  folly::Baton<> deleteUnicastRoutesBaton_;
  folly::Baton<> syncFibBaton_;
  folly::Baton<> updateMplsRoutesBaton_;
  folly::Baton<> deleteMplsRoutesBaton_;
  folly::Baton<> syncMplsFibBaton_;

  // We use queue for signalling & waiting for unhealthy exceptions as it can
  // be repetitive
  messaging::RWQueue<folly::Unit> unhealthyExceptionQueue_;
};

} // namespace openr
