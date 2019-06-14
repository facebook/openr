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
#include <thrift/lib/cpp/async/TAsyncSocket.h>

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
 */
class NetlinkFibHandler final : public thrift::FibServiceSvIf {
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

  facebook::fb303::cpp2::fb_status getStatus() override;

  void getCounters(std::map<std::string, int64_t>& counters) override;

  folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
  future_getRouteTableByClient(int16_t clientId) override;

  folly::Future<std::unique_ptr<std::vector<openr::thrift::MplsRoute>>>
  future_getMplsRouteTableByClient(int16_t clientId) override;

 private:
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

  NetlinkFibHandler(const NetlinkFibHandler&) = delete;
  NetlinkFibHandler& operator=(const NetlinkFibHandler&) = delete;

  template <class A>
  folly::Expected<int16_t, bool> getProtocol(
      folly::Promise<A>& promise, int16_t clientId);

  std::vector<thrift::UnicastRoute> toThriftUnicastRoutes(
      const fbnl::NlUnicastRoutes& routeDb);

  std::vector<thrift::MplsRoute> toThriftMplsRoutes(
      const fbnl::NlMplsRoutes& routeDb);

  std::vector<thrift::NextHopThrift> buildNextHops(
      const fbnl::NextHopSet& nextHopSet);

  fbnl::Route buildRoute(const thrift::UnicastRoute& route, int protocol) const
      noexcept;

  fbnl::Route buildMplsRoute(
      const thrift::MplsRoute& mplsRoute, int protocol) const noexcept;

  void buildMplsAction(
      fbnl::NextHopBuilder& nhBuilder, const thrift::NextHopThrift& nhop) const;

  void buildNextHop(
      fbnl::RouteBuilder& rtBuilder,
      const std::vector<thrift::NextHopThrift>& nhop) const;

  // This function only gets used when enable_recursive_lookup flags is set
  // Do recursive look up among static routes for current nexthop
  // if an entry is found, replace current nexthop with final nexthop
  // E.g. if "10.0.0.0/32 via 127.0.0.1 protoco static" exits,
  //     "ip add 1.1.1.1/32 via 10.0.0.0" will be resolved to
  //     "ip add 1.1.1.1/32 via 127.0.0.1"
  fbnl::NextHopSet lookupNexthop(const thrift::BinaryAddress& nh) const
      noexcept;

  // Used to interact with Linux kernel routing table
  std::shared_ptr<fbnl::NetlinkSocket> netlinkSocket_;

  // Time when service started, in number of seconds, since epoch
  const int64_t startTime_{0};

  // ZMQ Eventloop pointer
  fbzmq::ZmqEventLoop* evl_{nullptr};

  // Recent timepoint of aliveSince
  std::chrono::steady_clock::time_point recentKeepAliveTs_;

  // ZmqTimeout timer to check for openr aliveness
  std::unique_ptr<fbzmq::ZmqTimeout> keepAliveCheckTimer_;

  // cached static routes for recursive lookup
  fbnl::NlUnicastRoutes staticRouteCache_;

  // ZmqTimeout timer to periodically sync static routes
  std::unique_ptr<fbzmq::ZmqTimeout> syncStaticRouteTimer_;
};

} // namespace openr
