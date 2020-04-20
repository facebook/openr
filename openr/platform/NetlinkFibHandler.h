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
#include <folly/Expected.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/FibService.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/NeighborListenerClientForFibagent.h>
#include <openr/nl/NetlinkSocket.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr {
/**
 * This class implements OpenR's Platform.FibService thrit interface for
 * programming routes on Linux platform for packet routing in kernel
 *
 * TODO: Add UT for NetlinkFibHandler - for correctness of following APIs
 * - buildRoute / buildMplsRoute / buildNextHop / buildMplsAction
 * - Add/Del/Sync unicast routes (IPv4 & IPv6)
 * - Add/Del/Sync mpls routes
 * - NOTE: Use MockProtocolSocket to faciliate the UTs
 */
class NetlinkFibHandler : public thrift::FibServiceSvIf {
 public:
  explicit NetlinkFibHandler(
      fbzmq::ZmqEventLoop* zmqEventLoop,
      std::shared_ptr<fbnl::NetlinkSocket> netlinkSocket);
  ~NetlinkFibHandler() override;

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

  folly::Future<folly::Unit> future_addMplsRoute(
      int16_t clientId, std::unique_ptr<thrift::MplsRoute> route);

  folly::Future<folly::Unit> future_deleteMplsRoute(
      int16_t clientId, int32_t topLabel);

  folly::Future<folly::Unit> future_addMplsRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<thrift::MplsRoute>> mplsRoute) override;

  folly::Future<folly::Unit> future_deleteMplsRoutes(
      int16_t clientId,
      std::unique_ptr<std::vector<int32_t>> topLabels) override;

  folly::Future<folly::Unit> future_syncFib(
      int16_t clientId,
      std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) override;

  folly::Future<folly::Unit> future_syncMplsFib(
      int16_t clientId,
      std::unique_ptr<std::vector<thrift::MplsRoute>> routes) override;

  void sendNeighborDownInfo(
      std::unique_ptr<std::vector<std::string>> neighborIp) override;

  void async_eb_registerForNeighborChanged(
      std::unique_ptr<apache::thrift::HandlerCallback<void>> callback) override;

  int64_t aliveSince() override;

  facebook::fb303::cpp2::fb303_status getStatus() override;

  openr::thrift::SwitchRunState getSwitchRunState() override;

  void getCounters(std::map<std::string, int64_t>& counters) override;

  folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
  future_getRouteTableByClient(int16_t clientId) override;

  folly::Future<std::unique_ptr<std::vector<openr::thrift::MplsRoute>>>
  future_getMplsRouteTableByClient(int16_t clientId) override;

  std::shared_ptr<fbnl::NetlinkSocket>
  getNetlinkSocket() {
    return netlinkSocket_;
  }

  /**
   * Static API to convert protocol to clientId. Set exception in promise if
   * return value is not false;
   * TODO: Add UT for this API
   * TODO: Fix this by not taking promise as a parameter
   */
  template <class A>
  static folly::Expected<int16_t, bool> getProtocol(
      folly::Promise<A>& promise, int16_t clientId);

  /**
   * Convert clientId to client name
   * TODO: Add UT for this API
   */
  static std::string getClientName(const int16_t clientId);

  /**
   * Translate protocol identifier to priority
   * TODO: Add UT for this API
   */
  static uint8_t protocolToPriority(const uint8_t protocol);

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
      fbnl::NetlinkSocket::NeighborUpdate neighborUpdate);

  /**
   * APIs to convert netlink route representation to thrift. Used for sending
   * routes read from kernel to client.
   */
  std::vector<thrift::UnicastRoute> toThriftUnicastRoutes(
      const fbnl::NlUnicastRoutes& routeDb);
  std::vector<thrift::MplsRoute> toThriftMplsRoutes(
      const fbnl::NlMplsRoutes& routeDb);
  std::vector<thrift::NextHopThrift> buildNextHops(
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
  std::shared_ptr<fbnl::NetlinkSocket> netlinkSocket_;

  // ZMQ Eventloop pointer
  // TODO: Migrate to folly::EventBase
  fbzmq::ZmqEventLoop* evl_{nullptr};

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
