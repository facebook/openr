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

#include <fb303/BaseService.h>
#include <folly/Expected.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/FibService.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/NeighborListenerClientForFibagent.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr {
/**
 * This class implements OpenR's Platform.FibService thrit interface. It uses
 * NetlinkProtocolSocket to program routes in kernel. At a high level
 * - It translates thrift representation of routes to netlink for programming
 * - Translates netlink representation of routes to thrift for get* queries
 * - All APIs exposed are asynchronous. Sync API retries the existing routing
 *   state in synchronous way and program changes asynchrnously.
 */
class NetlinkFibHandler : public thrift::FibServiceSvIf,
                          public facebook::fb303::BaseService {
 public:
  explicit NetlinkFibHandler(fbnl::NetlinkProtocolSocket* nlSock);
  ~NetlinkFibHandler() override;

  void
  getCounters(std::map<std::string, int64_t>& /* counters */) override {
    // No counters. Return empty
  }

  folly::SemiFuture<folly::Unit> semifuture_addUnicastRoute(
      int16_t clientId, std::unique_ptr<thrift::UnicastRoute> route) override;

  folly::SemiFuture<folly::Unit> semifuture_deleteUnicastRoute(
      int16_t clientId, std::unique_ptr<thrift::IpPrefix> prefix) override;

  folly::SemiFuture<folly::Unit> semifuture_addUnicastRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) override;

  folly::SemiFuture<folly::Unit> semifuture_deleteUnicastRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<thrift::IpPrefix>> prefixes) override;

  folly::SemiFuture<folly::Unit> semifuture_addMplsRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<thrift::MplsRoute>> mplsRoute) override;

  folly::SemiFuture<folly::Unit> semifuture_deleteMplsRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<int32_t>> topLabels) override;

  folly::SemiFuture<folly::Unit> semifuture_syncFib(
      int16_t clientId,
      std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) override;

  folly::SemiFuture<folly::Unit> semifuture_syncMplsFib(
      int16_t clientId,
      std::unique_ptr<std::vector<thrift::MplsRoute>> routes) override;

  void sendNeighborDownInfo(
      std::unique_ptr<std::vector<std::string>> neighborIp) override;

  void async_eb_registerForNeighborChanged(
      std::unique_ptr<apache::thrift::HandlerCallback<void>> callback) override;

  int64_t aliveSince() override;

  facebook::fb303::cpp2::fb303_status getStatus() override;

  openr::thrift::SwitchRunState getSwitchRunState() override;

  folly::SemiFuture<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
  semifuture_getRouteTableByClient(int16_t clientId) override;

  folly::SemiFuture<std::unique_ptr<std::vector<openr::thrift::MplsRoute>>>
  semifuture_getMplsRouteTableByClient(int16_t clientId) override;

  /**
   * Static API to convert protocol to clientId
   */
  static std::optional<int16_t> getProtocol(int16_t clientId);

  /**
   * Convert clientId to client name
   */
  static std::string getClientName(const int16_t clientId);

  /**
   * Translate protocol identifier to priority
   */
  static uint8_t protocolToPriority(const uint8_t protocol);

  /**
   * Convert list<SemiFuture<int>> to SemiFuture<Unit>
   * The first error if any will be converted to NlException
   */
  static folly::SemiFuture<folly::Unit> collectAllResult(
      std::vector<folly::SemiFuture<int>>&& result,
      std::set<int> errorsToIgnore);

 protected:
  /**
   * TODO: Migrate BGP++ to stream API for neighbor notifications. Also need to
   * sync with WedgeAgent progress on it.
   */
  struct ThreadLocalListener {
    folly::EventBase* eventBase;
    std::unordered_map<
        const apache::thrift::server::TConnectionContext*,
        std::shared_ptr<thrift::NeighborListenerClientForFibagentAsyncClient>>
        clients;

    explicit ThreadLocalListener(folly::EventBase* eb) : eventBase(eb) {}
  };

  std::mutex listenersMutex_;
  folly::ThreadLocalPtr<ThreadLocalListener, int> listeners_;
  std::vector<const apache::thrift::TConnectionContext*> brokenClients_;

  void invokeNeighborListeners(
      ThreadLocalListener* listener,
      const std::vector<std::string>& neighborIps,
      bool isReachable);

  /**
   * APIs to convert netlink route representation to thrift. Used for sending
   * routes read from kernel to client.
   */
  std::vector<thrift::NextHopThrift> toThriftNextHops(
      const fbnl::NextHopSet& nextHopSet);

  /**
   * API to convert thrift route representation to netlink. Used for programming
   * routes in kernel.
   */
  fbnl::Route buildRoute(const thrift::UnicastRoute& route, int protocol);
  fbnl::Route buildMplsRoute(const thrift::MplsRoute& mplsRoute, int protocol);
  void buildMplsAction(
      fbnl::NextHopBuilder& nhBuilder, const thrift::NextHopThrift& nhop);
  void buildNextHop(
      fbnl::RouteBuilder& rtBuilder,
      const std::vector<thrift::NextHopThrift>& nhop);

  /**
   * APIs to convert ifName <-> ifIndex for thrift <-> netlink route conversions
   * Returns `folly::none` if can't find the mapping.
   *
   * Cache is used for optimized response to subsequent query for same interface
   * name or index. Entries in cache are lazily initialized on first instance by
   * querying `getAllLinks`.
   *
   * Returns `std::nullopt` if mapping is not found
   */
  std::optional<int> getIfIndex(const std::string& ifName);
  std::optional<std::string> getIfName(const int ifIndex);

  /**
   * Get interface index of loopback interface. Lazily query it from netlink
   * by querying `getAllLinks`
   */
  std::optional<int> getLoopbackIfIndex();

  // Used to interact with Linux kernel routing table
  fbnl::NetlinkProtocolSocket* nlSock_{nullptr};

 private:
  /**
   * Disable copy & assignment operators
   */
  NetlinkFibHandler(const NetlinkFibHandler&) = delete;
  NetlinkFibHandler& operator=(const NetlinkFibHandler&) = delete;

  /**
   * Initialize cache related to interfaces. Especially loopbackIfIndex_
   * and interface name <-> index mappings
   */
  void initializeInterfaceCache() noexcept;

  // Cache for interface index <-> name mapping
  folly::Synchronized<std::unordered_map<std::string, int>> ifNameToIndex_;
  folly::Synchronized<std::unordered_map<int, std::string>> ifIndexToName_;

  // Loopback interface index cache. Initialized to negative number
  std::atomic<int> loopbackIfIndex_{-1};

  // Time when service started, in number of seconds, since epoch
  const int64_t startTime_{0};
};

} // namespace openr
