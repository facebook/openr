/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/tests/mocks/MockNetlinkFibHandler.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

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
        toIPNetwork(*route.dest()),
        std::set<thrift::NextHopThrift>(
            route.nextHops()->begin(), route.nextHops()->end()));
  }
  for (auto const& route : rhs) {
    rhsRoutes.emplace(
        toIPNetwork(*route.dest()),
        std::set<thrift::NextHopThrift>(
            route.nextHops()->begin(), route.nextHops()->end()));
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
  return checkEqualUnicastRoutes(*lhs.unicastRoutes(), *rhs.unicastRoutes());
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
    uLhs.emplace_back(*route.unicastRoute());

    // Check bestRoute has been filled out properly with correct prefix
    if (*route.dest() != *route.bestRoute()->prefix()) {
      return false;
    }
  }

  for (auto const& route : rhs) {
    uRhs.emplace_back(*route.unicastRoute());

    // Check bestRoute has been filled out properly with correct prefix
    if (*route.dest() != *route.bestRoute()->prefix()) {
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
      *lhs.unicastRoutes(), *rhs.unicastRoutes());
}

// Verify if RouteDatabaseDelta are same.
// Handles values being arrange in different order.
bool
checkEqualRouteDatabaseDeltaUnicast(
    const thrift::RouteDatabaseDelta& lhs,
    const thrift::RouteDatabaseDelta& rhs) {
  // Check routes to update
  if (!checkEqualUnicastRoutes(
          *lhs.unicastRoutesToUpdate(), *rhs.unicastRoutesToUpdate())) {
    return false;
  }
  // Check routes to delete
  if ((*lhs.unicastRoutesToDelete()).size() !=
      (*rhs.unicastRoutesToDelete()).size()) {
    return false;
  }

  std::set<thrift::IpPrefix> lhsRoutesToDelete(
      (*lhs.unicastRoutesToDelete()).begin(),
      (*lhs.unicastRoutesToDelete()).end());
  std::set<thrift::IpPrefix> rhsRoutesToDelete(
      (*rhs.unicastRoutesToDelete()).begin(),
      (*rhs.unicastRoutesToDelete()).end());

  return lhsRoutesToDelete == rhsRoutesToDelete;
}

// Verify if RouteDatabaseDeltaDetail are same.
// Handles values being arrange in different order.
bool
checkEqualRouteDatabaseDeltaDetailUnicast(
    const thrift::RouteDatabaseDeltaDetail& lhs,
    const thrift::RouteDatabaseDeltaDetail& rhs) {
  // Check routes to update
  if (!checkEqualUnicastRoutesDetail(
          *lhs.unicastRoutesToUpdate(), *rhs.unicastRoutesToUpdate())) {
    return false;
  }
  // Check routes to delete
  if ((*lhs.unicastRoutesToDelete()).size() !=
      (*rhs.unicastRoutesToDelete()).size()) {
    return false;
  }

  std::set<thrift::IpPrefix> lhsRoutesToDelete(
      (*lhs.unicastRoutesToDelete()).begin(),
      (*lhs.unicastRoutesToDelete()).end());
  std::set<thrift::IpPrefix> rhsRoutesToDelete(
      (*rhs.unicastRoutesToDelete()).begin(),
      (*rhs.unicastRoutesToDelete()).end());

  return lhsRoutesToDelete == rhsRoutesToDelete;
}

bool
checkEqualDecisionRouteUpdate(
    const DecisionRouteUpdate& lhs, const DecisionRouteUpdate& rhs) {
  // Check unicast and MPLS routes are the same (assuming no dups in the
  // vectors). perfEvents is not considered here.

  if (lhs.unicastRoutesToUpdate.size() != rhs.unicastRoutesToUpdate.size() ||
      lhs.unicastRoutesToDelete.size() != rhs.unicastRoutesToDelete.size()) {
    return false;
  }

  if (lhs.unicastRoutesToUpdate != rhs.unicastRoutesToUpdate ||
      std::unordered_set(
          lhs.unicastRoutesToDelete.begin(), lhs.unicastRoutesToDelete.end()) !=
          std::unordered_set(
              rhs.unicastRoutesToDelete.begin(),
              rhs.unicastRoutesToDelete.end())) {
    LOG(INFO) << "unicast/mpls update/delete not the same.";
    return false;
  }
  return lhs.type == rhs.type;
}

class FibTestFixture : public ::testing::Test {
 public:
  explicit FibTestFixture(int32_t routeDeleteDelayMs = 1000)
      : routeDeleteDelay_(routeDeleteDelayMs) {}
  void
  SetUp() override {
    mockFibHandler_ = std::make_shared<MockNetlinkFibHandler>();

    server = std::make_shared<ThriftServer>();
    server->setNumIOWorkerThreads(1);
    server->setNumAcceptThreads(1);
    server->setPort(0);
    server->setInterface(mockFibHandler_);

    fibThriftThread.start(server);

    auto tConfig = getBasicOpenrConfig(
        "node-1",
        {}, /* area config */
        true, /* enableV4 */
        false /* dryrun */);
    tConfig.route_delete_delay_ms() = routeDeleteDelay_;
    tConfig.fib_port() = fibThriftThread.getAddress()->getPort();

    config_ = std::make_shared<Config>(tConfig);

    fib_ = std::make_shared<Fib>(
        config_, routeUpdatesQueue.getReader(), fibRouteUpdatesQueue);

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
    fibThriftThread.stop();
    mockFibHandler_->stop();
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
              if (!t.hasValue()) {
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
              if (!t.hasValue()) {
                return;
              }

              received++;
            });

    EXPECT_EQ(1, handler_->getNumFibDetailSubscribers());

    // Check we should receive 1 updates
    while (received < 1) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumFibDetailSubscribers() != 0) {
      std::this_thread::yield();
    }
  }

  std::shared_ptr<ThriftServer> server;
  ScopedServerThread fibThriftThread;

  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
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

 private:
  const int32_t routeDeleteDelay_{0};
};

class FibDryRunTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // create mockFibHandler to mimick underneath platform FIB agent
    // to receive route programming request
    mockFibHandler_ = std::make_shared<MockNetlinkFibHandler>();
    auto server = std::make_shared<ThriftServer>();
    server->setNumIOWorkerThreads(1);
    server->setNumAcceptThreads(1);
    server->setPort(0);
    server->setInterface(mockFibHandler_);

    fibThriftThread_.start(server);

    auto tConfig = getBasicOpenrConfig(
        "node-1",
        {}, /* area config */
        true, /* enableV4 */
        true /* dryrun */);
    tConfig.fib_port() = fibThriftThread_.getAddress()->getPort();
    tConfig.enable_clear_fib_state() = true;

    config_ = std::make_shared<Config>(std::move(tConfig));
    fib_ = std::make_shared<Fib>(
        config_, routeUpdatesQueue.getReader(), fibRouteUpdatesQueue);

    fibThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Fib thread starting";
      fib_->run();
      LOG(INFO) << "Fib thread finishing";
    });
    fib_->waitUntilRunning();
  }

  void
  TearDown() override {
    fibRouteUpdatesQueue.close();
    routeUpdatesQueue.close();

    LOG(INFO) << "Stopping the Fib thread";
    fib_->stop();
    fibThread_->join();
    fib_.reset();

    // stop mocked nl platform
    fibThriftThread_.stop();
    mockFibHandler_->stop();
    LOG(INFO) << "Mock fib platform is stopped";
  }

  std::shared_ptr<MockNetlinkFibHandler> mockFibHandler_;
  ScopedServerThread fibThriftThread_;

  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  messaging::RQueue<DecisionRouteUpdate> fibRouteUpdatesQueueReader =
      fibRouteUpdatesQueue.getReader();

  std::shared_ptr<Config> config_;
  std::shared_ptr<Fib> fib_;
  std::unique_ptr<std::thread> fibThread_;
};

TEST_F(FibTestFixture, initialRouteCleanupTest) {
  // reset all fb303 counters
  fb303::fbData->resetAllData();

  // validate initialization counter
  auto counterKey = fmt::format(
      Constants::kInitEventCounterFormat,
      apache::thrift::util::enumNameSafe(
          thrift::InitializationEvent::FIB_SYNCED));
  EXPECT_FALSE(fb303::fbData->hasCounter(counterKey));

  // push update from RIB to trigger initial state transition of FIB
  DecisionRouteUpdate routeUpdate1;
  routeUpdate1.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix1),
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1, path1_2_2}));
  routeUpdatesQueue.push(routeUpdate1);

  // wait for the counter to be published
  while (!fb303::fbData->hasCounter(counterKey)) {
  }

  // waiting for one-time clean up signal under dryrun
  EXPECT_FALSE(fib_->getUnicastRoutesCleared());
}

TEST_F(FibDryRunTestFixture, initialRouteCleanupTest) {
  // reset all fb303 counters
  fb303::fbData->resetAllData();

  // validate initialization counter
  auto counterKey = fmt::format(
      Constants::kInitEventCounterFormat,
      apache::thrift::util::enumNameSafe(
          thrift::InitializationEvent::FIB_SYNCED));
  EXPECT_FALSE(fb303::fbData->hasCounter(counterKey));

  // push update from RIB to trigger initial state transition of FIB
  DecisionRouteUpdate routeUpdate1;
  routeUpdate1.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix1),
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1, path1_2_2}));
  routeUpdatesQueue.push(routeUpdate1);

  // wait for the counter to be published
  while (!fb303::fbData->hasCounter(counterKey)) {
  }

  EXPECT_TRUE(fib_->getUnicastRoutesCleared());
}

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
  (*routeDbExpected1.unicastRoutes())
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
  (*routeDbExpected3.unicastRoutesToUpdate())
      .emplace_back(createUnicastRoute(prefix3, {path1_2_1, path1_2_2}));
  DecisionRouteUpdate routeUpdate3;
  routeUpdate3.unicastRoutesToUpdate.emplace(
      toIPNetwork(prefix3),
      RibUnicastEntry(toIPNetwork(prefix3), {path1_2_1, path1_2_2}));

  // Case 4: Verify delta unicast route deletion is published.
  thrift::RouteDatabaseDelta routeDbExpected4;
  (*routeDbExpected4.unicastRoutesToDelete()) = {prefix3};
  DecisionRouteUpdate routeUpdate4;
  routeUpdate4.unicastRoutesToDelete = {toIPNetwork(prefix3)};

  auto subscription =
      std::move(responseAndSubscription.stream)
          .toClientStreamUnsafeDoNotUse()
          .subscribeExTry(
              folly::getEventBase(),
              [&received, &routeDbExpected3, &routeDbExpected4](auto&& t) {
                if (!t.hasValue()) {
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
  (*routeDbExpected1.unicastRoutes())
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
  (*routeDbExpected2.unicastRoutesToUpdate())
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
                if (!t.hasValue()) {
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
                if (!t.hasValue()) {
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
  (*routeDbExpected1.unicastRoutes())
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
  (*routeDbExpected2.unicastRoutesToUpdate())
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
                if (!t.hasValue()) {
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

  EXPECT_EQ(1, handler_->getNumFibDetailSubscribers());

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
  while (handler_->getNumFibDetailSubscribers() != 0) {
    std::this_thread::yield();
  }
}

TEST_F(FibTestFixture, processRouteDb) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  // initial syncFib debounce
  routeUpdatesQueue.push(DecisionRouteUpdate());
  mockFibHandler_->waitForSyncFib();
  // Empty routes are sent fi tobRouteUpdatesQueue_.
  DecisionRouteUpdate emptyUpdate;
  emptyUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      emptyUpdate, fibRouteUpdatesQueueReader.get().value()));

  // Mimic decision pub sock publishing RouteDatabaseDelta and
  // RouteDatabaseDeltaDetail
  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName() = "node-1";
  routeDb.unicastRoutes()->emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2}));
  thrift::RouteDatabaseDetail routeDetailDb;
  routeDetailDb.thisNodeName() = "node-1";
  routeDetailDb.unicastRoutes()->emplace_back(
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
  routeDb.unicastRoutes()->emplace_back(
      RibUnicastEntry(toIPNetwork(prefix3), {path1_3_1, path1_3_2}).toThrift());
  routeDetailDb.unicastRoutes()->emplace_back(
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
  routeDb.unicastRoutes()->clear();
  routeDb.unicastRoutes()->emplace_back(
      createUnicastRoute(prefix2, {path1_2_2, path1_2_3}));
  routeDb.unicastRoutes()->emplace_back(
      createUnicastRoute(prefix3, {path1_3_2}));
  routeDetailDb.unicastRoutes()->clear();
  routeDetailDb.unicastRoutes()->emplace_back(
      createUnicastRouteDetail(prefix2, {path1_2_2, path1_2_3}, bestRoute2));
  routeDetailDb.unicastRoutes()->emplace_back(
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

TEST_F(FibTestFixture, WaitOnDecision) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  // Mimic decision pub sock publishing RouteDatabaseDelta
  DecisionRouteUpdate routeUpdate;
  routeUpdate.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1, path1_2_2}));

  routeUpdatesQueue.push(routeUpdate);

  // initial syncFib debounce
  mockFibHandler_->waitForSyncFib();
  // Synced routes are sent to fibRouteUpdatesQueue_.
  routeUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      routeUpdate, fibRouteUpdatesQueueReader.get().value()));

  // ensure no other calls occured
  EXPECT_EQ(mockFibHandler_->getFibSyncCount(), 1);
  EXPECT_EQ(mockFibHandler_->getAddRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler_->getDelRoutesCount(), 0);
}

TEST_F(FibTestFixture, getUnicastRoutesFilteredTest) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  // initial syncFib debounce
  routeUpdatesQueue.push(DecisionRouteUpdate());
  mockFibHandler_->waitForSyncFib();
  // Empty routes are sent fi tobRouteUpdatesQueue_.
  DecisionRouteUpdate emptyUpdate;
  emptyUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  EXPECT_TRUE(checkEqualDecisionRouteUpdate(
      emptyUpdate, fibRouteUpdatesQueueReader.get().value()));

  const auto testPrefix1 = toIpPrefix("192.168.20.16/28");
  const auto testPrefix2 = toIpPrefix("192.168.0.0/16");
  const auto testPrefix3 = toIpPrefix("fd00::48:2:0/128");
  const auto testPrefix4 = toIpPrefix("fd00::48:2:0/126");

  auto route1 = RibUnicastEntry(toIPNetwork(testPrefix1), {});
  auto route2 = RibUnicastEntry(toIPNetwork(testPrefix2), {});
  auto route3 = RibUnicastEntry(toIPNetwork(testPrefix3), {});
  auto route4 = RibUnicastEntry(toIPNetwork(testPrefix4), {});

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
  *expectedDb.thisNodeName() = "node-1";
  expectedDb.unicastRoutes()->emplace_back(tRoute1);
  expectedDb.unicastRoutes()->emplace_back(tRoute2);
  expectedDb.unicastRoutes()->emplace_back(tRoute4);
  // check if match correctly
  thrift::RouteDatabase responseDb;
  const auto& responseRoutes = getUnicastRoutesFiltered(std::move(filter));
  *responseDb.unicastRoutes() = responseRoutes;
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(expectedDb, responseDb));

  // check when get empty input - return all unicast routes
  thrift::RouteDatabase allRouteDb;
  allRouteDb.unicastRoutes()->emplace_back(tRoute1);
  allRouteDb.unicastRoutes()->emplace_back(tRoute2);
  allRouteDb.unicastRoutes()->emplace_back(tRoute3);
  allRouteDb.unicastRoutes()->emplace_back(tRoute4);
  auto emptyParamRet =
      std::unique_ptr<std::vector<std::string>>(new std::vector<std::string>());
  const auto& allRoutes = getUnicastRoutesFiltered(std::move(emptyParamRet));
  thrift::RouteDatabase allRoutesRespDb;
  *allRoutesRespDb.unicastRoutes() = allRoutes;
  EXPECT_TRUE(checkEqualRouteDatabaseUnicast(allRouteDb, allRoutesRespDb));

  // check getUnicastRoutes() API - return all unicast routes
  const auto& allRoute = getUnicastRoutes();
  thrift::RouteDatabase allRoutesApiDb;
  *allRoutesApiDb.unicastRoutes() = allRoute;
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
  EXPECT_TRUE(!result4.has_value());

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

  const auto testPrefix1 = toIpPrefix("192.168.20.16/28");
  const auto testPrefix2 = toIpPrefix("192.168.0.0/16");
  const auto testPrefix3 = toIpPrefix("fd00::48:2:0/128");
  const auto testPrefix4 = toIpPrefix("fd00::48:2:0/126");

  auto route1 = RibUnicastEntry(toIPNetwork(testPrefix1), {});
  auto route2 = RibUnicastEntry(toIPNetwork(testPrefix2), {});
  auto route3 = RibUnicastEntry(toIPNetwork(testPrefix3), {});
  auto route4 = RibUnicastEntry(toIPNetwork(testPrefix4), {});

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
 * Validates FIB synchronization logic and its error handling. This handles
 * re-sync logic of FIB as well.
 *
 * 1) Ensure FIB gets synced
 * 2) Trigger FIB sync (Fib Restart) with std::exception failure injection
 * 3) Trigger FIB sync (Fib Restart) with FibUpdateError failure injection
 */
TEST_F(FibTestFixture, SyncFibProgramming) {
  std::vector<thrift::UnicastRoute> routes;

  //
  // Send first RIB update
  //
  DecisionRouteUpdate routeUpdate;
  routeUpdate.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1}));
  routeUpdate.addRouteToUpdate(
      RibUnicastEntry(toIPNetwork(prefix2), {path1_2_1}));
  routeUpdatesQueue.push(routeUpdate);

  //
  // 1) Verify initial FIB sync
  //

  // Wait for sync to happen & verify routes
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(2, routes.size());

  // Fib should also produce a publication
  routeUpdate.type = DecisionRouteUpdate::FULL_SYNC;
  checkEqualDecisionRouteUpdate(
      routeUpdate, fibRouteUpdatesQueueReader.get().value());

  //
  // 2) Restart FIB to trigger Fib Sync - with unhealthy state exception
  //

  // Mark fib as unhealthy and restart
  mockFibHandler_->setHandlerHealthyState(false);
  mockFibHandler_->restart();

  // Wait for exception to get triggerred in mock-fib handler
  mockFibHandler_->waitForUnhealthyException();

  // Make sure routes are not yet programmed or published
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  EXPECT_EQ(0, fibRouteUpdatesQueueReader.size());

  // Mark fib as healthy & wait for sync signals
  mockFibHandler_->setHandlerHealthyState(true);
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);

  // Make sure FIB publication is empty
  {
    auto publication = fibRouteUpdatesQueueReader.get().value();
    EXPECT_TRUE(publication.empty());
    EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, publication.type);
  }

  //
  // 3) Restart FIB to trigger Fib Sync - with PlatformFibUpdateError exception
  //

  // Mark prefix2/prefix3 as dirty in Fib
  mockFibHandler_->setDirtyState(
      {toIPNetwork(prefix2), toIPNetwork(prefix3)}, {});
  mockFibHandler_->restart();

  // Make sure routes are not yet programmed or published
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  EXPECT_EQ(0, fibRouteUpdatesQueueReader.size());

  // Wait for fib sync event & verify that 1 unicast route gets programmed
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);

  // Make sure FIB publication withdraws prefix2
  {
    auto publication = fibRouteUpdatesQueueReader.get().value();
    EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, publication.type);
    EXPECT_EQ(1, publication.size()); // modifications
    ASSERT_EQ(1, publication.unicastRoutesToDelete.size());
    EXPECT_EQ(toIPNetwork(prefix2), publication.unicastRoutesToDelete.at(0));
  }

  // Wait for deletion of prefix3, label3 go through
  mockFibHandler_->waitForUpdateUnicastRoutes(); // prefix2 (will fail)
  mockFibHandler_->waitForDeleteUnicastRoutes(); // prefix3

  // Number of routes in MockFibHandler doesn't change
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);

  // Make sure FIB publication withdraws prefix3. `prefix2` and
  // `label2` will also be retried and fails, so they'll be reported as failed
  {
    auto publication = fibRouteUpdatesQueueReader.get().value();
    EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, publication.type);
    EXPECT_EQ(2, publication.size()); // modifications
    ASSERT_EQ(2, publication.unicastRoutesToDelete.size());
  }

  // Clear dirty state
  mockFibHandler_->setDirtyState({}, {});

  // Wait for incremental programming of unicast routes
  mockFibHandler_->waitForUpdateUnicastRoutes(); // prefix2 (will succeed)
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);

  // Make sure FIB publication programs prefix2
  {
    auto publication = fibRouteUpdatesQueueReader.get().value();
    EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, publication.type);
    EXPECT_EQ(1, publication.size()); // modifications
    ASSERT_EQ(1, publication.unicastRoutesToUpdate.size());
    EXPECT_TRUE(publication.unicastRoutesToUpdate.count(toIPNetwork(prefix2)));
  }

  // Make sure there are no pending FIB publications (just for fun)
  EXPECT_EQ(0, fibRouteUpdatesQueueReader.size());
}

/**
 * Validates incremental route programming and its error handling.
 * - Add P1
 * - Add P2 - with std::exception failure injection
 * - Update P1 - with FibUpdateError failure injection
 * - Delete P2 - with std::exception failure injection
 * - Delete P1
 */
TEST_F(FibTestFixture, IncrementalRouteProgramming) {
  std::vector<thrift::UnicastRoute> routes;

  //
  // Initialize FIB to SYNCED state with empty route db
  //
  routeUpdatesQueue.push(DecisionRouteUpdate());
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(0, routes.size());
  EXPECT_TRUE(fibRouteUpdatesQueueReader.get()->empty());

  //
  // 1) Add Routes - Prefix1
  //
  {
    // Advertise Prefix1 update
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(
        RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1}));
    routeUpdatesQueue.push(routeUpdate);

    // Verify that they get added successfully
    mockFibHandler_->waitForUpdateUnicastRoutes();
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(1, routes.size());

    // Verify that update is reflected in fib route updates
    auto publication = fibRouteUpdatesQueueReader.get().value();
    routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));
    EXPECT_EQ(1, publication.size());
    EXPECT_TRUE(publication.unicastRoutesToUpdate.count(toIPNetwork(prefix1)));
  }

  //
  // 2) Add some more routes - Prefix2 & introduce std::exception
  //
  {
    // Set handler unhealthy
    mockFibHandler_->setHandlerHealthyState(false);

    // Advertise Prefix2 update
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(
        RibUnicastEntry(toIPNetwork(prefix2), {path1_2_1}));
    routeUpdatesQueue.push(routeUpdate);

    // Verify that they don't get programmed. Wait for exception and make sure
    // it retries. And also for backoff to increase.
    mockFibHandler_->waitForUnhealthyException(6);
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(1, routes.size());

    // Verify that update is reflected as route withdraws in fib publication
    // NOTE: We'll receive update twice
    for (auto i = 0; i < 6; ++i) {
      auto publication = fibRouteUpdatesQueueReader.get().value();
      EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, publication.type);
      EXPECT_EQ(1, publication.size());
      EXPECT_EQ(toIPNetwork(prefix2), publication.unicastRoutesToDelete.at(0));
    }

    // Set handler healthy
    mockFibHandler_->setHandlerHealthyState(true);

    // Wait for route sync updates
    mockFibHandler_->waitForUpdateUnicastRoutes();
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(2, routes.size());

    // Verify that update is reflected as is in fib publication
    auto publication = fibRouteUpdatesQueueReader.get().value();
    routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));
  }

  //
  // 3) Update routes - Prefix1 & introduce FibUpdateError
  //
  {
    // Set dirty state to introduce FibUpdateError
    mockFibHandler_->setDirtyState({toIPNetwork(prefix1)}, {});

    // Advertise Prefix1 update
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(
        RibUnicastEntry(toIPNetwork(prefix1), {path1_2_2}));
    routeUpdatesQueue.push(routeUpdate);

    // Wait for route programming to proceed. Let it repeat a few times.
    // Verify that update is reflected as route withdraws in fib publication
    // NOTE: We'll receive publication multiple times (because of multiple
    // retries). Read publications later on
    for (int i = 0; i < 6; i++) {
      mockFibHandler_->waitForUpdateUnicastRoutes();
    }
    for (int i = 0; i < 6; i++) {
      auto publication = fibRouteUpdatesQueueReader.get().value();
      EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, publication.type);
      EXPECT_EQ(1, publication.size());
      EXPECT_EQ(toIPNetwork(prefix1), publication.unicastRoutesToDelete.at(0));
    }

    // Unset dirty state
    mockFibHandler_->setDirtyState({}, {});

    // Wait for route programming to proceed
    mockFibHandler_->waitForUpdateUnicastRoutes();

    // Verify that update is reflected as is in fib publication
    const auto publication = fibRouteUpdatesQueueReader.get().value();
    routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));

    // Verify the number of unicast and mpls routes
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(2, routes.size());
  }

  //
  // 4) Delete routes - Prefix2 with std::exception
  //
  {
    // Set handler unhealthy
    mockFibHandler_->setHandlerHealthyState(false);

    // Withdraw routes
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(prefix2));
    routeUpdatesQueue.push(routeUpdate);

    // There will be immediate notification about route withdrawl
    auto publication = fibRouteUpdatesQueueReader.get().value();
    routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));

    // Verify that they don't get programmed. Wait for exception for each
    // type for multiple times, to make sure it retries.
    for (int i = 0; i < 6; i++) {
      mockFibHandler_->waitForUnhealthyException(); // Unicast route
    }
    for (int i = 0; i < 6; i++) {
      publication = fibRouteUpdatesQueueReader.get().value();
      routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
      EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));
    }

    // Verify that route doesn't gets programmed
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(2, routes.size());

    // Set handler healthy
    mockFibHandler_->setHandlerHealthyState(true);

    // Verify that they get removed
    mockFibHandler_->waitForDeleteUnicastRoutes();
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(1, routes.size());

    // Verify that they're reported as withdrawn again (We can do optimize here
    // in code, but it is not going to affect correctness).
    publication = fibRouteUpdatesQueueReader.get().value();
    routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));
  }

  //
  // 5. Delete routes - Prefix1 (without any exception)
  //
  {
    // Withdraw routes
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(prefix1));
    routeUpdatesQueue.push(routeUpdate);

    // Verify that they get removed
    mockFibHandler_->waitForDeleteUnicastRoutes();
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(0, routes.size());

    // Verify that they're reported as withdrawn
    auto publication = fibRouteUpdatesQueueReader.get().value();
    LOG(INFO) << publication.str();
    routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));
  }
}

/**
 * Validates incremental route programming when certain set of routes keep
 * failing.
 * - Mark P2 as bad to introduce FibUpdateError
 * - Add P2 and see they won't get added
 * - Add P1 and see they'll get added
 * - Delete P1 and see they get removed
 * - Mark P2 as good
 * - See that they gets programmed
 */
TEST_F(FibTestFixture, RouteProgrammingWithPersistentFailure) {
  std::vector<thrift::UnicastRoute> routes;

  //
  // Initialize FIB to SYNCED state with empty route db
  //
  routeUpdatesQueue.push(DecisionRouteUpdate());
  mockFibHandler_->waitForSyncFib();
  mockFibHandler_->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(0, routes.size());
  EXPECT_TRUE(fibRouteUpdatesQueueReader.get()->empty());

  //
  // 1) Mark P2 as bad to introduce FibUpdateError
  //
  mockFibHandler_->setDirtyState({toIPNetwork(prefix2)}, {});

  //
  // 2) Add P2 and see they won't get added
  //
  {
    // Advertise Prefix2/Label2 update
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(
        RibUnicastEntry(toIPNetwork(prefix2), {path1_2_1}));
    routeUpdatesQueue.push(routeUpdate);

    // Verify that they don't get programmed. Wait for exception for each type
    // and multiple times. We wait for multiple times for backoff to increase
    for (int i = 0; i < 10; ++i) {
      mockFibHandler_->waitForUpdateUnicastRoutes();
      mockFibHandler_->getRouteTableByClient(routes, kFibId);
      EXPECT_EQ(0, routes.size());

      // Verify that update is reflected as route withdraws in fib publication
      auto publication = fibRouteUpdatesQueueReader.get().value();
      EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, publication.type);
      EXPECT_EQ(1, publication.size());
      EXPECT_EQ(toIPNetwork(prefix2), publication.unicastRoutesToDelete.at(0));
    }
  }

  //
  // 3) Add P1 and see they'll get added
  //
  {
    // Advertise Prefix1 update
    DecisionRouteUpdate routeUpdate;
    routeUpdate.addRouteToUpdate(
        RibUnicastEntry(toIPNetwork(prefix1), {path1_2_1}));
    routeUpdatesQueue.push(routeUpdate);

    // Verify that they get added successfully
    mockFibHandler_->waitForUpdateUnicastRoutes();
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(1, routes.size());

    // Verify that update is reflected in fib route updates
    auto publication = fibRouteUpdatesQueueReader.get().value();
    routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));
  }

  //
  // 4) Delete P1 and see they'll get deleted
  //
  {
    // Withdraw routes
    DecisionRouteUpdate routeUpdate;
    routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(prefix1));
    routeUpdatesQueue.push(routeUpdate);

    // Verify that they get removed
    mockFibHandler_->waitForDeleteUnicastRoutes();
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(0, routes.size());

    // Verify that they're reported as withdrawn twice - Once immediately &
    // second time delayed
    routeUpdate.type = DecisionRouteUpdate::INCREMENTAL;
    auto publication = fibRouteUpdatesQueueReader.get().value();
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));
    publication = fibRouteUpdatesQueueReader.get().value();
    EXPECT_TRUE(checkEqualDecisionRouteUpdate(routeUpdate, publication));
  }

  //
  // 5) Mark P2 as good and see it gets programmed
  //
  {
    // Reset dirty state
    mockFibHandler_->setDirtyState({}, {});

    // Verify that they get added successfully
    mockFibHandler_->waitForUpdateUnicastRoutes();
    mockFibHandler_->getRouteTableByClient(routes, kFibId);
    EXPECT_EQ(1, routes.size());

    // Verify that they're published as part of fib update
    auto publication = fibRouteUpdatesQueueReader.get().value();
    LOG(INFO) << publication.str();
    EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, publication.type);
    EXPECT_EQ(1, publication.size());
    EXPECT_TRUE(publication.unicastRoutesToUpdate.count(toIPNetwork(prefix2)));
  }
}

TEST(FibTest, createFibClientRetryTest) {
  // Ensure that we could retry createFibClient without crashing
  folly::EventBase evb;
  std::unique_ptr<apache::thrift::Client<thrift::FibService>> client;

  for (int i = 0; i < 10; ++i) {
    // Retry creation 10 times
    openr::Fib::createFibClient(evb, client, 1111);

    evb.loop();
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  testing::InitGoogleMock(&argc, argv);
  const folly::Init init(&argc, &argv);
  google::InstallFailureSignalHandler();

  auto rc = RUN_ALL_TESTS();

  // Run the tests
  return rc;
}
