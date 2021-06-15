/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/common/NetworkUtil.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/tests/mocks/MockNetlinkFibHandler.h>

using namespace std;
using namespace openr;

using apache::thrift::BaseThriftServer;
using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;

const int16_t kFibId{static_cast<int16_t>(thrift::FibClient::OPENR)};

const auto prefix1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto prefix2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto prefix3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto prefix4 = toIpPrefix("::ffff:10.4.4.4/128");

const auto bestRoute1 = createPrefixEntry(prefix1);
const auto bestRoute2 = createPrefixEntry(prefix2);
const auto bestRoute3 = createPrefixEntry(prefix3);

const auto label1{1};
const auto label2{2};
const auto label3{3};

const auto path1_2_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    1);
const auto path1_2_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2);
const auto path1_2_3 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_3"),
    1);
const auto path1_3_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_1"),
    2);
const auto path1_3_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_2"),
    2);
const auto path3_2_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_3_2_1"),
    1);
const auto path3_2_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_3_2_2"),
    2);
const auto path3_4_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::4")),
    std::string("iface_3_4_1"),
    2);
const auto path3_4_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::4")),
    std::string("iface_3_4_2"),
    2);

const auto mpls_path1_2_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    2,
    createMplsAction(thrift::MplsActionCode::SWAP, 2));
const auto mpls_path1_2_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2,
    createMplsAction(thrift::MplsActionCode::SWAP, 2));

// Check if two lists of unicastRoute's are equal.
// Handles elements being in different order.
bool
checkEqualUnicastRoutes(
    const std::vector<thrift::UnicastRoute>& lhs,
    const std::vector<thrift::UnicastRoute>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  std::unordered_map<folly::CIDRNetwork, std::set<thrift::NextHopThrift>>
      lhsRoutes;
  std::unordered_map<folly::CIDRNetwork, std::set<thrift::NextHopThrift>>
      rhsRoutes;
  for (auto const& route : lhs) {
    lhsRoutes.emplace(
        toIPNetwork(*route.dest_ref()),
        std::set<thrift::NextHopThrift>(
            route.nextHops_ref()->begin(), route.nextHops_ref()->end()));
  }
  for (auto const& route : rhs) {
    rhsRoutes.emplace(
        toIPNetwork(*route.dest_ref()),
        std::set<thrift::NextHopThrift>(
            route.nextHops_ref()->begin(), route.nextHops_ref()->end()));
  }

  for (auto const& kv : lhsRoutes) {
    if (rhsRoutes.count(kv.first) == 0) {
      return false;
    }
    if (rhsRoutes.at(kv.first) != kv.second) {
      return false;
    }
  }

  for (auto const& kv : rhsRoutes) {
    if (lhsRoutes.count(kv.first) == 0) {
      return false;
    }
    if (lhsRoutes.at(kv.first) != kv.second) {
      return false;
    }
  }

  return true;
}

bool
checkEqualRouteDatabaseUnicast(
    const thrift::RouteDatabase& lhs, const thrift::RouteDatabase& rhs) {
  return checkEqualUnicastRoutes(
      *lhs.unicastRoutes_ref(), *rhs.unicastRoutes_ref());
}

// Check if two lists of unicastRouteDetail's are equal.
// Handles elements being in different order.
bool
checkEqualUnicastRoutesDetail(
    const std::vector<thrift::UnicastRouteDetail>& lhs,
    const std::vector<thrift::UnicastRouteDetail>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  std::vector<thrift::UnicastRoute> uLhs;
  std::vector<thrift::UnicastRoute> uRhs;
  for (auto const& route : lhs) {
    uLhs.emplace_back(*route.unicastRoute_ref());

    // Check bestRoute has been filled out properly with correct prefix
    if (*route.dest_ref() != *route.bestRoute_ref()->prefix_ref()) {
      return false;
    }
  }

  for (auto const& route : rhs) {
    uRhs.emplace_back(*route.unicastRoute_ref());

    // Check bestRoute has been filled out properly with correct prefix
    if (*route.dest_ref() != *route.bestRoute_ref()->prefix_ref()) {
      return false;
    }
  }

  return checkEqualUnicastRoutes(uLhs, uRhs);
}

bool
checkEqualRouteDatabaseUnicastDetail(
    const thrift::RouteDatabaseDetail& lhs,
    const thrift::RouteDatabaseDetail& rhs) {
  return checkEqualUnicastRoutesDetail(
      *lhs.unicastRoutes_ref(), *rhs.unicastRoutes_ref());
}

bool
checkEqualRouteDatabaseMpls(
    const thrift::RouteDatabase& lhs, const thrift::RouteDatabase& rhs) {
  if (lhs.mplsRoutes_ref()->size() != rhs.mplsRoutes_ref()->size()) {
    return false;
  }
  std::unordered_map<int32_t, std::set<thrift::NextHopThrift>> lhsRoutes;
  std::unordered_map<int32_t, std::set<thrift::NextHopThrift>> rhsRoutes;
  for (auto const& route : *lhs.mplsRoutes_ref()) {
    lhsRoutes.emplace(
        *route.topLabel_ref(),
        std::set<thrift::NextHopThrift>(
            route.nextHops_ref()->begin(), route.nextHops_ref()->end()));
  }
  for (auto const& route : *rhs.mplsRoutes_ref()) {
    rhsRoutes.emplace(
        *route.topLabel_ref(),
        std::set<thrift::NextHopThrift>(
            route.nextHops_ref()->begin(), route.nextHops_ref()->end()));
  }

  for (auto const& kv : lhsRoutes) {
    if (rhsRoutes.count(kv.first) == 0) {
      return false;
    }
    if (rhsRoutes.at(kv.first) != kv.second) {
      return false;
    }
  }

  for (auto const& kv : rhsRoutes) {
    if (lhsRoutes.count(kv.first) == 0) {
      return false;
    }
    if (lhsRoutes.at(kv.first) != kv.second) {
      return false;
    }
  }
  return true;
}

// Verify if RouteDatabaseDelta are same.
// Handles values being arrange in different order.
bool
checkEqualRouteDatabaseDeltaUnicast(
    const thrift::RouteDatabaseDelta& lhs,
    const thrift::RouteDatabaseDelta& rhs) {
  // Check routes to update
  if (not checkEqualUnicastRoutes(
          *lhs.unicastRoutesToUpdate_ref(), *rhs.unicastRoutesToUpdate_ref())) {
    return false;
  }
  // Check routes to delete
  if ((*lhs.unicastRoutesToDelete_ref()).size() !=
      (*rhs.unicastRoutesToDelete_ref()).size()) {
    return false;
  }

  std::set<thrift::IpPrefix> lhsRoutesToDelete(
      (*lhs.unicastRoutesToDelete_ref()).begin(),
      (*lhs.unicastRoutesToDelete_ref()).end());
  std::set<thrift::IpPrefix> rhsRoutesToDelete(
      (*rhs.unicastRoutesToDelete_ref()).begin(),
      (*rhs.unicastRoutesToDelete_ref()).end());

  return lhsRoutesToDelete == rhsRoutesToDelete;
}

// Verify if RouteDatabaseDeltaDetail are same.
// Handles values being arrange in different order.
bool
checkEqualRouteDatabaseDeltaDetailUnicast(
    const thrift::RouteDatabaseDeltaDetail& lhs,
    const thrift::RouteDatabaseDeltaDetail& rhs) {
  // Check routes to update
  if (not checkEqualUnicastRoutesDetail(
          *lhs.unicastRoutesToUpdate_ref(), *rhs.unicastRoutesToUpdate_ref())) {
    return false;
  }
  // Check routes to delete
  if ((*lhs.unicastRoutesToDelete_ref()).size() !=
      (*rhs.unicastRoutesToDelete_ref()).size()) {
    return false;
  }

  std::set<thrift::IpPrefix> lhsRoutesToDelete(
      (*lhs.unicastRoutesToDelete_ref()).begin(),
      (*lhs.unicastRoutesToDelete_ref()).end());
  std::set<thrift::IpPrefix> rhsRoutesToDelete(
      (*rhs.unicastRoutesToDelete_ref()).begin(),
      (*rhs.unicastRoutesToDelete_ref()).end());

  return lhsRoutesToDelete == rhsRoutesToDelete;
}

bool
checkEqualDecisionRouteUpdate(
    const DecisionRouteUpdate& lhs, const DecisionRouteUpdate& rhs) {
  // Check unicast and MPLS routes are the same (assuming no dups in the
  // vectors). perfEvents is not considered here.

  if (lhs.unicastRoutesToUpdate.size() != rhs.unicastRoutesToUpdate.size() or
      lhs.unicastRoutesToDelete.size() != rhs.unicastRoutesToDelete.size() or
      lhs.mplsRoutesToUpdate.size() != rhs.mplsRoutesToUpdate.size() or
      lhs.mplsRoutesToDelete.size() != rhs.mplsRoutesToDelete.size()) {
    return false;
  }

  if (lhs.unicastRoutesToUpdate != rhs.unicastRoutesToUpdate or
      std::unordered_set(
          lhs.unicastRoutesToDelete.begin(), lhs.unicastRoutesToDelete.end()) !=
          std::unordered_set(
              rhs.unicastRoutesToDelete.begin(),
              rhs.unicastRoutesToDelete.end()) or
      std::unordered_set(
          lhs.mplsRoutesToDelete.begin(), lhs.mplsRoutesToDelete.end()) !=
          std::unordered_set(
              rhs.mplsRoutesToDelete.begin(), rhs.mplsRoutesToDelete.end())) {
    LOG(INFO) << "unicast update/delete, or mpls delete, not the same.";
    return false;
  }
  // check mplsRoutesToUpdate.
  std::unordered_map<uint32_t, RibMplsEntry> lMplsRouteMap;
  std::unordered_map<uint32_t, RibMplsEntry> rMplsRouteMap;
  for (auto& mplsRoute : lhs.mplsRoutesToUpdate) {
    lMplsRouteMap.insert({mplsRoute.label, mplsRoute});
  }
  for (auto& mplsRoute : rhs.mplsRoutesToUpdate) {
    rMplsRouteMap.insert({mplsRoute.label, mplsRoute});
  }
  if (lMplsRouteMap != rMplsRouteMap) {
    return false;
    LOG(INFO) << "mpls route not the same.";
  }
  return lhs.type == rhs.type;
}

class FibTestFixture : public ::testing::Test {
 public:
  explicit FibTestFixture(bool waitOnDecision = false)
      : waitOnDecision_(waitOnDecision) {}
  void
  SetUp() override {
    mockFibHandler_ = std::make_shared<MockNetlinkFibHandler>();

    server = make_shared<ThriftServer>();
    server->setNumIOWorkerThreads(1);
    server->setNumAcceptThreads(1);
    server->setPort(0);
    server->setInterface(mockFibHandler_);

    fibThriftThread.start(server);
    port = fibThriftThread.getAddress()->getPort();

    auto tConfig = getBasicOpenrConfig(
        "node-1",
        "domain",
        {}, /* area config */
        true, /* enableV4 */
        true /*enableSegmentRouting*/,
        false /*orderedFibProgramming*/,
        false /*dryrun*/);
    if (waitOnDecision_) {
      tConfig.eor_time_s_ref() = 1;
    }

    config_ = make_shared<Config>(tConfig);

    fib_ = std::make_shared<Fib>(
        config_,
        port, /* thrift port */
        std::chrono::seconds(2), /* coldStartDuration */
        routeUpdatesQueue.getReader(),
        staticRouteUpdatesQueue.getReader(),
        fibRouteUpdatesQueue,
        logSampleQueue);

    fibThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Fib thread starting";
      fib_->run();
      LOG(INFO) << "Fib thread finishing";
    });
    fib_->waitUntilRunning();

    // instantiate openrCtrlHandler to invoke fib API
    handler_ = std::make_shared<OpenrCtrlHandler>(
        "node-1",
        std::unordered_set<std::string>{} /* acceptable peers */,
        &evb_,
        nullptr /* decision */,
        fib_.get() /* fib */,
        nullptr /* kvStore */,
        nullptr /* linkMonitor */,
        nullptr /* monitor */,
        nullptr /* configStore */,
        nullptr /* prefixManager */,
        nullptr /* spark */,
        config_ /* config */);

    evbThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting ctrlEvb";
      evb_.run();
      LOG(INFO) << "ctrlEvb finished";
    });
    evb_.waitUntilRunning();
  }

  void
  TearDown() override {
    LOG(INFO) << "Closing queues";
    fibRouteUpdatesQueue.close();
    routeUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
    logSampleQueue.close();

    LOG(INFO) << "Stopping openr ctrl handler";
    handler_.reset();
    evb_.stop();
    evb_.waitUntilStopped();
    evbThread_->join();

    // this will be invoked before Fib's d-tor
    LOG(INFO) << "Stopping the Fib thread";
    fib_->stop();
    fibThread_->join();
    fib_.reset();

    // stop mocked nl platform
    mockFibHandler_->stop();
    fibThriftThread.stop();
    LOG(INFO) << "Mock fib platform is stopped";
  }

  thrift::RouteDatabase
  getRouteDb() {
    auto resp = handler_->semifuture_getRouteDb().get();
    EXPECT_TRUE(resp);
    return std::move(*resp);
  }

  thrift::RouteDatabaseDetail
  getRouteDetailDb() {
    auto resp = handler_->semifuture_getRouteDetailDb().get();
    EXPECT_TRUE(resp);
    return std::move(*resp);
  }

  std::vector<thrift::UnicastRoute>
  getUnicastRoutesFiltered(std::unique_ptr<std::vector<std::string>> prefixes) {
    auto resp =
        handler_->semifuture_getUnicastRoutesFiltered(std::move(prefixes))
            .get();
    EXPECT_TRUE(resp);
    return *resp;
  }

  std::vector<thrift::UnicastRoute>
  getUnicastRoutes() {
    auto resp = handler_->semifuture_getUnicastRoutes().get();
    EXPECT_TRUE(resp);
    return *resp;
  }

  std::vector<thrift::MplsRoute>
  getMplsRoutesFiltered(std::unique_ptr<std::vector<int32_t>> labels) {
    auto resp =
        handler_->semifuture_getMplsRoutesFiltered(std::move(labels)).get();
    EXPECT_TRUE(resp);
    return *resp;
  }

  std::vector<thrift::MplsRoute>
  getMplsRoutes() {
    auto resp = handler_->semifuture_getMplsRoutes().get();
    EXPECT_TRUE(resp);
    return *resp;
  }

  // Method to wait for OpenrCtrlHandler fib streaming fiber
  // to consume the initial update.
  void
  waitForInitialUpdate() {
    std::atomic<int> received{0};

    auto responseAndSubscription =
        handler_->semifuture_subscribeAndGetFib().get();

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received](auto&& t) {
              if (not t.hasValue()) {
                return;
              }

              received++;
            });

    EXPECT_EQ(1, handler_->getNumFibPublishers());

    // Check we should receive 1 updates
    while (received < 1) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumFibPublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // Method to wait for OpenrCtrlHandler fib detail streaming fiber
  // to consume the initial update.
  void
  wait_for_initial_detail_update() {
    std::atomic<int> received{0};

    auto responseAndSubscription =
        handler_->semifuture_subscribeAndGetFibDetail().get();

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received](auto&& t) {
              if (not t.hasValue()) {
                return;
              }

              received++;
            });

    EXPECT_EQ(1, handler_->getNumFibDetailPublishers());

    // Check we should receive 1 updates
    while (received < 1) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumFibDetailPublishers() != 0) {
      std::this_thread::yield();
    }
  }

  int port{0};
  std::shared_ptr<ThriftServer> server;
  ScopedServerThread fibThriftThread;

  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  messaging::ReplicateQueue<openr::LogSample> logSampleQueue;
  messaging::RQueue<DecisionRouteUpdate> fibRouteUpdatesQueueReader =
      fibRouteUpdatesQueue.getReader();

  // ctrlEvb for openrCtrlHandler instantiation
  OpenrEventBase evb_;
  std::unique_ptr<std::thread> evbThread_;

  std::shared_ptr<Config> config_;
  std::shared_ptr<Fib> fib_;
  std::unique_ptr<std::thread> fibThread_;

  std::shared_ptr<MockNetlinkFibHandler> mockFibHandler_;
  std::shared_ptr<OpenrCtrlHandler> handler_;
  const int16_t kFibId_{static_cast<int16_t>(thrift::FibClient::OPENR)};

 private:
  bool waitOnDecision_{false};
};

// Fib single streaming client test.
// Case 1: Verify initial full dump is received properly.
// Case 2: Verify doNotInstall route is not published.
// Case 3: Verify delta unicast route addition is published.
// Case 4: Verify delta unicast route deletion is published.
TEST_F(FibTestFixture, fibStreamingSingleSubscriber) {
  std::atomic<int> received{0};

  // Case 1: Verify initial full dump is received properly.
  // Mimic decision publishing RouteDatabase (Full initial dump)
  DecisionRouteUpdate routeUpdate1;
  routeUpdate1.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix1),
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1, path1_2_2}));
  routeUpdatesQueue.push(routeUpdate1);

  // Start the streaming after OpenrCtrlHandler consumes initial route update.
  waitForInitialUpdate();
  auto responseAndSubscription =
      handler_->semifuture_subscribeAndGetFib().get();

  thrift::RouteDatabase routeDbExpected1;
  (*routeDbExpected1.unicastRoutes_ref())
      .emplace_back(createUnicastRoute(prefix1, {path1_2_1, path1_2_2}));
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(
      routeDbExpected1, responseAndSubscription.response));
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate1.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate1, fibRouteUpdatesQueueReader.get().value()));

  // Case 2: Verify doNotInstall route is not published.
  // Mimic decision publishing doNotInstall (incremental)
  // No streaming update is expected from fib
  DecisionRouteUpdate routeUpdate2;
  auto ribUnicastEntry =
      RibUnicastEntry(toIPNetwork(prefix2), {path1_2_1, path1_2_2});
  ribUnicastEntry.doNotInstall = true;
  routeUpdate2.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix2), ribUnicastEntry);

  // Case 3: Verify delta unicast route addition is published.
  // Mimic decision publishing unicast route addition (incremental)
  thrift::RouteDatabaseDelta routeDbExpected3;
  (*routeDbExpected3.unicastRoutesToUpdate_ref())
      .emplace_back(createUnicastRoute(prefix3, {path1_2_1, path1_2_2}));
  DecisionRouteUpdate routeUpdate3;
  routeUpdate3.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix3),
      RibUnicastEntry(toIPNetwork(prefix3), {path1_2_1, path1_2_2}));

  // Case 4: Verify delta unicast route deletion is published.
  thrift::RouteDatabaseDelta routeDbExpected4;
  (*routeDbExpected4.unicastRoutesToDelete_ref()) = {prefix3};
  DecisionRouteUpdate routeUpdate4;
  routeUpdate4.unicastRoutesToDelete = {toIPNetwork(prefix3)};

  auto subscription =
      std::move(responseAndSubscription.stream)
          .toClientStreamUnsafeDoNotUse()
          .subscribeExTry(
              folly::getEventBase(),
              [&received, &routeDbExpected3, &routeDbExpected4](auto&& t) {
                if (not t.hasValue()) {
                  return;
                }

                auto& deltaUpdate = *t;
                if (received == 0) {
                  // NOTE: due to donotinstall logic routeUpdate2 get's
                  // suppressed and we directly receive routeUpdate3
                  // notification
                  EXPECT_TRUE(checkEqualRouteDatabaseDeltaUnicast(
                      routeDbExpected3, deltaUpdate));
                } else if (received == 1) {
                  EXPECT_TRUE(checkEqualRouteDatabaseDeltaUnicast(
                      routeDbExpected4, deltaUpdate));
                } else {
                  // Not expected to reach here.
                  FAIL() << "Unexpected stream update";
                }
                received++;
              });

  EXPECT_EQ(1, handler_->getNumFibPublishers());

  routeUpdatesQueue.push(std::move(routeUpdate2));
  routeUpdatesQueue.push(routeUpdate3);
  routeUpdatesQueue.push(routeUpdate4);
  // routeUpdate2 is not installed thus not sent to fibRouteUpdatesQueue.
  routeUpdate3.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate3, fibRouteUpdatesQueueReader.get().value()));
  routeUpdate4.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate4, fibRouteUpdatesQueueReader.get().value()));

  // Check we should receive 2 updates
  while (received < 2) {
    std::this_thread::yield();
  }

  // Cancel subscription
  subscription.cancel();
  std::move(subscription).detach();

  // Wait until publisher is destroyed
  while (handler_->getNumFibPublishers() != 0) {
    std::this_thread::yield();
  }
}

// Fib multiple streaming client test.
// Case 1: Verify initial full dump is received properly by both the clients.
// Case 2: Verify delta unicast route addition is received by both the clients.
TEST_F(FibTestFixture, fibStreamingTwoSubscribers) {
  std::atomic<int> received_1{0};
  std::atomic<int> received_2{0};

  // Case 1: Verify initial full dump is received properly.
  // Mimic decision publishing RouteDatabase (Full initial dump)
  thrift::RouteDatabase routeDbExpected1;
  (*routeDbExpected1.unicastRoutes_ref())
      .emplace_back(createUnicastRoute(prefix1, {path1_2_1, path1_2_2}));
  DecisionRouteUpdate routeUpdate1;
  routeUpdate1.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix1),
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1, path1_2_2}));
  routeUpdatesQueue.push(routeUpdate1);

  // Start the streaming after OpenrCtrlHandler consumes initial route update.
  waitForInitialUpdate();
  auto responseAndSubscription_1 =
      handler_->semifuture_subscribeAndGetFib().get();
  auto responseAndSubscription_2 =
      handler_->semifuture_subscribeAndGetFib().get();

  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(
      routeDbExpected1, responseAndSubscription_1.response));
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(
      routeDbExpected1, responseAndSubscription_2.response));
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate1.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate1, fibRouteUpdatesQueueReader.get().value()));

  // Case 2: Verify delta unicast route addition is published.
  // Mimic decision publishing unicast route addition (incremental)
  thrift::RouteDatabaseDelta routeDbExpected2;
  (*routeDbExpected2.unicastRoutesToUpdate_ref())
      .emplace_back(createUnicastRoute(prefix3, {path1_2_1, path1_2_2}));
  DecisionRouteUpdate routeUpdate2;
  routeUpdate2.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix3),
      RibUnicastEntry(toIPNetwork(prefix3), {path1_2_1, path1_2_2}));

  auto subscription_1 =
      std::move(responseAndSubscription_1.stream)
          .toClientStreamUnsafeDoNotUse()
          .subscribeExTry(
              folly::getEventBase(),
              [&received_1, &routeDbExpected2](auto&& t) {
                if (not t.hasValue()) {
                  return;
                }
                EXPECT_TRUE(
                    checkEqualRouteDatabaseDeltaUnicast(routeDbExpected2, *t));
                received_1++;
              });

  auto subscription_2 =
      std::move(responseAndSubscription_2.stream)
          .toClientStreamUnsafeDoNotUse()
          .subscribeExTry(
              folly::getEventBase(),
              [&received_2, &routeDbExpected2](auto&& t) {
                if (not t.hasValue()) {
                  return;
                }
                EXPECT_TRUE(
                    checkEqualRouteDatabaseDeltaUnicast(routeDbExpected2, *t));
                received_2++;
              });

  EXPECT_EQ(2, handler_->getNumFibPublishers());

  routeUpdatesQueue.push(routeUpdate2);
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate2.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate2, fibRouteUpdatesQueueReader.get().value()));

  // Check we should receive 1 updates for each client
  while ((received_1 < 1) || (received_2 < 1)) {
    std::this_thread::yield();
  }

  // Cancel subscription
  subscription_1.cancel();
  std::move(subscription_1).detach();
  subscription_2.cancel();
  std::move(subscription_2).detach();

  // Wait until publisher is destroyed
  while (handler_->getNumFibPublishers() != 0) {
    std::this_thread::yield();
  }
}

// Fib single streaming client test.
// Case 1: Verify initial full dump is received properly.
// Case 2: Verify delta unicast route addition is published.
TEST_F(FibTestFixture, fibDetailStreaming) {
  std::atomic<int> received{0};

  // Case 1: Verify initial full dump is received properly.
  // Mimic decision publishing RouteDatabaseDetail (Full initial dump)
  thrift::RouteDatabaseDetail routeDbExpected1;
  (*routeDbExpected1.unicastRoutes_ref())
      .emplace_back(createUnicastRouteDetail(
          prefix1, {path1_2_1, path1_2_2}, bestRoute1));
  DecisionRouteUpdate routeUpdate1;

  routeUpdate1.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix1),
      RibUnicastEntry(
          toIPNetwork(prefix1), {path1_2_1, path1_2_2}, bestRoute1, "0"));
  routeUpdatesQueue.push(routeUpdate1);

  // Start the streaming after OpenrCtrlHandler consumes initial route update.
  wait_for_initial_detail_update();
  auto responseAndSubscription =
      handler_->semifuture_subscribeAndGetFibDetail().get();

  EXPECT_TRUE(checkEqualRouteDatabaseUnicastDetail(
      routeDbExpected1, responseAndSubscription.response));
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate1.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate1, fibRouteUpdatesQueueReader.get().value()));

  // Verify delta unicast route addition is published.
  // Mimic decision publishing unicast route addition (incremental)
  thrift::RouteDatabaseDeltaDetail routeDbExpected2;
  (*routeDbExpected2.unicastRoutesToUpdate_ref())
      .emplace_back(createUnicastRouteDetail(
          prefix3, {path1_2_1, path1_2_2}, bestRoute3));
  DecisionRouteUpdate routeUpdate2;
  routeUpdate2.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix3),
      RibUnicastEntry(
          toIPNetwork(prefix3), {path1_2_1, path1_2_2}, bestRoute3, "0"));

  auto subscription =
      std::move(responseAndSubscription.stream)
          .toClientStreamUnsafeDoNotUse()
          .subscribeExTry(
              folly::getEventBase(), [&received, &routeDbExpected2](auto&& t) {
                if (not t.hasValue()) {
                  return;
                }

                auto& deltaUpdate = *t;
                if (received == 0) {
                  EXPECT_TRUE(checkEqualRouteDatabaseDeltaDetailUnicast(
                      routeDbExpected2, deltaUpdate));
                } else {
                  // Not expected to reach here.
                  FAIL() << "Unexpected stream update";
                }
                received++;
              });

  EXPECT_EQ(1, handler_->getNumFibDetailPublishers());

  routeUpdatesQueue.push(routeUpdate2);
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate2.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate2, fibRouteUpdatesQueueReader.get().value()));

  // Check we should received 1 update
  while (received < 1) {
    std::this_thread::yield();
  }

  // Cancel subscription
  subscription.cancel();
  std::move(subscription).detach();

  // Wait until publisher is destroyed
  while (handler_->getNumFibDetailPublishers() != 0) {
    std::this_thread::yield();
  }
}

TEST_F(FibTestFixture, processRouteDb) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // initial syncFib debounce
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->waitForSyncMplsFib();
  // Empty routes are sent fi tobRouteUpdatesQueue_.
  DecisionRouteUpdate emptyUpdate;
  emptyUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      emptyUpdate, fibRouteUpdatesQueueReader.get().value()));

  // Mimic decision pub sock publishing RouteDatabaseDelta and
  // RouteDatabaseDeltaDetail
  thrift::RouteDatabase routeDb;
  *routeDb.thisNodeName_ref() = "node-1";
  routeDb.unicastRoutes_ref()->emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2}));
  thrift::RouteDatabaseDetail routeDetailDb;
  *routeDetailDb.thisNodeName_ref() = "node-1";
  routeDetailDb.unicastRoutes_ref()->emplace_back(
      createUnicastRouteDetail(prefix2, {path1_2_1, path1_2_2}, bestRoute2));

  DecisionRouteUpdate routeUpdate1;
  routeUpdate1.addRouteToUpdate(RibUnicastEntry(
      toIPNetwork(prefix2), {path1_2_1, path1_2_2}, bestRoute2, "0"));
  routeUpdatesQueue.push(routeUpdate1);

  int64_t countAdd = mockFibHandler_->getAddRoutesCount();
  // add routes
  mockFibHandler_->waitForUpdateUnicastRoutes();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate1.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate1, fibRouteUpdatesQueueReader.get().value()));

  EXPECT_EQ(mockFibHandler_->getAddRoutesCount(), 1);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), 0);

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(routeDb, getRouteDb()));
  EXPECT_TRUE(
      checkEqualRouteDatabaseUnicastDetail(routeDetailDb, getRouteDetailDb()));

  // Update routes
  countAdd = mockFibHandler_->getAddRoutesCount();
  int64_t countDel = mockFibHandler_->getDelRoutesCount();
  routeDb.unicastRoutes_ref()->emplace_back(
      RibUnicastEntry(toIPNetwork(prefix3), {path1_3_1, path1_3_2}).toThrift());
  routeDetailDb.unicastRoutes_ref()->emplace_back(
      RibUnicastEntry(
          toIPNetwork(prefix3), {path1_3_1, path1_3_2}, bestRoute3, "0")
          .toThriftDetail());

  DecisionRouteUpdate routeUpdate2;
  routeUpdate2.addRouteToUpdate(RibUnicastEntry(
      toIPNetwork(prefix3), {path1_3_1, path1_3_2}, bestRoute3, "0"));
  routeUpdatesQueue.push(routeUpdate2);

  // syncFib debounce
  mockFibHandler_->waitForUpdateUnicastRoutes();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate2.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate2, fibRouteUpdatesQueueReader.get().value()));

  EXPECT_GT(mockFibHandler_->getAddRoutesCount(), countAdd);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), countDel);
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(routeDb, getRouteDb()));
  EXPECT_TRUE(
      checkEqualRouteDatabaseUnicastDetail(routeDetailDb, getRouteDetailDb()));

  // Update routes by removing some nextHop
  countAdd = mockFibHandler_->getAddRoutesCount();
  routeDb.unicastRoutes_ref()->clear();
  routeDb.unicastRoutes_ref()->emplace_back(
      createUnicastRoute(prefix2, {path1_2_2, path1_2_3}));
  routeDb.unicastRoutes_ref()->emplace_back(
      createUnicastRoute(prefix3, {path1_3_2}));
  routeDetailDb.unicastRoutes_ref()->clear();
  routeDetailDb.unicastRoutes_ref()->emplace_back(
      createUnicastRouteDetail(prefix2, {path1_2_2, path1_2_3}, bestRoute2));
  routeDetailDb.unicastRoutes_ref()->emplace_back(
      createUnicastRouteDetail(prefix3, {path1_3_2}, bestRoute3));

  DecisionRouteUpdate routeUpdate3;
  routeUpdate3.addRouteToUpdate(RibUnicastEntry(
      toIPNetwork(prefix2), {path1_2_2, path1_2_3}, bestRoute2, "0"));
  routeUpdate3.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix3), {path1_3_2}, bestRoute3, "0"));
  routeUpdatesQueue.push(routeUpdate3);
  // syncFib debounce
  mockFibHandler_->waitForUpdateUnicastRoutes();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate3.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate3, fibRouteUpdatesQueueReader.get().value()));

  EXPECT_GT(mockFibHandler_->getAddRoutesCount(), countAdd);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), countDel);
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(routeDb, getRouteDb()));
  EXPECT_TRUE(
      checkEqualRouteDatabaseUnicastDetail(routeDetailDb, getRouteDetailDb()));
}

TEST_F(FibTestFixture, basicAddAndDelete) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // initial syncFib debounce
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->waitForSyncMplsFib();
  // Empty routes are sent fi tobRouteUpdatesQueue_.
  DecisionRouteUpdate emptyUpdate;
  emptyUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      emptyUpdate, fibRouteUpdatesQueueReader.get().value()));

  // Mimic decision pub sock publishing RouteDatabaseDelta
  DecisionRouteUpdate routeUpdate1;
  routeUpdate1.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1, path1_2_2}));
  routeUpdate1.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix3), {path1_3_1, path1_3_2}));
  routeUpdate1.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label1, {mpls_path1_2_1, mpls_path1_2_2}));
  routeUpdate1.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label2, {mpls_path1_2_2}));
  routeUpdate1.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label3, {mpls_path1_2_1}));
  routeUpdatesQueue.push(routeUpdate1);

  // wait
  mockFibHandler_->waitForUpdateUnicastRoutes();
  mockFibHandler_->waitForUpdateMplsRoutes();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate1.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate1, fibRouteUpdatesQueueReader.get().value()));

  // verify routes
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  EXPECT_EQ(mockFibHandler_->getAddRoutesCount(), 2);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), 0);

  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 3);
  EXPECT_EQ(mockFibHandler_->getAddMplsRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler_->getDelMplsRoutesCount(), 0);

  // delete one route
  DecisionRouteUpdate routeUpdate2;
  routeUpdate2.unicastRoutesToDelete = {toIPNetwork(prefix3)};
  routeUpdate2.mplsRoutesToDelete = {label1, label3};
  routeUpdatesQueue.push(routeUpdate2);

  mockFibHandler_->waitForDeleteUnicastRoutes();
  mockFibHandler_->waitForDeleteMplsRoutes();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate2.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate2, fibRouteUpdatesQueueReader.get().value()));

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  EXPECT_EQ(*routes.at(0).dest_ref(), prefix1);
  EXPECT_EQ(mockFibHandler_->getAddRoutesCount(), 2);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), 1);

  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 1);
  EXPECT_EQ(*mplsRoutes.at(0).topLabel_ref(), label2);
  EXPECT_EQ(mockFibHandler_->getAddMplsRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler_->getDelMplsRoutesCount(), 2);

  // add back that route
  DecisionRouteUpdate routeUpdate3;
  routeUpdate3.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix3), {path1_3_1, path1_3_2}));
  routeUpdate3.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label1, {mpls_path1_2_1, mpls_path1_2_2}));
  routeUpdatesQueue.push(routeUpdate3);

  mockFibHandler_->waitForUpdateUnicastRoutes();
  mockFibHandler_->waitForUpdateMplsRoutes();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate3.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate3, fibRouteUpdatesQueueReader.get().value()));

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  EXPECT_EQ(mockFibHandler_->getAddRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), 1);

  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);
  EXPECT_EQ(mockFibHandler_->getAddMplsRoutesCount(), 4);
  EXPECT_EQ(mockFibHandler_->getDelMplsRoutesCount(), 2);
}

TEST_F(FibTestFixture, fibRestart) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // Mimic decision pub sock publishing RouteDatabaseDelta
  DecisionRouteUpdate routeUpdate;
  routeUpdate.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1, path1_2_2}));
  routeUpdate.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label1, {mpls_path1_2_1, mpls_path1_2_2}));
  routeUpdate.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label2, {mpls_path1_2_2}));

  routeUpdatesQueue.push(routeUpdate);

  // initial syncFib debounce
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->waitForSyncMplsFib();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate, fibRouteUpdatesQueueReader.get().value()));

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);

  // Restart
  mockFibHandler_->restart();

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // syncFib debounce
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->waitForSyncMplsFib();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate.type = DecisionRouteUpdate::FULL_SYNC_AFTER_FIB_FAILURES;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate, fibRouteUpdatesQueueReader.get().value()));

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);
}

class FibTestFixtureWaitOnDecision : public FibTestFixture {
 public:
  FibTestFixtureWaitOnDecision() : FibTestFixture(true) {}
};

TEST_F(FibTestFixtureWaitOnDecision, WaitOnDecision) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // Mimic decision pub sock publishing RouteDatabaseDelta
  DecisionRouteUpdate routeUpdate;
  routeUpdate.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1, path1_2_2}));
  routeUpdate.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label1, {mpls_path1_2_1, mpls_path1_2_2}));
  routeUpdate.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label2, {mpls_path1_2_2}));

  routeUpdatesQueue.push(routeUpdate);

  // initial syncFib debounce
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->waitForSyncMplsFib();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate, fibRouteUpdatesQueueReader.get().value()));

  // ensure no other calls occured
  EXPECT_EQ(mockFibHandler_->getFibSyncCount(), 1);
  EXPECT_EQ(mockFibHandler_->getAddRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), 0);

  EXPECT_EQ(mockFibHandler_->getFibMplsSyncCount(), 1);
  EXPECT_EQ(mockFibHandler_->getAddMplsRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler_->getDelMplsRoutesCount(), 0);
}

TEST_F(FibTestFixture, getMslpRoutesFilteredTest) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // initial syncFib debounce
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->waitForSyncMplsFib();
  // Empty routes are sent fi tobRouteUpdatesQueue_.
  DecisionRouteUpdate emptyUpdate;
  emptyUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      emptyUpdate, fibRouteUpdatesQueueReader.get().value()));

  // Mimic decision pub sock publishing RouteDatabaseDelta
  auto route1 = RibMplsEntry(label1, {mpls_path1_2_1, mpls_path1_2_2});
  auto route2 = RibMplsEntry(label2, {mpls_path1_2_2});
  auto route3 = RibMplsEntry(label3, {mpls_path1_2_1});
  const auto& tRoute1 = route1.toThrift();
  const auto& tRoute2 = route2.toThrift();
  const auto& tRoute3 = route3.toThrift();

  DecisionRouteUpdate routeUpdate;
  routeUpdate.mplsRoutesToUpdate.emplace_back(std::move(route1));
  routeUpdate.mplsRoutesToUpdate.emplace_back(std::move(route2));
  routeUpdate.mplsRoutesToUpdate.emplace_back(std::move(route3));
  routeUpdatesQueue.push(routeUpdate);

  // wait for mpls
  mockFibHandler_->waitForUpdateMplsRoutes();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate, fibRouteUpdatesQueueReader.get().value()));

  // verify mpls routes in DB
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 3);
  EXPECT_EQ(mockFibHandler_->getAddMplsRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler_->getDelMplsRoutesCount(), 0);

  // 1. check the MPLS filtering API
  auto labels = std::unique_ptr<std::vector<int32_t>>(
      new std::vector<int32_t>({1, 1, 3})); // matching route1 and route3
  thrift::RouteDatabase responseDb;
  const auto& filteredRoutes = getMplsRoutesFiltered(std::move(labels));
  *responseDb.mplsRoutes_ref() = filteredRoutes;
  // expected routesDB after filtering - delete duplicate entries
  thrift::RouteDatabase expectedDb;
  *expectedDb.thisNodeName_ref() = "node-1";
  *expectedDb.mplsRoutes_ref() = {tRoute1, tRoute3};
  EXPECT_TRUE(checkEqualRouteDatabaseMpls(responseDb, expectedDb));

  // 2. check getting all MPLS routes API
  thrift::RouteDatabase allRoutesDb;
  *allRoutesDb.mplsRoutes_ref() = getMplsRoutes();
  // expected routesDB for all MPLS Routes
  thrift::RouteDatabase allRoutesExpectedDb;
  *allRoutesExpectedDb.thisNodeName_ref() = "node-1";
  *allRoutesExpectedDb.mplsRoutes_ref() = {tRoute1, tRoute2, tRoute3};
  *expectedDb.mplsRoutes_ref() = {tRoute1, tRoute2, tRoute3};
  EXPECT_TRUE(checkEqualRouteDatabaseMpls(allRoutesDb, allRoutesExpectedDb));

  // 3. check filtering API with empty input list - return all MPLS routes
  auto emptyLabels =
      std::unique_ptr<std::vector<int32_t>>(new std::vector<int32_t>({}));
  thrift::RouteDatabase responseAllDb;
  *responseAllDb.mplsRoutes_ref() =
      getMplsRoutesFiltered(std::move(emptyLabels));
  EXPECT_TRUE(checkEqualRouteDatabaseMpls(responseAllDb, allRoutesExpectedDb));

  // 4. check if no result found
  auto notFoundFilter = std::unique_ptr<std::vector<std::int32_t>>(
      new std::vector<std::int32_t>({4, 5}));
  const auto& notFoundResp = getMplsRoutesFiltered(std::move(notFoundFilter));
  EXPECT_EQ(notFoundResp.size(), 0);
}

TEST_F(FibTestFixture, getUnicastRoutesFilteredTest) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  // initial syncFib debounce
  mockFibHandler_->waitForSyncFib();
  // Empty routes are sent fi tobRouteUpdatesQueue_.
  DecisionRouteUpdate emptyUpdate;
  emptyUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      emptyUpdate, fibRouteUpdatesQueueReader.get().value()));

  const auto prefix1 = toIpPrefix("192.168.20.16/28");
  const auto prefix2 = toIpPrefix("192.168.0.0/16");
  const auto prefix3 = toIpPrefix("fd00::48:2:0/128");
  const auto prefix4 = toIpPrefix("fd00::48:2:0/126");

  auto route1 = RibUnicastEntry(toIPNetwork(prefix1), {});
  auto route2 = RibUnicastEntry(toIPNetwork(prefix2), {});
  auto route3 = RibUnicastEntry(toIPNetwork(prefix3), {});
  auto route4 = RibUnicastEntry(toIPNetwork(prefix4), {});

  const auto& tRoute1 = route1.toThrift();
  const auto& tRoute2 = route2.toThrift();
  const auto& tRoute3 = route3.toThrift();
  const auto& tRoute4 = route4.toThrift();

  // add routes to DB and update DB
  DecisionRouteUpdate routeUpdate;
  routeUpdate.addRouteToUpdate(std::move(route1));
  routeUpdate.addRouteToUpdate(std::move(route2));
  routeUpdate.addRouteToUpdate(std::move(route3));
  routeUpdate.addRouteToUpdate(std::move(route4));
  routeUpdatesQueue.push(routeUpdate);
  mockFibHandler_->waitForUpdateUnicastRoutes();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate, fibRouteUpdatesQueueReader.get().value()));

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 4);

  // input filter prefix list
  auto filter =
      std::unique_ptr<std::vector<std::string>>(new std::vector<std::string>({
          "192.168.20.16/28", // match prefix1
          "192.168.20.19", // match prefix1
          "192.168.0.0", // match prefix2
          "192.168.0.0/18", // match prefix2
          "10.46.8.0", // no match
          "fd00::48:2:0/127", // match prefix4
          "fd00::48:2:0/125" // no match
      }));

  // expected routesDB after filtering - delete duplicate entries
  thrift::RouteDatabase expectedDb;
  *expectedDb.thisNodeName_ref() = "node-1";
  expectedDb.unicastRoutes_ref()->emplace_back(tRoute1);
  expectedDb.unicastRoutes_ref()->emplace_back(tRoute2);
  expectedDb.unicastRoutes_ref()->emplace_back(tRoute4);
  // check if match correctly
  thrift::RouteDatabase responseDb;
  const auto& responseRoutes = getUnicastRoutesFiltered(std::move(filter));
  *responseDb.unicastRoutes_ref() = responseRoutes;
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(expectedDb, responseDb));

  // check when get empty input - return all unicast routes
  thrift::RouteDatabase allRouteDb;
  allRouteDb.unicastRoutes_ref()->emplace_back(tRoute1);
  allRouteDb.unicastRoutes_ref()->emplace_back(tRoute2);
  allRouteDb.unicastRoutes_ref()->emplace_back(tRoute3);
  allRouteDb.unicastRoutes_ref()->emplace_back(tRoute4);
  auto emptyParamRet =
      std::unique_ptr<std::vector<std::string>>(new std::vector<std::string>());
  const auto& allRoutes = getUnicastRoutesFiltered(std::move(emptyParamRet));
  thrift::RouteDatabase allRoutesRespDb;
  *allRoutesRespDb.unicastRoutes_ref() = allRoutes;
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(allRouteDb, allRoutesRespDb));

  // check getUnicastRoutes() API - return all unicast routes
  const auto& allRoute = getUnicastRoutes();
  thrift::RouteDatabase allRoutesApiDb;
  *allRoutesApiDb.unicastRoutes_ref() = allRoute;
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(allRouteDb, allRoutesApiDb));

  // check when no result found
  auto notFoundFilter = std::unique_ptr<std::vector<std::string>>(
      new std::vector<std::string>({"10.46.8.0", "10.46.8.0/24"}));
  const auto& notFoundResp =
      getUnicastRoutesFiltered(std::move(notFoundFilter));
  EXPECT_EQ(notFoundResp.size(), 0);
}

TEST_F(FibTestFixture, longestPrefixMatchTest) {
  std::unordered_map<folly::CIDRNetwork, RibUnicastEntry> unicastRoutes;
  const auto& defaultRoute = toIpPrefix("::/0");
  const auto& dbPrefix1 = toIpPrefix("192.168.0.0/16");
  const auto& dbPrefix2 = toIpPrefix("192.168.0.0/20");
  const auto& dbPrefix3 = toIpPrefix("192.168.0.0/24");
  const auto& dbPrefix4 = toIpPrefix("192.168.20.16/28");

  const auto defaultRouteCidr = toIPNetwork(defaultRoute);
  const auto dbPrefix1Cidr = toIPNetwork(dbPrefix1);
  const auto dbPrefix2Cidr = toIPNetwork(dbPrefix2);
  const auto dbPrefix3Cidr = toIPNetwork(dbPrefix3);
  const auto dbPrefix4Cidr = toIPNetwork(dbPrefix4);

  unicastRoutes.emplace(
      defaultRouteCidr, RibUnicastEntry(defaultRouteCidr, {}));
  unicastRoutes.emplace(dbPrefix1Cidr, RibUnicastEntry(dbPrefix1Cidr, {}));
  unicastRoutes.emplace(dbPrefix2Cidr, RibUnicastEntry(dbPrefix2Cidr, {}));
  unicastRoutes.emplace(dbPrefix3Cidr, RibUnicastEntry(dbPrefix3Cidr, {}));
  unicastRoutes.emplace(dbPrefix4Cidr, RibUnicastEntry(dbPrefix4Cidr, {}));

  const auto inputdefaultRoute =
      folly::IPAddress::tryCreateNetwork("::/0").value();
  const auto inputPrefix1 =
      folly::IPAddress::tryCreateNetwork("192.168.20.19").value();
  const auto inputPrefix2 =
      folly::IPAddress::tryCreateNetwork("192.168.20.16/28").value();
  const auto inputPrefix3 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0").value();
  const auto inputPrefix4 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0/14").value();
  const auto inputPrefix5 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0/18").value();
  const auto inputPrefix6 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0/22").value();
  const auto inputPrefix7 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0/26").value();

  // default route matching
  const auto& result =
      Fib::longestPrefixMatch(inputdefaultRoute, unicastRoutes);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), defaultRouteCidr);

  // input 192.168.20.19 matched 192.168.20.16/28
  const auto& result1 = Fib::longestPrefixMatch(inputPrefix1, unicastRoutes);
  EXPECT_TRUE(result1.has_value());
  EXPECT_EQ(result1.value(), dbPrefix4Cidr);

  // input 192.168.20.16/28 matched 192.168.20.16/28
  const auto& result2 = Fib::longestPrefixMatch(inputPrefix2, unicastRoutes);
  EXPECT_TRUE(result2.has_value());
  EXPECT_EQ(result2.value(), dbPrefix4Cidr);

  // input 192.168.0.0 matched 192.168.0.0/24
  const auto& result3 = Fib::longestPrefixMatch(inputPrefix3, unicastRoutes);
  EXPECT_TRUE(result3.has_value());
  EXPECT_EQ(result3.value(), dbPrefix3Cidr);

  //
  // input 192.168.0.0/14 has no match
  const auto& result4 = Fib::longestPrefixMatch(inputPrefix4, unicastRoutes);
  EXPECT_TRUE(not result4.has_value());

  // input 192.168.0.0/18 matched 192.168.0.0/16
  const auto& result5 = Fib::longestPrefixMatch(inputPrefix5, unicastRoutes);
  EXPECT_TRUE(result5.has_value());
  EXPECT_EQ(result5.value(), dbPrefix1Cidr);

  // input 192.168.0.0/22 matched 192.168.0.0/20
  const auto& result6 = Fib::longestPrefixMatch(inputPrefix6, unicastRoutes);
  EXPECT_TRUE(result6.has_value());
  EXPECT_EQ(result6.value(), dbPrefix2Cidr);

  // input 192.168.0.0/26 matched 192.168.0.0/24
  const auto& result7 = Fib::longestPrefixMatch(inputPrefix7, unicastRoutes);
  EXPECT_TRUE(result7.has_value());
  EXPECT_EQ(result7.value(), dbPrefix3Cidr);
}

TEST_F(FibTestFixture, doNotInstall) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  const auto prefix1 = toIpPrefix("192.168.20.16/28");
  const auto prefix2 = toIpPrefix("192.168.0.0/16");
  const auto prefix3 = toIpPrefix("fd00::48:2:0/128");
  const auto prefix4 = toIpPrefix("fd00::48:2:0/126");

  auto route1 = RibUnicastEntry(toIPNetwork(prefix1), {});
  auto route2 = RibUnicastEntry(toIPNetwork(prefix2), {});
  auto route3 = RibUnicastEntry(toIPNetwork(prefix3), {});
  auto route4 = RibUnicastEntry(toIPNetwork(prefix4), {});

  route1.doNotInstall = true;
  route3.doNotInstall = true;

  // add routes to DB and update DB
  DecisionRouteUpdate routeUpdate1;
  routeUpdate1.addRouteToUpdate(std::move(route1));
  routeUpdate1.addRouteToUpdate(route2);
  routeUpdatesQueue.push(std::move(routeUpdate1));

  mockFibHandler_->waitForSyncFib();

  DecisionRouteUpdate programmedRouteUpdate1;
  programmedRouteUpdate1.addRouteToUpdate(route2);
  programmedRouteUpdate1.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      programmedRouteUpdate1, fibRouteUpdatesQueueReader.get().value()));

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  // only 1 route is installable
  EXPECT_EQ(routes.size(), 1);

  // add routes to DB and update DB
  DecisionRouteUpdate routeUpdate2;
  routeUpdate2.addRouteToUpdate(std::move(route3));
  routeUpdate2.addRouteToUpdate(route4);
  routeUpdatesQueue.push(std::move(routeUpdate2));

  mockFibHandler_->waitForUpdateUnicastRoutes();

  DecisionRouteUpdate programmedRouteUpdate2;
  programmedRouteUpdate2.addRouteToUpdate(route4);
  programmedRouteUpdate2.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      programmedRouteUpdate2, fibRouteUpdatesQueueReader.get().value()));

  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  // now 2 routes are installable
  EXPECT_EQ(routes.size(), 2);
}

/**
 * Ensure FIB processes static routes with following in-variant
 * - Only MPLS route Add/Update are processed. All others are ignored
 * - Static routes are only processed before first RIB instance
 * - Fiber terminates after receipt of first RIB instance
 */
TEST_F(FibTestFixtureWaitOnDecision, StaticRouteUpdates) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // Send the static route update (with all types of updates)
  DecisionRouteUpdate routeUpdate;
  routeUpdate.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1}));
  routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(prefix2));
  auto mplsEntry = RibMplsEntry(label1, {mpls_path1_2_1});
  routeUpdate.mplsRoutesToUpdate.emplace_back(mplsEntry);
  routeUpdate.mplsRoutesToDelete.emplace_back(label2);
  staticRouteUpdatesQueue.push(routeUpdate);

  // Wait for MPLS route updates
  mockFibHandler_->waitForUpdateMplsRoutes();
  DecisionRouteUpdate syncedUpdate;
  syncedUpdate.type = DecisionRouteUpdate::INCREMENTAL;
  // Only updated MPLS routes are synced.
  syncedUpdate.mplsRoutesToUpdate.emplace_back(mplsEntry);
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      syncedUpdate, fibRouteUpdatesQueueReader.get().value()));

  // Verify counters
  EXPECT_EQ(mockFibHandler_->getFibSyncCount(), 0);
  EXPECT_EQ(mockFibHandler_->getAddRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler_->getFibMplsSyncCount(), 0);
  EXPECT_EQ(mockFibHandler_->getAddMplsRoutesCount(), 1);
  EXPECT_EQ(mockFibHandler_->getDelMplsRoutesCount(), 0);

  // Verify routes
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 1);

  // Verify that MPLS route exists in cache
  EXPECT_EQ(0, fib_->getUnicastRoutes({}).get()->size());
  EXPECT_EQ(1, fib_->getMplsRoutes({}).get()->size());

  // Mimic decision pub sock publishing RouteDatabaseDelta (empty DB)
  DecisionRouteUpdate routeUpdateEmpty;
  routeUpdatesQueue.push(std::move(routeUpdateEmpty));

  // Expect FIB sync for unicast & mpls routes
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->waitForSyncMplsFib();

  // ensure no other calls occured
  EXPECT_EQ(mockFibHandler_->getFibSyncCount(), 1);
  EXPECT_EQ(mockFibHandler_->getAddRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler_->getFibMplsSyncCount(), 1);
  EXPECT_EQ(mockFibHandler_->getAddMplsRoutesCount(), 1);
  EXPECT_EQ(mockFibHandler_->getDelMplsRoutesCount(), 0);

  // Verify routes
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  // MPLS route cached in routeState_ is overridden by Decision update
  EXPECT_EQ(mplsRoutes.size(), 0);

  // Expect fiber for processing static route to be active
  EXPECT_EQ(1, staticRouteUpdatesQueue.getNumReaders());

  // Publish same event and we can expect it to terminate
  staticRouteUpdatesQueue.push(routeUpdate);
  while (staticRouteUpdatesQueue.getNumReaders()) {
    std::this_thread::yield();
  }
}

/**
 * Validates static MPLS routes programming is retried till successful in case
 * of Fib socket failures.
 * 1. There are initially 1 unicast and 1 MPLS routes in FIB client;
 * 2. FIB socket turns unhealthy;
 * 3. Static MPLS routes program failed but backed up;
 * 4. FIB socket flips to healthy;
 * 5. Backed MPLS routes are programmed successfully, while existing routes are
 *    still kept.
 */
TEST_F(
    FibTestFixtureWaitOnDecision, StaticMplsRoutesUpdateRetryTillSuccessful) {
  // 1. Initially there are 1 unicast and 1 MPLS routes in FIB agent.
  DecisionRouteUpdate initialRoutes;
  initialRoutes.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1}));
  initialRoutes.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label1, {mpls_path1_2_1}));
  auto initialRouteDb = initialRoutes.toThrift();
  mockFibHandler_->addUnicastRoutes(
      kFibId_,
      std::make_unique<std::vector<::openr::thrift::UnicastRoute>>(
          std::move(*initialRouteDb.unicastRoutesToUpdate_ref())));
  mockFibHandler_->addMplsRoutes(
      kFibId_,
      std::make_unique<std::vector<::openr::thrift::MplsRoute>>(
          std::move(*initialRouteDb.mplsRoutesToUpdate_ref())));
  mockFibHandler_->waitForUpdateUnicastRoutes();
  mockFibHandler_->waitForUpdateMplsRoutes();

  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 1);

  // 2. FIB client is set as unhealthy.
  mockFibHandler_->setHandlerHealthyState(false /*isHealthy*/);

  // 3. Add static MPLS routes into the queue.
  DecisionRouteUpdate staticRouteUpdate;
  staticRouteUpdate.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label2, {mpls_path1_2_2}));
  staticRouteUpdate.mplsRoutesToDelete.emplace_back(label3);
  staticRouteUpdatesQueue.push(staticRouteUpdate);

  // Static routes are not programmed.
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 1);

  // 4. FIB client flips to healthy.
  mockFibHandler_->setHandlerHealthyState(true /*isHealthy*/);

  // 5. Expect static routes to be prorammed successfully.
  mockFibHandler_->waitForUpdateMplsRoutes();
  DecisionRouteUpdate syncedUpdate;
  // Synced routes are sent to fibRouteUpdatesQueue_.
  staticRouteUpdate.type = DecisionRouteUpdate::INCREMENTAL;
  staticRouteUpdate.mplsRoutesToDelete.clear();
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      staticRouteUpdate, fibRouteUpdatesQueueReader.get().value()));

  // Above backlogged static routes are programmed.
  // Existing routes in FIB client still exist.
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);
}

/**
 * Validates RIB routes programming is retried till successful in case of Fib
 * socket failures.
 * 1. FIB client turns unhealthy;
 * 2. RIB routes program failed but backed up;
 * 3. FIB client flips to healthy;
 * 4. Backed RIB routes are programmed successfully, while existing routes are
 *    kept.
 * 5. Followup RIB instance is successful.
 */
TEST_F(FibTestFixtureWaitOnDecision, RibRoutesUpdateRetryTillSuccessful) {
  // 1. FIB client is initially unhealthy.
  mockFibHandler_->setHandlerHealthyState(false /*isHealthy*/);

  // 2. RIB routes program failed but backed up;
  DecisionRouteUpdate routeUpdate;
  routeUpdate.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1}));
  routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(prefix2));
  routeUpdate.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label1, {mpls_path1_2_1}));
  routeUpdatesQueue.push(routeUpdate);

  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  // RIB routes are not programmed.
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // Make sure there is no update in FIB updates queue
  EXPECT_EQ(0, fibRouteUpdatesQueueReader.size());

  // 3. FIB client flips to healthy.
  mockFibHandler_->setHandlerHealthyState(true /*isHealthy*/);

  // 4. Expect FIB sync for unicast & mpls routes
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->waitForSyncMplsFib();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  routeUpdate.unicastRoutesToDelete.clear();
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate, fibRouteUpdatesQueueReader.get().value()));

  // Above backlogged RIB routes are programmed.
  // Existing routes in FIB client still exist.
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 1);

  // 5. Send another normal RIB route updates.
  DecisionRouteUpdate routeUpdate2;
  routeUpdate2.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix2), {path1_2_2}));
  routeUpdate2.mplsRoutesToUpdate.emplace_back(
      RibMplsEntry(label2, {mpls_path1_2_1}));
  routeUpdate2.mplsRoutesToDelete.emplace_back(label3);
  routeUpdatesQueue.push(routeUpdate2);

  // Wait for route updates
  mockFibHandler_->waitForUpdateUnicastRoutes();
  mockFibHandler_->waitForUpdateMplsRoutes();
  mockFibHandler_->waitForDeleteMplsRoutes();

  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate2.type = DecisionRouteUpdate::INCREMENTAL;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate2, fibRouteUpdatesQueueReader.get().value()));

  // Above normal RIB instance is programmed.
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  mockFibHandler_->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  testing::InitGoogleMock(&argc, argv);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  auto rc = RUN_ALL_TESTS();

  // Run the tests
  return rc;
}
