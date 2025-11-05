/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Sleep.h>
#include <folly/init/Init.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/common/Constants.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/messaging/Queue.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/mocks/NetlinkEventsInjector.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

namespace {
AreaId const kSpineAreaId("spine");
AreaId const kPlaneAreaId("plane");
AreaId const kPodAreaId("pod");

std::set<std::string> const kSpineOnlySet = {kSpineAreaId};
} // namespace

class OpenrCtrlFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // create config
    std::vector<openr::thrift::AreaConfig> areaConfig;
    for (auto id : {kSpineAreaId, kPlaneAreaId, kPodAreaId}) {
      thrift::AreaConfig area;
      area.area_id() = id;
      area.include_interface_regexes() = {"po.*"};
      area.neighbor_regexes() = {".*"};
      areaConfig.emplace_back(std::move(area));
    }
    auto tConfig =
        getBasicOpenrConfig(nodeName_, areaConfig, true /* enableV4 */);

    config = std::make_shared<Config>(tConfig);

    // Create the PersistentStore and start fresh
    std::remove((*tConfig.persistent_config_store_path()).data());
    persistentStore = std::make_unique<PersistentStore>(config);
    persistentStoreThread_ = std::thread([&]() { persistentStore->run(); });

    // Create KvStore module
    kvStoreWrapper_ =
        std::make_unique<KvStoreWrapper<thrift::OpenrCtrlCppAsyncClient>>(
            config->getAreaIds(),
            config->toThriftKvStoreConfig(),
            std::nullopt,
            kvRequestQueue_.getReader());
    kvStoreWrapper_->run();

    // Create Dispatcher module
    dispatcher_ = std::make_shared<Dispatcher>(
        kvStoreWrapper_->getReader(), kvStorePublicationsQueue_);

    dispatcherThread_ = std::thread([this]() { dispatcher_->run(); });
    dispatcher_->waitUntilRunning();

    // Create Decision module
    decision = std::make_shared<Decision>(
        config,
        peerUpdatesQueue_.getReader(),
        kvStoreWrapper_->getReader(),
        staticRoutesUpdatesQueue_.getReader(),
        routeUpdatesQueue_);
    decisionThread_ = std::thread([&]() { decision->run(); });

    // Create Fib moduleKeySyncMultipleArea
    fib = std::make_shared<Fib>(
        config, routeUpdatesQueue_.getReader(), fibRouteUpdatesQueue_);
    fibThread_ = std::thread([&]() { fib->run(); });

    // Create PrefixManager module
    prefixManager = std::make_shared<PrefixManager>(
        staticRoutesUpdatesQueue_,
        kvRequestQueue_,
        prefixMgrInitializationEventsQueue_,
        dispatcher_->getReader({Constants::kPrefixDbMarker.toString()}),
        prefixUpdatesQueue_.getReader(),
        fibRouteUpdatesQueue_.getReader(),
        config);
    prefixManagerThread_ = std::thread([&]() { prefixManager->run(); });

    // create fakeNetlinkProtocolSocket
    nlSock_ = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&evb_);

    // Create LinkMonitor
    linkMonitor = std::make_shared<LinkMonitor>(
        config,
        nlSock_.get(),
        persistentStore.get(),
        interfaceUpdatesQueue_,
        prefixUpdatesQueue_,
        peerUpdatesQueue_,
        logSampleQueue_,
        kvRequestQueue_,
        dispatcher_->getReader({"link-monitor"}),
        neighborUpdatesQueue_.getReader(),
        nlSock_->getReader());
    linkMonitorThread_ = std::thread([&]() { linkMonitor->run(); });

    // initialize OpenrCtrlHandler for testing usage
    handler_ = std::make_shared<OpenrCtrlHandler>(
        nodeName_,
        std::unordered_set<std::string>{},
        &ctrlEvb_,
        decision.get(),
        fib.get(),
        kvStoreWrapper_->getKvStore(),
        linkMonitor.get(),
        monitor.get(),
        persistentStore.get(),
        prefixManager.get(),
        nullptr,
        config,
        dispatcher_.get());
    ctrlEvbThread_ = std::thread([&]() { ctrlEvb_.run(); });
    ctrlEvb_.waitUntilRunning();
  }

  void
  TearDown() override {
    routeUpdatesQueue_.close();
    staticRoutesUpdatesQueue_.close();
    prefixMgrInitializationEventsQueue_.close();
    interfaceUpdatesQueue_.close();
    peerUpdatesQueue_.close();
    neighborUpdatesQueue_.close();
    prefixUpdatesQueue_.close();
    fibRouteUpdatesQueue_.close();
    kvRequestQueue_.close();
    logSampleQueue_.close();
    nlSock_->closeQueue();
    kvStoreWrapper_->closeQueue();
    kvStorePublicationsQueue_.close();

    // ATTN: stop handler first as it holds all ptrs
    handler_.reset();
    ctrlEvb_.stop();
    ctrlEvb_.waitUntilStopped();
    ctrlEvbThread_.join();

    linkMonitor->stop();
    linkMonitorThread_.join();

    persistentStore->stop();
    persistentStoreThread_.join();

    prefixManager->stop();
    prefixManagerThread_.join();

    nlSock_.reset();

    fib->stop();
    fibThread_.join();

    decision->stop();
    decisionThread_.join();

    dispatcher_->stop();
    dispatcherThread_.join();

    kvStoreWrapper_->stop();
    kvStoreWrapper_.reset();
  }

  thrift::PrefixEntry
  createPrefixEntry(const std::string& prefix, thrift::PrefixType prefixType) {
    thrift::PrefixEntry prefixEntry;
    *prefixEntry.prefix() = toIpPrefix(prefix);
    prefixEntry.type() = prefixType;
    return prefixEntry;
  }

  void
  setKvStoreKeyVals(const thrift::KeyVals& keyVals, const std::string& area) {
    thrift::KeySetParams setParams;
    setParams.keyVals() = keyVals;

    handler_
        ->semifuture_setKvStoreKeyVals(
            std::make_unique<thrift::KeySetParams>(setParams),
            std::make_unique<std::string>(area))
        .get();
  }

#if FOLLY_HAS_COROUTINES
  folly::coro::Task<void>
  co_setKvStoreKeyVals(
      const thrift::KeyVals& keyVals, const std::string& area) {
    thrift::KeySetParams setParams;
    setParams.keyVals() = keyVals;

    co_await handler_->co_setKvStoreKeyVals(
        std::make_unique<thrift::KeySetParams>(setParams),
        std::make_unique<std::string>(area));
  }

  folly::coro::Task<std::unique_ptr<thrift::SetKeyValsResult>>
  co_setKvStoreKeyValues(
      const thrift::KeyVals& keyVals, const std::string& area) {
    thrift::KeySetParams setParams;
    setParams.keyVals() = keyVals;

    auto result = co_await handler_->co_setKvStoreKeyValues(
        std::make_unique<thrift::KeySetParams>(setParams),
        std::make_unique<std::string>(area));
    co_return result;
  }
#endif

  std::unique_ptr<openr::thrift::SetKeyValsResult>
  setKvStoreKeyValues(
      const thrift::KeyVals& keyVals,
      const std::string& area,
      const std::string& nodeId = "") {
    thrift::KeySetParams setParams;
    setParams.keyVals() = keyVals;
    if (!nodeId.empty()) {
      setParams.nodeIds() = {nodeId};
    }
    return handler_
        ->semifuture_setKvStoreKeyValues(
            std::make_unique<thrift::KeySetParams>(setParams),
            std::make_unique<std::string>(area))
        .get();
  }

 protected:
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue_;
  messaging::ReplicateQueue<InterfaceDatabase> interfaceUpdatesQueue_;
  messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue_;
  messaging::ReplicateQueue<NeighborInitEvent> neighborUpdatesQueue_;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRoutesUpdatesQueue_;
  messaging::ReplicateQueue<thrift::InitializationEvent>
      prefixMgrInitializationEventsQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue_;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
  messaging::ReplicateQueue<LogSample> logSampleQueue_;
  DispatcherQueue kvStorePublicationsQueue_;

  folly::EventBase evb_;
  OpenrEventBase ctrlEvb_;

  std::thread decisionThread_;
  std::thread fibThread_;
  std::thread prefixManagerThread_;
  std::thread persistentStoreThread_;
  std::thread linkMonitorThread_;
  std::thread ctrlEvbThread_;
  std::thread dispatcherThread_;

  std::shared_ptr<Config> config;
  std::shared_ptr<Decision> decision;
  std::shared_ptr<Fib> fib;
  std::shared_ptr<PrefixManager> prefixManager;
  std::shared_ptr<PersistentStore> persistentStore;
  std::shared_ptr<LinkMonitor> linkMonitor;
  std::shared_ptr<Monitor> monitor;
  std::shared_ptr<Dispatcher> dispatcher_;

 public:
  const std::string nodeName_{"thanos@universe"};
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock_{nullptr};
  std::unique_ptr<KvStoreWrapper<thrift::OpenrCtrlCppAsyncClient>>
      kvStoreWrapper_{nullptr};
  std::shared_ptr<OpenrCtrlHandler> handler_{nullptr};
};

TEST_F(OpenrCtrlFixture, GetMyNodeName) {
  std::string res;
  handler_->getMyNodeName(res);
  EXPECT_EQ(nodeName_, res);
}

#if FOLLY_HAS_COROUTINES
CO_TEST_F(OpenrCtrlFixture, GetMyNodeNameCoro) {
  auto name = co_await handler_->co_getMyNodeName();
  EXPECT_EQ(nodeName_, *name);
}
#endif // FOLLY_HAS_COROUTINES

TEST_F(OpenrCtrlFixture, InitializationApis) {
  // Add KVSTORE_SYNCED event into fb303. Initialization not converged yet.
  logInitializationEvent(
      "KvStore", thrift::InitializationEvent::KVSTORE_SYNCED);
  EXPECT_FALSE(handler_->initializationConverged());
  EXPECT_THROW(handler_->getInitializationDurationMs(), std::invalid_argument);
  std::map<thrift::InitializationEvent, int64_t> events;
  handler_->getInitializationEvents(events);
  EXPECT_EQ(events.count(thrift::InitializationEvent::KVSTORE_SYNCED), 1);

  // Add INITIALIZED event into fb303. Initialization converged.
  logInitializationEvent(
      "PrefixManager", thrift::InitializationEvent::INITIALIZED);
  EXPECT_TRUE(handler_->initializationConverged());
  EXPECT_GE(handler_->getInitializationDurationMs(), 0);
  handler_->getInitializationEvents(events);
  EXPECT_EQ(events.count(thrift::InitializationEvent::INITIALIZED), 1);
}

TEST_F(OpenrCtrlFixture, PrefixManagerApis) {
  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("10.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("11.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("20.0.0.0/8", thrift::PrefixType::BGP),
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    handler_
        ->semifuture_advertisePrefixes(
            std::make_unique<std::vector<thrift::PrefixEntry>>(
                std::move(prefixes)))
        .get();
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    handler_
        ->semifuture_withdrawPrefixes(
            std::make_unique<std::vector<thrift::PrefixEntry>>(
                std::move(prefixes)))
        .get();
    handler_->semifuture_withdrawPrefixesByType(thrift::PrefixType::LOOPBACK);
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    handler_->semifuture_syncPrefixesByType(
        thrift::PrefixType::BGP,
        std::make_unique<std::vector<thrift::PrefixEntry>>(
            std::move(prefixes)));
  }

  {
    const std::vector<thrift::PrefixEntry> exp{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto res = handler_->semifuture_getPrefixes().get();
    EXPECT_EQ(exp, *res);
  }

  {
    auto res =
        handler_->semifuture_getPrefixesByType(thrift::PrefixType::LOOPBACK)
            .get();
    EXPECT_EQ(0, res->size());
  }

  {
    auto routes = handler_->semifuture_getAdvertisedRoutes().get();
    EXPECT_EQ(1, routes->size());
  }
}

TEST_F(OpenrCtrlFixture, RouteApis) {
  {
    auto db = handler_->semifuture_getRouteDb().get();
    EXPECT_EQ(nodeName_, *db->thisNodeName());
    EXPECT_EQ(0, db->unicastRoutes()->size());
    EXPECT_EQ(0, db->mplsRoutes()->size());
  }

  {
    auto db = handler_
                  ->semifuture_getRouteDbComputed(
                      std::make_unique<std::string>(nodeName_))
                  .get();
    EXPECT_EQ(nodeName_, *db->thisNodeName());
    EXPECT_EQ(0, db->unicastRoutes()->size());
    EXPECT_EQ(0, db->mplsRoutes()->size());
  }

  {
    const std::string testNode("avengers@universe");
    auto db = handler_
                  ->semifuture_getRouteDbComputed(
                      std::make_unique<std::string>(testNode))
                  .get();
    EXPECT_EQ(testNode, *db->thisNodeName());
    EXPECT_EQ(0, db->unicastRoutes()->size());
    EXPECT_EQ(0, db->mplsRoutes()->size());
  }

  {
    const std::vector<std::string> prefixes{"10.46.2.0", "10.46.2.0/24"};
    auto res = handler_
                   ->semifuture_getUnicastRoutesFiltered(
                       std::make_unique<std::vector<std::string>>(prefixes))
                   .get();
    EXPECT_EQ(0, res->size());
  }

  {
    auto res = handler_->semifuture_getUnicastRoutes().get();
    EXPECT_EQ(0, res->size());
  }
  {
    const std::vector<std::int32_t> labels{1, 2};
    auto res = handler_
                   ->semifuture_getMplsRoutesFiltered(
                       std::make_unique<std::vector<std::int32_t>>(labels))
                   .get();
    EXPECT_EQ(0, res->size());
  }
  {
    auto res = handler_->semifuture_getMplsRoutes().get();
    EXPECT_EQ(0, res->size());
  }
}

TEST_F(OpenrCtrlFixture, PerfApis) {
  auto db = handler_->semifuture_getPerfDb().get();
  EXPECT_EQ(nodeName_, *db->thisNodeName());
}

TEST_F(OpenrCtrlFixture, DrainStateApis) {
  auto state =
      handler_
          ->semifuture_getDrainState(std::make_unique<std::string>(nodeName_))
          .get();
  ASSERT_TRUE(state->drain_state());
  EXPECT_EQ(thrift::DrainState::UNDRAINED, *state->drain_state());
  ASSERT_FALSE(state->drained_interfaces());
}

TEST_F(OpenrCtrlFixture, DecisionApis) {
  {
    auto dbs = handler_
                   ->semifuture_getDecisionAdjacenciesFiltered(
                       std::make_unique<thrift::AdjacenciesFilter>())
                   .get();
    EXPECT_EQ(0, dbs->size());
  }

  {
    auto dbs = handler_
                   ->semifuture_getDecisionAreaAdjacenciesFiltered(
                       std::make_unique<thrift::AdjacenciesFilter>())
                   .get();
    EXPECT_EQ(0, dbs->size());
  }

  {
    auto routes = handler_->semifuture_getReceivedRoutes().get();
    EXPECT_EQ(0, routes->size());
  }

  {
    // Positive Test
    auto routes = handler_
                      ->semifuture_getReceivedRoutesFiltered(
                          std::make_unique<thrift::ReceivedRouteFilter>())
                      .get();
    EXPECT_EQ(0, routes->size());

    // Nevative Test
    thrift::ReceivedRouteFilter filter;
    folly::IPAddress v4Addr = folly::IPAddress("11.0.0.1");
    folly::IPAddress v6Addr = folly::IPAddress("fe80::1");
    thrift::IpPrefix v4Prefix, v6Prefix;
    v4Prefix.prefixAddress() = toBinaryAddress(v4Addr);
    v4Prefix.prefixLength() = 36; // ATTN: max mask length is 32 for IPV4
    v6Prefix.prefixAddress() = toBinaryAddress(v6Addr);
    v6Prefix.prefixLength() = 130; // ATTN: max mask length is 32 for IPV4

    filter.prefixes() = {v4Prefix};
    EXPECT_THROW(
        handler_
            ->semifuture_getReceivedRoutesFiltered(
                std::make_unique<thrift::ReceivedRouteFilter>(filter))
            .get(),
        thrift::OpenrError);

    filter.prefixes() = {v6Prefix};
    EXPECT_THROW(
        handler_
            ->semifuture_getReceivedRoutesFiltered(
                std::make_unique<thrift::ReceivedRouteFilter>(filter))
            .get(),
        thrift::OpenrError);
  }
}

TEST_F(OpenrCtrlFixture, KvStoreSetApi) {
  thrift::KeyVals kvs(
      {{"key1", createThriftValue(2, "node1", std::string("value1"))},
       {"key11", createThriftValue(1, "node1", std::string("value11"))},
       {"key111", createThriftValue(1, "node1", std::string("value111"))},
       {"key2", createThriftValue(1, "node1", std::string("value2"))},
       {"key22", createThriftValue(1, "node1", std::string("value22"))},
       {"key222", createThriftValue(1, "node1", std::string("value222"))},
       {"key3", createThriftValue(1, "node3", std::string("value3"))},
       {"key33", createThriftValue(1, "node33", std::string("value33"))},
       {"key333", createThriftValue(1, "node33", std::string("value333"))}});

  thrift::KeyVals keyValsPod;
  keyValsPod["keyPod1"] =
      createThriftValue(1, "node1", std::string("valuePod1"));
  keyValsPod["keyPod2"] =
      createThriftValue(1, "node1", std::string("valuePod2"));

  thrift::KeyVals keyValsPlane;
  keyValsPlane["keyPlane1"] =
      createThriftValue(1, "node1", std::string("valuePlane1"));
  keyValsPlane["keyPlane2"] =
      createThriftValue(1, "node1", std::string("valuePlane2"));

  const std::string kSimpleNodeId = "node1";
  //
  // area list get
  //
  {
    auto config = handler_->semifuture_getRunningConfigThrift().get();
    std::unordered_set<std::string> areas;
    for (auto const& area : *config->areas()) {
      areas.insert(*area.area_id());
    }
    EXPECT_THAT(areas, testing::SizeIs(3));
    EXPECT_THAT(
        areas,
        testing::UnorderedElementsAre(kPodAreaId, kPlaneAreaId, kSpineAreaId));
  }

  // Success
  {
    EXPECT_EQ(
        setKvStoreKeyValues(kvs, kSpineAreaId)->noMergeReasons()->size(), 0);
    EXPECT_EQ(
        setKvStoreKeyValues(keyValsPod, kPodAreaId)->noMergeReasons()->size(),
        0);
    EXPECT_EQ(
        setKvStoreKeyValues(keyValsPlane, kPlaneAreaId)
            ->noMergeReasons()
            ->size(),
        0);
  }

  // LOOP_DETECTED
  {
    const std::string kLoopDetectionKey = "key99";
    thrift::KeyVals loopKvs(
        {{kLoopDetectionKey,
          createThriftValue(1, "node1", std::string("value1"))}});
    auto result = *setKvStoreKeyValues(loopKvs, kSpineAreaId, nodeName_)
                       ->noMergeReasons();
    EXPECT_EQ(result.size(), 1);
    EXPECT_EQ(
        thrift::KvStoreNoMergeReason::LOOP_DETECTED, result[kLoopDetectionKey]);
  }

  // INVALID_TTL
  {
    const std::string kInvalidTtlKey = "key99";
    int64_t kInvalidTtl = -1;
    thrift::KeyVals invalidTtlKvs(
        {{kInvalidTtlKey,
          createThriftValue(1, "node1", std::string("value1"), kInvalidTtl)}});
    auto result =
        *setKvStoreKeyValues(invalidTtlKvs, kSpineAreaId)->noMergeReasons();
    EXPECT_EQ(result.size(), 1);
    EXPECT_EQ(
        thrift::KvStoreNoMergeReason::INVALID_TTL, result[kInvalidTtlKey]);
  }

  // OLD_VERSION
  {
    const std::string kOldVersionKey = "key1";
    int64_t kOldVersion = 1;
    thrift::KeyVals oldVersionKvs(
        {{kOldVersionKey,
          createThriftValue(kOldVersion, "node1", std::string("value1"))}});
    auto result =
        *setKvStoreKeyValues(oldVersionKvs, kSpineAreaId)->noMergeReasons();
    EXPECT_EQ(result.size(), 1);
    EXPECT_EQ(
        thrift::KvStoreNoMergeReason::OLD_VERSION, result[kOldVersionKey]);
  }

  // NO_NEED_TO_UPDATE
  {
    auto result = *setKvStoreKeyValues(kvs, kSpineAreaId)->noMergeReasons();
    EXPECT_EQ(result.size(), kvs.size());
    for (const auto& [_, v] : result) {
      EXPECT_EQ(thrift::KvStoreNoMergeReason::NO_NEED_TO_UPDATE, v);
    }
  }
  // TODO:
  // NO_MATCHED_KEY = 1,
  // INCONSISTENCY_DETECTED = 6,
}

#if FOLLY_HAS_COROUTINES
CO_TEST_F(OpenrCtrlFixture, CoKvStoreApis) {
  thrift::KeyVals kvs(
      {{"key1", createThriftValue(1, "node1", std::string("value1"))},
       {"key11", createThriftValue(1, "node1", std::string("value11"))},
       {"key111", createThriftValue(1, "node1", std::string("value111"))},
       {"key2", createThriftValue(1, "node1", std::string("value2"))},
       {"key22", createThriftValue(1, "node1", std::string("value22"))},
       {"key222", createThriftValue(1, "node1", std::string("value222"))},
       {"key3", createThriftValue(1, "node3", std::string("value3"))},
       {"key33", createThriftValue(1, "node33", std::string("value33"))},
       {"key333", createThriftValue(1, "node33", std::string("value333"))}});

  thrift::KeyVals keyValsPod;
  keyValsPod["keyPod1"] =
      createThriftValue(1, "node1", std::string("valuePod1"));
  keyValsPod["keyPod2"] =
      createThriftValue(1, "node1", std::string("valuePod2"));

  thrift::KeyVals keyValsPlane;
  keyValsPlane["keyPlane1"] =
      createThriftValue(1, "node1", std::string("valuePlane1"));
  keyValsPlane["keyPlane2"] =
      createThriftValue(1, "node1", std::string("valuePlane2"));

  //
  // area list get
  //
  {
    auto config = handler_->semifuture_getRunningConfigThrift().get();
    std::unordered_set<std::string> areas;
    for (auto const& area : *config->areas()) {
      areas.insert(*area.area_id());
    }
    EXPECT_THAT(areas, testing::SizeIs(3));
    EXPECT_THAT(
        areas,
        testing::UnorderedElementsAre(kPodAreaId, kPlaneAreaId, kSpineAreaId));
  }

  // Key set/get
  // clang-format off
  {
    {
      auto result = co_await co_setKvStoreKeyValues(kvs, kSpineAreaId);
      EXPECT_EQ(result->noMergeReasons()->size(), 0);
    }

    {
      auto result = co_await co_setKvStoreKeyValues(keyValsPod, kPodAreaId);
      EXPECT_EQ(result->noMergeReasons()->size(), 0);
    }
    {
      auto result = co_await co_setKvStoreKeyValues(keyValsPlane, kPlaneAreaId);
      EXPECT_EQ(result->noMergeReasons()->size(), 0);
    }
  }
  {
    std::vector<std::string> filterKeys{"key11", "key2"};
    auto pub = co_await handler_->co_getKvStoreKeyValsArea(
        std::make_unique<std::vector<std::string>>(std::move(filterKeys)),
        std::make_unique<std::string>(kSpineAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(kvs.at("key2"), keyVals["key2"]);
    EXPECT_EQ(kvs.at("key11"), keyVals["key11"]);
  }

  // pod keys
  {
    std::vector<std::string> filterKeys{"keyPod1"};
    auto pub = co_await handler_->co_getKvStoreKeyValsArea(
        std::make_unique<std::vector<std::string>>(std::move(filterKeys)),
        std::make_unique<std::string>(kPodAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(1, keyVals.size());
    EXPECT_EQ(keyValsPod.at("keyPod1"), keyVals["keyPod1"]);
  }
  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node3");
    params.keys() = {"key3"};

    auto pub = co_await handler_->co_getKvStoreKeyValsFilteredArea(
        std::make_unique<thrift::KeyDumpParams>(std::move(params)),
        std::make_unique<std::string>(kSpineAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(3, keyVals.size());
    EXPECT_EQ(kvs.at("key3"), keyVals["key3"]);
    EXPECT_EQ(kvs.at("key33"), keyVals["key33"]);
    EXPECT_EQ(kvs.at("key333"), keyVals["key333"]);
  }

  // with areas
  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node1");
    params.keys() = {"keyP"};

    auto pub = co_await handler_->co_getKvStoreKeyValsFilteredArea(
        std::make_unique<thrift::KeyDumpParams>(std::move(params)),
        std::make_unique<std::string>(kPlaneAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(keyValsPlane.at("keyPlane1"), keyVals["keyPlane1"]);
    EXPECT_EQ(keyValsPlane.at("keyPlane2"), keyVals["keyPlane2"]);
  }
  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node3");
    params.keys() = {"key3"};

    auto pub =
        co_await handler_
            ->co_getKvStoreHashFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(3, keyVals.size());
    auto value3 = kvs.at("key3");
    value3.value().reset();
    auto value33 = kvs.at("key33");
    value33.value().reset();
    auto value333 = kvs.at("key333");
    value333.value().reset();
    EXPECT_EQ(value3, keyVals["key3"]);
    EXPECT_EQ(value33, keyVals["key33"]);
    EXPECT_EQ(value333, keyVals["key333"]);
  }

  //
  // getKvStoreAreaSummary() related
  //
  {
    std::set<std::string> areaSetAll{
        kPodAreaId, kPlaneAreaId, kSpineAreaId, kTestingAreaName};
    std::map<std::string, int> areaKVCountMap{};

    // get summary from KvStore for all configured areas (one extra
    // non-existent area is provided)
    auto summary =
        co_await handler_
            ->co_getKvStoreAreaSummary(
                std::make_unique<std::set<std::string>>(std::move(areaSetAll)));
    EXPECT_THAT(*summary, testing::SizeIs(3));

    // map each area to the # of keyVals in each area
    areaKVCountMap[*summary->at(0).area()] = *summary->at(0).keyValsCount();
    areaKVCountMap[*summary->at(1).area()] = *summary->at(1).keyValsCount();
    areaKVCountMap[*summary->at(2).area()] = *summary->at(2).keyValsCount();
    // test # of keyVals for each area, as per config above.
    // area names are being implicitly tested as well
    EXPECT_EQ(9, areaKVCountMap[kSpineAreaId]);
    EXPECT_EQ(2, areaKVCountMap[kPodAreaId]);
    EXPECT_EQ(2, areaKVCountMap[kPlaneAreaId]);
  }

  //
  // Peers APIs
  //
  const thrift::PeersMap peers{
      {"peer1", createPeerSpec(Constants::kPlatformHost.toString())},
      {"peer2", createPeerSpec(Constants::kPlatformHost.toString())},
      {"peer3", createPeerSpec(Constants::kPlatformHost.toString())}};

  // do the same with non-default area
  const thrift::PeersMap peersPod{
      {"peer11", createPeerSpec(Constants::kPlatformHost.toString())},
      {"peer21", createPeerSpec(Constants::kPlatformHost.toString())},
  };

  {
    for (auto& peer : peers) {
      kvStoreWrapper_->addPeer(kSpineAreaId, peer.first, peer.second);
    }
    for (auto& peerPod : peersPod) {
      kvStoreWrapper_->addPeer(kPodAreaId, peerPod.first, peerPod.second);
    }

    auto ret = co_await handler_
                   ->co_getKvStorePeersArea(
                       std::make_unique<std::string>(kSpineAreaId));

    EXPECT_EQ(3, ret->size());
    EXPECT_TRUE(ret->count("peer1"));
    EXPECT_TRUE(ret->count("peer2"));
    EXPECT_TRUE(ret->count("peer3"));
  }

  {
    kvStoreWrapper_->delPeer(kSpineAreaId, "peer2");

    auto ret = co_await handler_
                   ->co_getKvStorePeersArea(
                       std::make_unique<std::string>(kSpineAreaId));
    EXPECT_EQ(2, ret->size());
    EXPECT_TRUE(ret->count("peer1"));
    EXPECT_TRUE(ret->count("peer3"));
  }

  {
    auto ret = co_await handler_
                   ->co_getKvStorePeersArea(
                       std::make_unique<std::string>(kPodAreaId));

    EXPECT_EQ(2, ret->size());
    EXPECT_TRUE(ret->count("peer11"));
    EXPECT_TRUE(ret->count("peer21"));
  }

  {
    kvStoreWrapper_->delPeer(kPodAreaId, "peer21");

    auto ret = co_await handler_
                   ->co_getKvStorePeersArea(
                       std::make_unique<std::string>(kPodAreaId));
    EXPECT_EQ(1, ret->size());
    EXPECT_TRUE(ret->count("peer11"));
  }

  // Not using params.prefix. Instead using keys. params.prefix will be
  // deprecated soon. There are three sub-tests with different prefix
  // key values.
  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node3");
    params.keys() = {"key3"};

    auto pub =
        co_await handler_
            ->co_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(3, keyVals.size());
    EXPECT_EQ(kvs.at("key3"), keyVals["key3"]);
    EXPECT_EQ(kvs.at("key33"), keyVals["key33"]);
    EXPECT_EQ(kvs.at("key333"), keyVals["key333"]);
  }

  {
    thrift::KeyDumpParams params;
    params.originatorIds() = {"node33"};
    params.keys() = {"key33"};

    auto pub =
        co_await handler_
            ->co_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(kvs.at("key33"), keyVals["key33"]);
    EXPECT_EQ(kvs.at("key333"), keyVals["key333"]);
  }

  {
    // Two updates because the operator is OR and originator ids for keys
    // key33 and key333 are same.
    thrift::KeyDumpParams params;
    params.originatorIds() = {"node33"};
    params.keys() = {"key333"};
    auto pub =
        co_await handler_
            ->co_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(kvs.at("key33"), keyVals["key33"]);
    EXPECT_EQ(kvs.at("key333"), keyVals["key333"]);
  }

  // with areas but do not use prefix (to be deprecated). use prefixes/keys
  // instead.
  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node1");
    params.keys() = {"keyP", "keyPl"};

    auto pub =
        co_await handler_
            ->co_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kPlaneAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(keyValsPlane.at("keyPlane1"), keyVals["keyPlane1"]);
    EXPECT_EQ(keyValsPlane.at("keyPlane2"), keyVals["keyPlane2"]);
  }

  // Operator is OR and params.prefix is empty.
  // Use HashFiltered
  {
    thrift::KeyDumpParams params;
    params.originatorIds() = {"node3"};
    params.keys() = {"key3"};

    auto pub =
        co_await handler_
            ->co_getKvStoreHashFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId));
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(3, keyVals.size());
    auto value3 = kvs.at("key3");
    value3.value().reset();
    auto value33 = kvs.at("key33");
    value33.value().reset();
    auto value333 = kvs.at("key333");
    value333.value().reset();
    EXPECT_EQ(value3, keyVals["key3"]);
    EXPECT_EQ(value33, keyVals["key33"]);
    EXPECT_EQ(value333, keyVals["key333"]);
  }
  // clang-format on
}
#endif // FOLLY_HAS_COROUTINES

TEST_F(OpenrCtrlFixture, KvStoreApis) {
  thrift::KeyVals kvs(
      {{"key1", createThriftValue(1, "node1", std::string("value1"))},
       {"key11", createThriftValue(1, "node1", std::string("value11"))},
       {"key111", createThriftValue(1, "node1", std::string("value111"))},
       {"key2", createThriftValue(1, "node1", std::string("value2"))},
       {"key22", createThriftValue(1, "node1", std::string("value22"))},
       {"key222", createThriftValue(1, "node1", std::string("value222"))},
       {"key3", createThriftValue(1, "node3", std::string("value3"))},
       {"key33", createThriftValue(1, "node33", std::string("value33"))},
       {"key333", createThriftValue(1, "node33", std::string("value333"))}});

  thrift::KeyVals keyValsPod;
  keyValsPod["keyPod1"] =
      createThriftValue(1, "node1", std::string("valuePod1"));
  keyValsPod["keyPod2"] =
      createThriftValue(1, "node1", std::string("valuePod2"));

  thrift::KeyVals keyValsPlane;
  keyValsPlane["keyPlane1"] =
      createThriftValue(1, "node1", std::string("valuePlane1"));
  keyValsPlane["keyPlane2"] =
      createThriftValue(1, "node1", std::string("valuePlane2"));

  //
  // area list get
  //
  {
    auto config = handler_->semifuture_getRunningConfigThrift().get();
    std::unordered_set<std::string> areas;
    for (auto const& area : *config->areas()) {
      areas.insert(*area.area_id());
    }
    EXPECT_THAT(areas, testing::SizeIs(3));
    EXPECT_THAT(
        areas,
        testing::UnorderedElementsAre(kPodAreaId, kPlaneAreaId, kSpineAreaId));
  }

  // Key set/get
  {
    setKvStoreKeyVals(kvs, kSpineAreaId);
    setKvStoreKeyVals(keyValsPod, kPodAreaId);
    setKvStoreKeyVals(keyValsPlane, kPlaneAreaId);
  }

  {
    std::vector<std::string> filterKeys{"key11", "key2"};
    auto pub = handler_
                   ->semifuture_getKvStoreKeyValsArea(
                       std::make_unique<std::vector<std::string>>(
                           std::move(filterKeys)),
                       std::make_unique<std::string>(kSpineAreaId))
                   .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(kvs.at("key2"), keyVals["key2"]);
    EXPECT_EQ(kvs.at("key11"), keyVals["key11"]);
  }

  // pod keys
  {
    std::vector<std::string> filterKeys{"keyPod1"};
    auto pub = handler_
                   ->semifuture_getKvStoreKeyValsArea(
                       std::make_unique<std::vector<std::string>>(
                           std::move(filterKeys)),
                       std::make_unique<std::string>(kPodAreaId))
                   .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(1, keyVals.size());
    EXPECT_EQ(keyValsPod.at("keyPod1"), keyVals["keyPod1"]);
  }

  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node3");
    params.keys() = {"key3"};

    auto pub =
        handler_
            ->semifuture_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId))
            .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(3, keyVals.size());
    EXPECT_EQ(kvs.at("key3"), keyVals["key3"]);
    EXPECT_EQ(kvs.at("key33"), keyVals["key33"]);
    EXPECT_EQ(kvs.at("key333"), keyVals["key333"]);
  }

  // with areas
  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node1");
    params.keys() = {"keyP"};

    auto pub =
        handler_
            ->semifuture_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kPlaneAreaId))
            .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(keyValsPlane.at("keyPlane1"), keyVals["keyPlane1"]);
    EXPECT_EQ(keyValsPlane.at("keyPlane2"), keyVals["keyPlane2"]);
  }

  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node3");
    params.keys() = {"key3"};

    auto pub =
        handler_
            ->semifuture_getKvStoreHashFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId))
            .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(3, keyVals.size());
    auto value3 = kvs.at("key3");
    value3.value().reset();
    auto value33 = kvs.at("key33");
    value33.value().reset();
    auto value333 = kvs.at("key333");
    value333.value().reset();
    EXPECT_EQ(value3, keyVals["key3"]);
    EXPECT_EQ(value33, keyVals["key33"]);
    EXPECT_EQ(value333, keyVals["key333"]);
  }

  //
  // getKvStoreAreaSummary() related
  //
  {
    std::set<std::string> areaSetAll{
        kPodAreaId, kPlaneAreaId, kSpineAreaId, kTestingAreaName};
    std::map<std::string, int> areaKVCountMap{};

    // get summary from KvStore for all configured areas (one extra
    // non-existent area is provided)
    auto summary =
        handler_
            ->semifuture_getKvStoreAreaSummary(
                std::make_unique<std::set<std::string>>(std::move(areaSetAll)))
            .get();
    EXPECT_THAT(*summary, testing::SizeIs(3));

    // map each area to the # of keyVals in each area
    areaKVCountMap[*summary->at(0).area()] = *summary->at(0).keyValsCount();
    areaKVCountMap[*summary->at(1).area()] = *summary->at(1).keyValsCount();
    areaKVCountMap[*summary->at(2).area()] = *summary->at(2).keyValsCount();
    // test # of keyVals for each area, as per config above.
    // area names are being implicitly tested as well
    EXPECT_EQ(9, areaKVCountMap[kSpineAreaId]);
    EXPECT_EQ(2, areaKVCountMap[kPodAreaId]);
    EXPECT_EQ(2, areaKVCountMap[kPlaneAreaId]);
  }

  //
  // Peers APIs
  //
  const thrift::PeersMap peers{
      {"peer1", createPeerSpec(Constants::kPlatformHost.toString())},
      {"peer2", createPeerSpec(Constants::kPlatformHost.toString())},
      {"peer3", createPeerSpec(Constants::kPlatformHost.toString())}};

  // do the same with non-default area
  const thrift::PeersMap peersPod{
      {"peer11", createPeerSpec(Constants::kPlatformHost.toString())},
      {"peer21", createPeerSpec(Constants::kPlatformHost.toString())},
  };

  {
    for (auto& peer : peers) {
      kvStoreWrapper_->addPeer(kSpineAreaId, peer.first, peer.second);
    }
    for (auto& peerPod : peersPod) {
      kvStoreWrapper_->addPeer(kPodAreaId, peerPod.first, peerPod.second);
    }

    auto ret = handler_
                   ->semifuture_getKvStorePeersArea(
                       std::make_unique<std::string>(kSpineAreaId))
                   .get();

    EXPECT_EQ(3, ret->size());
    EXPECT_TRUE(ret->count("peer1"));
    EXPECT_TRUE(ret->count("peer2"));
    EXPECT_TRUE(ret->count("peer3"));
  }

  {
    kvStoreWrapper_->delPeer(kSpineAreaId, "peer2");

    auto ret = handler_
                   ->semifuture_getKvStorePeersArea(
                       std::make_unique<std::string>(kSpineAreaId))
                   .get();
    EXPECT_EQ(2, ret->size());
    EXPECT_TRUE(ret->count("peer1"));
    EXPECT_TRUE(ret->count("peer3"));
  }

  {
    auto ret = handler_
                   ->semifuture_getKvStorePeersArea(
                       std::make_unique<std::string>(kPodAreaId))
                   .get();

    EXPECT_EQ(2, ret->size());
    EXPECT_TRUE(ret->count("peer11"));
    EXPECT_TRUE(ret->count("peer21"));
  }

  {
    kvStoreWrapper_->delPeer(kPodAreaId, "peer21");

    auto ret = handler_
                   ->semifuture_getKvStorePeersArea(
                       std::make_unique<std::string>(kPodAreaId))
                   .get();
    EXPECT_EQ(1, ret->size());
    EXPECT_TRUE(ret->count("peer11"));
  }

  // Not using params.prefix. Instead using keys. params.prefix will be
  // deprecated soon. There are three sub-tests with different prefix
  // key values.
  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node3");
    params.keys() = {"key3"};

    auto pub =
        handler_
            ->semifuture_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId))
            .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(3, keyVals.size());
    EXPECT_EQ(kvs.at("key3"), keyVals["key3"]);
    EXPECT_EQ(kvs.at("key33"), keyVals["key33"]);
    EXPECT_EQ(kvs.at("key333"), keyVals["key333"]);
  }

  {
    thrift::KeyDumpParams params;
    params.originatorIds() = {"node33"};
    params.keys() = {"key33"};

    auto pub =
        handler_
            ->semifuture_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId))
            .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(kvs.at("key33"), keyVals["key33"]);
    EXPECT_EQ(kvs.at("key333"), keyVals["key333"]);
  }

  {
    // Two updates because the operator is OR and originator ids for keys
    // key33 and key333 are same.
    thrift::KeyDumpParams params;
    params.originatorIds() = {"node33"};
    params.keys() = {"key333"};
    auto pub =
        handler_
            ->semifuture_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId))
            .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(kvs.at("key33"), keyVals["key33"]);
    EXPECT_EQ(kvs.at("key333"), keyVals["key333"]);
  }

  // with areas but do not use prefix (to be deprecated). use prefixes/keys
  // instead.
  {
    thrift::KeyDumpParams params;
    params.originatorIds()->insert("node1");
    params.keys() = {"keyP", "keyPl"};

    auto pub =
        handler_
            ->semifuture_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kPlaneAreaId))
            .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(2, keyVals.size());
    EXPECT_EQ(keyValsPlane.at("keyPlane1"), keyVals["keyPlane1"]);
    EXPECT_EQ(keyValsPlane.at("keyPlane2"), keyVals["keyPlane2"]);
  }

  // Operator is OR and params.prefix is empty.
  // Use HashFiltered
  {
    thrift::KeyDumpParams params;
    params.originatorIds() = {"node3"};
    params.keys() = {"key3"};

    auto pub =
        handler_
            ->semifuture_getKvStoreHashFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kSpineAreaId))
            .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(3, keyVals.size());
    auto value3 = kvs.at("key3");
    value3.value().reset();
    auto value33 = kvs.at("key33");
    value33.value().reset();
    auto value333 = kvs.at("key333");
    value333.value().reset();
    EXPECT_EQ(value3, keyVals["key3"]);
    EXPECT_EQ(value33, keyVals["key33"]);
    EXPECT_EQ(value333, keyVals["key333"]);
  }
}

TEST_F(OpenrCtrlFixture, subscribeAndGetKvStoreFilteredWithKeysNoTtlUpdate) {
  thrift::KeyVals kvs({
      {"key1", createThriftValue(1, "node1", std::string("value1"), 30000, 1)},
      {"key11",
       createThriftValue(1, "node1", std::string("value11"), 30000, 1)},
      {"key111",
       createThriftValue(1, "node1", std::string("value11"), 30000, 1)},
      {"key2", createThriftValue(1, "node1", std::string("value2"), 30000, 1)},
      {"key22",
       createThriftValue(1, "node1", std::string("value22"), 30000, 1)},
      {"key222",
       createThriftValue(1, "node1", std::string("value222"), 30000, 1)},
      {"key3", createThriftValue(1, "node3", std::string("value3"), 30000, 1)},
      {"key33",
       createThriftValue(1, "node33", std::string("value33"), 30000, 1)},
      {"key333",
       createThriftValue(1, "node33", std::string("value333"), 30000, 1)},
  });

  // Key set
  setKvStoreKeyVals(std::move(kvs), kSpineAreaId);

  //
  // Subscribe and Get API
  //
  {
    // Add more keys and values
    const std::string key{"snoop-key"};
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(1, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(1, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(2, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(3, "node1", std::string("value1")));

    std::vector<std::string> filterKeys{key};
    auto pub = handler_
                   ->semifuture_getKvStoreKeyValsArea(
                       std::make_unique<std::vector<std::string>>(
                           std::move(filterKeys)),
                       std::make_unique<std::string>(kSpineAreaId))
                   .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(1, keyVals.size());
    EXPECT_EQ(3, *(keyVals.at(key).version()));
    EXPECT_EQ("value1", keyVals.at(key).value().value());
  }

  {
    const std::string key{"snoop-key"};
    std::atomic<int> received{0}, diff{0};
    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();
    // Expect 10 keys in the initial dump
    // NOTE: there may be extra keys from PrefixManager & LinkMonitor)
    EXPECT_LE(
        10, (*responseAndSubscription.response.begin()->keyVals()).size());
    ASSERT_EQ(
        1, (*responseAndSubscription.response.begin()->keyVals()).count(key));
    EXPECT_EQ(
        responseAndSubscription.response.begin()->keyVals()->at(key),
        createThriftValue(3, "node1", std::string("value1")));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, &diff, key](auto&& t) {
                  // Consider publication only if `key` is present
                  // NOTE: There can be updates to prefix or adj keys
                  if (!t.hasValue() || !t->keyVals()->count(key)) {
                    return;
                  }
                  auto& pub = *t;
                  EXPECT_EQ(1, (*pub.keyVals()).size());
                  ASSERT_EQ(1, (*pub.keyVals()).count(key));
                  EXPECT_EQ("value1", (*pub.keyVals()).at(key).value().value());

                  /**
                   * Expect incremental version for every received update
                   * and hence diff between them should always be constant
                   */
                  if (!received) {
                    diff = received - *(*pub.keyVals()).at(key).version();
                  }
                  EXPECT_EQ(
                      diff, received - *(*pub.keyVals()).at(key).version());
                  received++;
                });
    EXPECT_EQ(1, handler_->getNumKvStorePublishers());
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(5, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(6, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kPodAreaId,
        key,
        createThriftValue(7, "node1", std::string("value1")),
        std::nullopt);
    kvStoreWrapper_->setKey(
        kPlaneAreaId,
        key,
        createThriftValue(8, "node1", std::string("value1")),
        std::nullopt);

    // Check we should receive 3 updates in kSpineAreaId
    while (received < 3) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // Subscribe and Get API
  // No entry is found in the initial shapshot
  // Matching prefixes get injected later.
  // AND operator is used. There are two clients for kv store updates.
  {
    std::atomic<int> received{0};
    const std::string key{"key4"};
    const std::string random_key{"random_key"};
    std::vector<std::string> keys = {key, random_key};
    thrift::KeyDumpParams filter;
    filter.keys() = keys;
    filter.originatorIds() = {"node1", "node2", "node3", "node33"};
    filter.oper() = thrift::FilterOperator::AND;

    auto handler_other = handler_;
    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    auto responseAndSubscription_other =
        handler_other
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    /* key4 and random_key don't exist already */
    EXPECT_LE(0, (*responseAndSubscription.response.begin()->keyVals()).size());
    ASSERT_EQ(
        0, (*responseAndSubscription.response.begin()->keyVals()).count(key));
    EXPECT_LE(
        0, (*responseAndSubscription_other.response.begin()->keyVals()).size());
    ASSERT_EQ(
        0,
        (*responseAndSubscription_other.response.begin()->keyVals())
            .count(key));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() || !t->keyVals()->count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals()).size());
              ASSERT_EQ(1, (*pub.keyVals()).count(key));
              EXPECT_EQ("value4", (*pub.keyVals()).at(key).value().value());
              received++;
            });

    auto subscription_other =
        std::move(responseAndSubscription_other.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, random_key](auto&& t) {
                  if (!t.hasValue() || !t->keyVals()->count(random_key)) {
                    return;
                  }
                  auto& pub = *t;
                  EXPECT_EQ(1, (*pub.keyVals()).size());
                  ASSERT_EQ(1, (*pub.keyVals()).count(random_key));
                  EXPECT_EQ(
                      "value_random",
                      (*pub.keyVals()).at(random_key).value().value());
                  received++;
                });

    /* There are two clients */
    EXPECT_EQ(2, handler_->getNumKvStorePublishers());
    EXPECT_EQ(2, handler_other->getNumKvStorePublishers());

    /* key4 and random_prefix keys are getting added for the first time */
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(1, "node1", std::string("value4")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        random_key,
        createThriftValue(1, "node1", std::string("value_random")));

    // Check we should receive 2 updates
    while (received < 2) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    subscription_other.cancel();
    std::move(subscription_other).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // Subscribe and Get API
  // Initial kv store snapshot has matching entries
  // More Matching prefixes get injected later.
  // AND operator is used in the filter.
  {
    std::atomic<int> received{0};
    const std::string key{"key333"};
    thrift::KeyDumpParams filter;
    filter.keys() = {"key33"};
    filter.originatorIds() = {"node1", "node2", "node3", "node33"};
    filter.oper() = thrift::FilterOperator::AND;

    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    /* prefix key is key33. kv store has key33 and key333 */
    EXPECT_LE(2, responseAndSubscription.response.begin()->keyVals()->size());
    ASSERT_EQ(
        1, responseAndSubscription.response.begin()->keyVals()->count(key));
    ASSERT_EQ(
        1,
        responseAndSubscription.response.begin()->keyVals()->count("key333"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() || !t->keyVals()->count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals()).size());
              ASSERT_EQ(1, (*pub.keyVals()).count(key));
              // Validates value is set with KeyDumpParams.doNotPublishValue =
              // false
              EXPECT_EQ("value333", (*pub.keyVals()).at(key).value().value());
              received++;
            });

    EXPECT_EQ(1, handler_->getNumKvStorePublishers());
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(2, "node33", std::string("value333")));

    // Check we should receive-1 updates
    while (received < 1) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // Subscribe and Get API
  // Initial kv store snapshot has matching entries
  // More Matching prefixes get injected later.
  // Prefix is a regex and operator is OR.
  {
    std::atomic<int> received{0};
    const std::string key{"key33.*"};
    std::vector<std::string> keys = {"key33.*"};
    thrift::KeyDumpParams filter;
    filter.keys() = keys;
    filter.originatorIds() = {"node1", "node2", "node3", "node33"};
    filter.oper() = thrift::FilterOperator::OR;

    std::unordered_map<std::string, std::string> keyvals;
    keyvals["key33"] = "value33";
    keyvals["key333"] = "value333";

    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(2, (*responseAndSubscription.response.begin()->keyVals()).size());
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals()).count("key33"));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals()).count("key333"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (!t.hasValue()) {
                    return;
                  }

                  for (const auto& kv : keyvals) {
                    if (!t->keyVals()->count(kv.first)) {
                      continue;
                    }
                    auto& pub = *t;
                    EXPECT_EQ(1, (*pub.keyVals()).size());
                    ASSERT_EQ(1, (*pub.keyVals()).count(kv.first));
                    EXPECT_EQ(
                        kv.second,
                        (*pub.keyVals()).at(kv.first).value().value());
                    received++;
                  }
                });

    EXPECT_EQ(1, handler_->getNumKvStorePublishers());
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key333",
        createThriftValue(3, "node33", std::string("value333")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key33",
        createThriftValue(3, "node33", std::string("value33")));

    // Check we should receive 2 updates
    while (received < 2) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // Subscribe and Get API
  // Multiple matching keys
  // AND operator is used
  {
    std::atomic<int> received{0};
    const std::string key{"test-key"};
    std::vector<std::string> keys = {"key1", key, "key3"};
    std::unordered_map<std::string, std::string> keyvals;
    keyvals["key1"] = "value1";
    keyvals["key3"] = "value3";
    keyvals[key] = "value1";

    thrift::KeyDumpParams filter;
    filter.keys() = keys;
    filter.originatorIds() = {"node1", "node2", "node3", "node33"};
    filter.oper() = thrift::FilterOperator::AND;

    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(3, (*responseAndSubscription.response.begin()->keyVals()).size());
    EXPECT_EQ(
        0, (*responseAndSubscription.response.begin()->keyVals()).count(key));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals()).count("key1"));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals()).count("key3"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (!t.hasValue()) {
                    return;
                  }

                  bool found = false;
                  auto& pub = *t;
                  for (const auto& kv : keyvals) {
                    if (t->keyVals()->count(kv.first)) {
                      EXPECT_EQ(1, (*pub.keyVals()).size());
                      ASSERT_EQ(1, (*pub.keyVals()).count(kv.first));
                      EXPECT_EQ(
                          kv.second,
                          (*pub.keyVals()).at(kv.first).value().value());
                      received++;
                      found = true;
                    }
                  }
                  if (!found) {
                    return;
                  }
                });

    EXPECT_EQ(1, handler_->getNumKvStorePublishers());
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key1",
        createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key3",
        createThriftValue(4, "node3", std::string("value3")));

    // Check we should receive 3 updates
    while (received < 3) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // Subscribe and Get API
  // OR operator is used. A random-prefix is injected which matches only
  // originator-id.
  {
    std::atomic<int> received{0};
    const std::string key{"test-key"};
    std::vector<std::string> keys = {"key1", key, "key3"};
    thrift::KeyDumpParams filter;
    filter.keys() = keys;
    filter.originatorIds() = {"node1", "node2", "node3", "node33"};
    filter.oper() = thrift::FilterOperator::OR;

    std::unordered_map<std::string, std::string> keyvals;
    keyvals["key1"] = "value1";
    keyvals["key3"] = "value3";
    keyvals[key] = "value1";
    keyvals["random-prefix"] = "value1";

    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(3, (*responseAndSubscription.response.begin()->keyVals()).size());
    EXPECT_EQ(
        1, (*responseAndSubscription.response.begin()->keyVals()).count(key));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals()).count("key1"));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals()).count("key3"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (!t.hasValue()) {
                    return;
                  }

                  bool found = false;
                  auto& pub = *t;
                  for (const auto& kv : keyvals) {
                    if (t->keyVals()->count(kv.first)) {
                      EXPECT_EQ(1, (*pub.keyVals()).size());
                      ASSERT_EQ(1, (*pub.keyVals()).count(kv.first));
                      EXPECT_EQ(
                          kv.second,
                          (*pub.keyVals()).at(kv.first).value().value());
                      received++;
                      found = true;
                    }
                  }
                  if (!found) {
                    return;
                  }
                });

    EXPECT_EQ(1, handler_->getNumKvStorePublishers());
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(5, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key1",
        createThriftValue(5, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key3",
        createThriftValue(5, "node3", std::string("value3")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "random-prefix",
        createThriftValue(1, "node1", std::string("value1")));

    // Check we should receive 4 updates
    while (received < 4) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // Subscribe and Get API
  // No matching originator id in initial snapshot
  {
    std::atomic<int> received{0};
    const std::string key{"test_key"};
    std::vector<std::string> keys = {"key1", "key2", "key3", key};
    thrift::KeyDumpParams filter;
    filter.keys() = keys;
    filter.originatorIds()->insert("node10");
    filter.oper() = thrift::FilterOperator::AND;

    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    /* The key is not in kv store */
    EXPECT_LE(0, (*responseAndSubscription.response.begin()->keyVals()).size());

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              if (!t.hasValue() || !t->keyVals()->count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals()).size());
              ASSERT_EQ(1, (*pub.keyVals()).count(key));
              EXPECT_EQ("value1", (*pub.keyVals()).at(key).value().value());
              received++;
            });

    EXPECT_EQ(1, handler_->getNumKvStorePublishers());
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(10, "node10", std::string("value1")));

    // Check we should receive 1 updates
    while (received < 1) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // Subscribe and Get API
  // No matching originator id
  // Operator OR is used. Matching is based on prefix keys only
  {
    std::atomic<int> received{0};
    const std::string key{"test_key"};
    thrift::KeyDumpParams filter;
    filter.keys() = {"key1", "key2", "key3", key};
    filter.originatorIds()->insert("node10");
    filter.oper() = thrift::FilterOperator::OR;

    std::unordered_map<std::string, std::string> keyvals;
    keyvals["key1"] = "value1";
    keyvals["key2"] = "value2";
    keyvals["key3"] = "value3";
    keyvals[key] = "value1";
    keyvals["random-prefix-2"] = "value1";

    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(0, (*responseAndSubscription.response.begin()->keyVals()).size());

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (!t.hasValue()) {
                    return;
                  }

                  for (const auto& kv : keyvals) {
                    if (!t->keyVals()->count(kv.first)) {
                      continue;
                    }
                    auto& pub = *t;
                    EXPECT_EQ(1, (*pub.keyVals()).size());
                    ASSERT_EQ(1, (*pub.keyVals()).count(kv.first));
                    EXPECT_EQ(
                        kv.second,
                        (*pub.keyVals()).at(kv.first).value().value());
                    received++;
                  }
                });

    EXPECT_EQ(1, handler_->getNumKvStorePublishers());
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key1",
        createThriftValue(20, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key2",
        createThriftValue(20, "node2", std::string("value2")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "key3",
        createThriftValue(20, "node3", std::string("value3")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        key,
        createThriftValue(20, "node1", std::string("value1")));
    kvStoreWrapper_->setKey(
        kSpineAreaId,
        "random-prefix-2",
        createThriftValue(20, "node1", std::string("value1")));

    // Check we should receive-4 updates
    while (received < 4) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }
}

TEST_F(
    OpenrCtrlFixture, subscribeAndGetKvStoreFilteredWithKeysTtlUpdateOption) {
  thrift::KeyVals kvs({
      {"key1", createThriftValue(1, "node1", std::string("value1"), 30000, 1)},
      {"key11",
       createThriftValue(1, "node1", std::string("value11"), 30000, 1)},
      {"key111",
       createThriftValue(1, "node1", std::string("value111"), 30000, 1)},
      {"key2", createThriftValue(1, "node1", std::string("value2"), 30000, 1)},
      {"key22",
       createThriftValue(1, "node1", std::string("value22"), 30000, 1)},
      {"key222",
       createThriftValue(1, "node1", std::string("value222"), 30000, 1)},
      {"key3", createThriftValue(1, "node3", std::string("value3"), 30000, 1)},
      {"key33",
       createThriftValue(1, "node33", std::string("value33"), 30000, 1)},
      {"key333",
       createThriftValue(1, "node33", std::string("value333"), 30000, 1)},
  });

  // Key set
  setKvStoreKeyVals(kvs, kSpineAreaId);

  // ignoreTTL = false is specified in filter.
  // Client should receive publication associated with TTL update
  {
    const std::string key{"key1"};
    const std::unordered_map<std::string, std::string> keyvals{{key, "value1"}};
    thrift::KeyDumpParams filter;
    filter.keys() = {key};
    filter.ignoreTtl() = false;
    filter.originatorIds()->insert("node1");
    filter.oper() = thrift::FilterOperator::AND;

    const auto value = createThriftValue(
        1 /* version */,
        "node1",
        "value1",
        30000 /* ttl*/,
        5 /* ttl version */,
        0 /* hash */);

    auto thriftValue = value;
    thriftValue.value().reset();
    kvStoreWrapper_->setKey(kSpineAreaId, "key1", thriftValue);

    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(filter),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(3, (*responseAndSubscription.response.begin()->keyVals()).size());
    for (const auto& key_ : {"key1", "key11", "key111"}) {
      EXPECT_EQ(
          1,
          (*responseAndSubscription.response.begin()->keyVals()).count(key_));
    }

    EXPECT_EQ(
        0,
        (*responseAndSubscription.response.begin()->keyVals()).count("key2"));
    const auto& val1 =
        (*responseAndSubscription.response.begin()->keyVals())["key1"];
    ASSERT_EQ(true, val1.value().has_value()); /* value is non-null */
    EXPECT_EQ(1, *val1.version());
    EXPECT_LT(10000, *val1.ttl());
    EXPECT_EQ(5, *val1.ttlVersion()); /* Reflects updated TTL version */

    std::atomic<bool> newTtlVersionSeen = false;
    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [keyvals, &newTtlVersionSeen](auto&& t) {
                  if (!t.hasValue()) {
                    return;
                  }

                  for (const auto& kv : keyvals) {
                    if (!t->keyVals()->count(kv.first)) {
                      continue;
                    }
                    auto& pub = *t;
                    ASSERT_EQ(1, (*pub.keyVals()).count(kv.first));
                    if ((*pub.keyVals()).count("key1") == 1) {
                      const auto& val = (*pub.keyVals())["key1"];
                      if (*val.ttlVersion() == 6) {
                        newTtlVersionSeen = true;
                        /* TTL update has no value */
                        EXPECT_EQ(false, val.value().has_value());
                        EXPECT_EQ(1, (*pub.keyVals()).size());
                      }
                    }
                  }
                });

    EXPECT_EQ(1, handler_->getNumKvStorePublishers());

    // TTL update
    auto thriftValue2 = value;
    thriftValue2.value().reset();
    thriftValue2.ttl() = 50000;
    *thriftValue2.ttlVersion() += 1;
    kvStoreWrapper_->setKey(kSpineAreaId, key, thriftValue2);

    // Wait until new TTL version is seen.
    while (!newTtlVersionSeen) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // ignoreTTL = true is specified in filter.
  // Client should not receive publication associated with TTL update
  {
    const std::string key{"key3"};
    thrift::KeyDumpParams filter;
    filter.keys() = {key};
    filter.ignoreTtl() = true;
    filter.originatorIds()->insert("node3");
    filter.originatorIds()->insert("node33");
    std::unordered_map<std::string, std::string> keyvals;
    keyvals[key] = "value3";
    filter.oper() = thrift::FilterOperator::AND;

    const auto value = createThriftValue(
        1 /* version */,
        "node3",
        "value3",
        20000 /* ttl*/,
        5 /* ttl version */,
        0 /* hash */);

    auto thriftValue = value;
    thriftValue.value().reset();
    kvStoreWrapper_->setKey(kSpineAreaId, "key3", thriftValue);
    auto responseAndSubscription =
        handler_
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(filter),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(3, (*responseAndSubscription.response.begin()->keyVals()).size());
    for (const auto& key_ : {"key3", "key33", "key333"}) {
      EXPECT_EQ(
          1,
          (*responseAndSubscription.response.begin()->keyVals()).count(key_));
    }

    EXPECT_EQ(
        0,
        (*responseAndSubscription.response.begin()->keyVals()).count("key2"));
    const auto& val1 =
        (*responseAndSubscription.response.begin()->keyVals())["key3"];
    ASSERT_EQ(true, val1.value().has_value());
    EXPECT_EQ(1, *val1.version());
    EXPECT_LT(10000, *val1.ttl());
    EXPECT_EQ(5, *val1.ttlVersion()); /* Reflects updated TTL version */

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [keyvals](auto&& t) {
              if (!t.hasValue()) {
                return;
              }

              for (const auto& kv : keyvals) {
                if (!t->keyVals()->count(kv.first)) {
                  continue;
                }
                auto& pub = *t;
                EXPECT_EQ(1, (*pub.keyVals()).size());
                ASSERT_EQ(1, (*pub.keyVals()).count(kv.first));
                if ((*pub.keyVals()).count("key3") == 1) {
                  const auto& val = (*pub.keyVals())["key3"];
                  EXPECT_LE(6, *val.ttlVersion());
                }
              }
            });

    EXPECT_EQ(1, handler_->getNumKvStorePublishers());

    // TTL update
    auto thriftValue2 = value;
    thriftValue2.value().reset();
    thriftValue2.ttl() = 30000;
    *thriftValue2.ttlVersion() += 1;
    /* No TTL update message should be received */
    kvStoreWrapper_->setKey(kSpineAreaId, key, thriftValue2);

    /* Check that the TTL version is updated */
    std::vector<std::string> filterKeys{key};
    auto pub = handler_
                   ->semifuture_getKvStoreKeyValsArea(
                       std::make_unique<std::vector<std::string>>(
                           std::move(filterKeys)),
                       std::make_unique<std::string>(kSpineAreaId))
                   .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(1, keyVals.size());
    EXPECT_EQ(1, *(keyVals.at(key).version()));
    EXPECT_EQ(true, keyVals.at(key).value().has_value());
    EXPECT_EQ(thriftValue2.ttlVersion(), keyVals.at(key).ttlVersion());

    // Check we should receive 0 update.
    std::this_thread::yield();

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler_->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }
}

// Verify that we can subscribe kvStore without value.
// We use filters exactly mimicking what is needed for kvstore monitor.
// Verify both in initial full dump and incremental updates we do not
// see value.
TEST_F(OpenrCtrlFixture, subscribeAndGetKvStoreFilteredWithoutValue) {
  thrift::KeyVals keyVals;
  keyVals["key1"] =
      createThriftValue(1, "node1", std::string("value1"), 30000, 1);
  keyVals["key2"] =
      createThriftValue(1, "node1", std::string("value2"), 30000, 1);

  // Key set
  setKvStoreKeyVals(keyVals, kSpineAreaId);

  // doNotPublishValue = true is specified in filter.
  // ignoreTTL = false is specified in filter.
  // Client should receive publication associated with TTL update
  thrift::KeyDumpParams filter;
  filter.ignoreTtl() = false;
  filter.doNotPublishValue() = true;

  auto responseAndSubscription =
      handler_
          ->semifuture_subscribeAndGetAreaKvStores(
              std::make_unique<thrift::KeyDumpParams>(filter),
              std::make_unique<std::set<std::string>>(kSpineOnlySet))
          .get();

  auto initialPub = responseAndSubscription.response.begin();
  EXPECT_EQ(2, (*initialPub->keyVals()).size());
  // Verify timestamp is set
  EXPECT_TRUE(initialPub->timestamp_ms().has_value());
  for (const auto& key_ : {"key1", "key2"}) {
    EXPECT_EQ(1, (*initialPub->keyVals()).count(key_));
    const auto& val1 = (*initialPub->keyVals())[key_];
    ASSERT_EQ(false, val1.value().has_value()); /* value is null */
    EXPECT_EQ(1, *val1.version());
    EXPECT_LT(10000, *val1.ttl());
    EXPECT_EQ(1, *val1.ttlVersion());
  }

  std::atomic<bool> newUpdateSeen = false;
  // Test key which gets updated.
  auto test_key = "key1";

  auto subscription =
      std::move(responseAndSubscription.stream)
          .toClientStreamUnsafeDoNotUse()
          .subscribeExTry(
              folly::getUnsafeMutableGlobalEventBase(),
              [&test_key, &newUpdateSeen](
                  folly::Try<openr::thrift::Publication>&& t) mutable {
                if (!t.hasValue()) {
                  return;
                }

                auto& pub = *t;
                ASSERT_EQ(1, (*pub.keyVals()).count(test_key));
                const auto& val = (*pub.keyVals())[test_key];
                if (*val.ttlVersion() < 2) {
                  // Ignore this version since it is NOT the update
                  // the subscriber interested in
                  return;
                }

                newUpdateSeen = true;
                // Verify no value seen in update
                ASSERT_EQ(false, val.value().has_value());
                // Verify ttl timestamp
                EXPECT_LE(30000, *val.ttl());
                EXPECT_GE(50000, *val.ttl());
                // Verify timestamp is set
                EXPECT_TRUE(pub.timestamp_ms().has_value());
              });

  EXPECT_EQ(1, handler_->getNumKvStorePublishers());

  // Update value and publish to verify incremental update also filters value
  auto thriftValue2 = keyVals[test_key];
  thriftValue2.value() = "value_updated";
  thriftValue2.ttl() = 50000;
  *thriftValue2.ttlVersion() += 1;
  kvStoreWrapper_->setKey(kSpineAreaId, test_key, thriftValue2);

  // Wait until new update is seen by stream subscriber
  while (!newUpdateSeen) {
    std::this_thread::yield();
  }

  // Cancel subscription
  subscription.cancel();
  std::move(subscription).detach();

  // Wait until publisher is destroyed
  while (handler_->getNumKvStorePublishers() != 0) {
    std::this_thread::yield();
  }
}

TEST_F(OpenrCtrlFixture, LinkMonitorApis) {
  // create an interface
  auto nlEventsInjector =
      std::make_shared<NetlinkEventsInjector>(nlSock_.get());

  nlEventsInjector->sendLinkEvent("po1011", 100, true);
  const std::string ifName = "po1011";
  const std::string adjName = "night@king";

  {
    handler_->semifuture_setNodeOverload().get();
    handler_->semifuture_unsetNodeOverload().get();
  }

  {
    handler_
        ->semifuture_setInterfaceOverload(std::make_unique<std::string>(ifName))
        .get();
    handler_
        ->semifuture_unsetInterfaceOverload(
            std::make_unique<std::string>(ifName))
        .get();
  }

  {
    handler_
        ->semifuture_setInterfaceMetric(
            std::make_unique<std::string>(ifName), 110)
        .get();
    handler_
        ->semifuture_unsetInterfaceMetric(std::make_unique<std::string>(ifName))
        .get();
  }

  {
    handler_
        ->semifuture_setAdjacencyMetric(
            std::make_unique<std::string>(ifName),
            std::make_unique<std::string>(adjName),
            110)
        .get();
    handler_
        ->semifuture_unsetAdjacencyMetric(
            std::make_unique<std::string>(ifName),
            std::make_unique<std::string>(adjName))
        .get();
  }

  {
    handler_->semifuture_setNodeInterfaceMetricIncrement(10).get();
    handler_->semifuture_unsetNodeInterfaceMetricIncrement().get();
  }

  {
    handler_
        ->semifuture_setInterfaceMetricIncrement(
            std::make_unique<std::string>(ifName), 10)
        .get();
    handler_
        ->semifuture_unsetInterfaceMetricIncrement(
            std::make_unique<std::string>(ifName))
        .get();
  }

  {
    auto reply = handler_->semifuture_getInterfaces().get();
    EXPECT_EQ(nodeName_, *reply->thisNodeName());
    EXPECT_FALSE(*reply->isOverloaded());
    EXPECT_EQ(1, reply->interfaceDetails()->size());
  }

  {
    auto ret = handler_->semifuture_getOpenrVersion().get();
    EXPECT_LE(*(ret->lowestSupportedVersion()), *(ret->version()));
  }

  {
    auto info = handler_->semifuture_getBuildInfo().get();
    EXPECT_NE("", *(info->buildMode()));
  }

  {
    thrift::AdjacenciesFilter filter;
    filter.selectAreas() = {kSpineAreaId};
    auto adjDbs =
        handler_
            ->semifuture_getLinkMonitorAdjacenciesFiltered(
                std::make_unique<thrift::AdjacenciesFilter>(std::move(filter)))
            .get();
    EXPECT_EQ(0, adjDbs->begin()->adjacencies()->size());
  }

  {
    thrift::AdjacenciesFilter filter;
    filter.selectAreas() = {kSpineAreaId};
    auto adjDbs =
        handler_
            ->semifuture_getLinkMonitorAreaAdjacenciesFiltered(
                std::make_unique<thrift::AdjacenciesFilter>(std::move(filter)))
            .get();
    EXPECT_EQ(
        0, adjDbs->operator[](kSpineAreaId).begin()->adjacencies()->size());
  }
}

TEST_F(OpenrCtrlFixture, PersistentStoreApis) {
  {
    handler_
        ->semifuture_setConfigKey(
            std::make_unique<std::string>("key1"),
            std::make_unique<std::string>("value1"))
        .get();
  }

  {
    handler_
        ->semifuture_setConfigKey(
            std::make_unique<std::string>("key2"),
            std::make_unique<std::string>("value2"))
        .get();
  }

  {
    handler_->semifuture_eraseConfigKey(std::make_unique<std::string>("key1"))
        .get();
  }

  {
    auto ret =
        handler_->semifuture_getConfigKey(std::make_unique<std::string>("key2"))
            .get();
    EXPECT_EQ("value2", *ret);
  }

  {
    EXPECT_THROW(
        handler_->semifuture_getConfigKey(std::make_unique<std::string>("key1"))
            .get(),
        thrift::OpenrError);
  }
}

TEST_F(OpenrCtrlFixture, RibPolicy) {
  // Set API
  {
    // Create valid rib policy
    thrift::RibRouteActionWeight actionWeight;
    actionWeight.area_to_weight()->emplace("test-area", 2);
    actionWeight.neighbor_to_weight()->emplace("nbr", 3);
    thrift::RibPolicyStatement policyStatement;
    policyStatement.matcher()->prefixes() = std::vector<thrift::IpPrefix>();
    policyStatement.action()->set_weight() = actionWeight;
    thrift::RibPolicy policy;
    policy.statements()->emplace_back(policyStatement);
    policy.ttl_secs() = 1;

    EXPECT_NO_THROW(handler_
                        ->semifuture_setRibPolicy(
                            std::make_unique<thrift::RibPolicy>(policy))
                        .get());
  }

  // Get API
  {
    EXPECT_NO_THROW(handler_->semifuture_getRibPolicy().get());
  }

  // Clear API
  {
    // Clear Rib Policy and expect no error as rib policy exists
    EXPECT_NO_THROW(handler_->semifuture_clearRibPolicy().get());

    // An attempt to clear non-existing rib policy will show a message.
    EXPECT_THROW(
        handler_->semifuture_clearRibPolicy().get(), thrift::OpenrError);
  }

  // Using Get API after clearing rib policy will show a message.
  {
    EXPECT_THROW(handler_->semifuture_getRibPolicy().get(), thrift::OpenrError);
  }
}

TEST_F(OpenrCtrlFixture, PersistSelfOriginatedKeyApi) {
  const std::string key1 = "persist-key1";
  const std::string value1 = "persist-value1";
  const std::string updatedValue = "persist-value-updated";

  // Test 1: Persist a new key-value pair
  {
    thrift::KeyVals keyVals;
    keyVals[key1] = createThriftValue(0, nodeName_, value1);

    thrift::KeySetParams params;
    params.keyVals() = keyVals;

    handler_
        ->semifuture_persistSelfOriginatedKey(
            std::make_unique<thrift::KeySetParams>(params),
            std::make_unique<std::string>(kSpineAreaId))
        .get();

    folly::EventBase evb;
    evb.scheduleAt(
        [this, &key1, &value1]() {
          std::vector<std::string> filterKeys{key1};
          auto pub = handler_
                         ->semifuture_getKvStoreKeyValsArea(
                             std::make_unique<std::vector<std::string>>(
                                 std::move(filterKeys)),
                             std::make_unique<std::string>(kSpineAreaId))
                         .get();
          auto keyVals_read = *pub->keyVals();
          EXPECT_EQ(1, keyVals_read.size());
          EXPECT_EQ(1, keyVals_read.count(key1));
          EXPECT_EQ(value1, *keyVals_read.at(key1).value());
          EXPECT_EQ(1, *keyVals_read.at(key1).version());
          EXPECT_EQ(nodeName_, *keyVals_read.at(key1).originatorId());
        },
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    evb.loop();
  }

  // Test 2: Update an existing persisted key with new value
  {
    thrift::KeyVals keyVals;
    keyVals[key1] = createThriftValue(0, nodeName_, updatedValue);

    thrift::KeySetParams params;
    params.keyVals() = keyVals;

    handler_
        ->semifuture_persistSelfOriginatedKey(
            std::make_unique<thrift::KeySetParams>(params),
            std::make_unique<std::string>(kSpineAreaId))
        .get();

    folly::EventBase evb;
    evb.scheduleAt(
        [this, &key1, &updatedValue]() {
          std::vector<std::string> filterKeys{key1};
          auto pub = handler_
                         ->semifuture_getKvStoreKeyValsArea(
                             std::make_unique<std::vector<std::string>>(
                                 std::move(filterKeys)),
                             std::make_unique<std::string>(kSpineAreaId))
                         .get();
          auto keyVals_read = *pub->keyVals();
          EXPECT_EQ(1, keyVals_read.size());
          EXPECT_EQ(1, keyVals_read.count(key1));
          EXPECT_EQ(updatedValue, *keyVals_read.at(key1).value());
          EXPECT_EQ(2, *keyVals_read.at(key1).version());
          EXPECT_EQ(nodeName_, *keyVals_read.at(key1).originatorId());
        },
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    evb.loop();
  }
}

#if FOLLY_HAS_COROUTINES
CO_TEST_F(OpenrCtrlFixture, CoPersistSelfOriginatedKeyApi) {
  const std::string key1 = "co-persist-key1";
  const std::string value1 = "co-persist-value1";
  const std::string updatedValue = "co-persist-value-updated";

  // Test 1: Persist a new key-value pair
  {
    thrift::KeyVals keyVals;
    keyVals[key1] = createThriftValue(1, nodeName_, value1);

    thrift::KeySetParams params;
    params.keyVals() = keyVals;

    co_await handler_->co_persistSelfOriginatedKey(
        std::make_unique<thrift::KeySetParams>(params),
        std::make_unique<std::string>(kSpineAreaId));

    // Wait for async operations to complete
    co_await folly::coro::sleep(std::chrono::milliseconds(100));

    std::vector<std::string> filterKeys{key1};
    auto pub = co_await handler_->co_getKvStoreKeyValsArea(
        std::make_unique<std::vector<std::string>>(std::move(filterKeys)),
        std::make_unique<std::string>(kSpineAreaId));
    auto keyVals_read = *pub->keyVals();
    EXPECT_EQ(1, keyVals_read.size());
    EXPECT_EQ(1, keyVals_read.count(key1));
    EXPECT_EQ(value1, *keyVals_read.at(key1).value());
    EXPECT_EQ(1, *keyVals_read.at(key1).version());
    EXPECT_EQ(nodeName_, *keyVals_read.at(key1).originatorId());
  }

  // Test 2: Update an existing persisted key with new value
  {
    thrift::KeyVals keyVals;
    keyVals[key1] = createThriftValue(1, nodeName_, updatedValue);

    thrift::KeySetParams params;
    params.keyVals() = keyVals;

    co_await handler_->co_persistSelfOriginatedKey(
        std::make_unique<thrift::KeySetParams>(params),
        std::make_unique<std::string>(kSpineAreaId));

    // Wait for async operations to complete
    co_await folly::coro::sleep(std::chrono::milliseconds(100));

    std::vector<std::string> filterKeys{key1};
    auto pub = co_await handler_->co_getKvStoreKeyValsArea(
        std::make_unique<std::vector<std::string>>(std::move(filterKeys)),
        std::make_unique<std::string>(kSpineAreaId));
    auto keyVals_read = *pub->keyVals();
    EXPECT_EQ(1, keyVals_read.size());
    EXPECT_EQ(updatedValue, *keyVals_read.at(key1).value());
    EXPECT_EQ(2, *keyVals_read.at(key1).version());
  }
}
#endif // FOLLY_HAS_COROUTINES

TEST_F(OpenrCtrlFixture, UnsetSelfOriginatedKeyApi) {
  const std::string key1 = "unset-key1";
  const std::string value1 = "unset-value1";
  const std::string finalValue = "unset-final-value";

  // Persist a key first
  {
    thrift::KeyVals keyVals;
    keyVals[key1] = createThriftValue(0, nodeName_, value1);

    thrift::KeySetParams params;
    params.keyVals() = keyVals;

    handler_
        ->semifuture_persistSelfOriginatedKey(
            std::make_unique<thrift::KeySetParams>(params),
            std::make_unique<std::string>(kSpineAreaId))
        .get();

    folly::EventBase evb;
    evb.scheduleAt(
        [this, &key1, &value1]() {
          std::vector<std::string> filterKeys{key1};
          auto pub = handler_
                         ->semifuture_getKvStoreKeyValsArea(
                             std::make_unique<std::vector<std::string>>(
                                 std::move(filterKeys)),
                             std::make_unique<std::string>(kSpineAreaId))
                         .get();
          auto keyVals_read = *pub->keyVals();
          EXPECT_EQ(1, keyVals_read.size());
          EXPECT_EQ(1, keyVals_read.count(key1));
          EXPECT_EQ(value1, *keyVals_read.at(key1).value());
          EXPECT_EQ(1, *keyVals_read.at(key1).version());
          EXPECT_EQ(nodeName_, *keyVals_read.at(key1).originatorId());
        },
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    evb.loop();
  }

  // Unset the persisted key with a final value
  {
    thrift::KeyVals keyVals;
    keyVals[key1] = createThriftValue(0, nodeName_, finalValue);

    thrift::KeySetParams params;
    params.keyVals() = keyVals;

    handler_
        ->semifuture_unsetSelfOriginatedKey(
            std::make_unique<thrift::KeySetParams>(params),
            std::make_unique<std::string>(kSpineAreaId))
        .get();

    folly::EventBase evb;
    evb.scheduleAt(
        [this, &key1, &finalValue]() {
          std::vector<std::string> filterKeys{key1};
          auto pub = handler_
                         ->semifuture_getKvStoreKeyValsArea(
                             std::make_unique<std::vector<std::string>>(
                                 std::move(filterKeys)),
                             std::make_unique<std::string>(kSpineAreaId))
                         .get();
          auto keyVals_read = *pub->keyVals();
          EXPECT_EQ(1, keyVals_read.size());
          EXPECT_EQ(1, keyVals_read.count(key1));
          EXPECT_EQ(finalValue, *keyVals_read.at(key1).value());
          // Version should be incremented from 1 to 2
          EXPECT_EQ(2, *keyVals_read.at(key1).version());
          EXPECT_EQ(nodeName_, *keyVals_read.at(key1).originatorId());
          // Verify the key is no longer in self-originated key list
          auto selfOriginatedKeys =
              kvStoreWrapper_->dumpAllSelfOriginated(kSpineAreaId);
          EXPECT_EQ(0, selfOriginatedKeys.count(key1));
        },
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    evb.loop();
  }
}

#if FOLLY_HAS_COROUTINES
CO_TEST_F(OpenrCtrlFixture, CoUnsetSelfOriginatedKeyApi) {
  const std::string key1 = "co-unset-key1";
  const std::string value1 = "co-unset-value1";
  const std::string finalValue = "co-unset-final-value";

  // Test 1: Persist a key first
  {
    thrift::KeyVals keyVals;
    keyVals[key1] = createThriftValue(1, nodeName_, value1);

    thrift::KeySetParams params;
    params.keyVals() = keyVals;

    co_await handler_->co_persistSelfOriginatedKey(
        std::make_unique<thrift::KeySetParams>(params),
        std::make_unique<std::string>(kSpineAreaId));

    // Wait for async operations to complete
    co_await folly::coro::sleep(std::chrono::milliseconds(100));

    std::vector<std::string> filterKeys{key1};
    auto pub = co_await handler_->co_getKvStoreKeyValsArea(
        std::make_unique<std::vector<std::string>>(std::move(filterKeys)),
        std::make_unique<std::string>(kSpineAreaId));
    auto keyVals_read = *pub->keyVals();
    EXPECT_EQ(1, keyVals_read.size());
    EXPECT_EQ(1, keyVals_read.count(key1));
    EXPECT_EQ(value1, *keyVals_read.at(key1).value());
    EXPECT_EQ(1, *keyVals_read.at(key1).version());
    EXPECT_EQ(nodeName_, *keyVals_read.at(key1).originatorId());
  }

  // Unset the persisted key with a final value
  {
    thrift::KeyVals keyVals;
    keyVals[key1] = createThriftValue(1, nodeName_, finalValue);

    thrift::KeySetParams params;
    params.keyVals() = keyVals;

    co_await handler_->co_unsetSelfOriginatedKey(
        std::make_unique<thrift::KeySetParams>(params),
        std::make_unique<std::string>(kSpineAreaId));

    // Wait for async operations to complete
    co_await folly::coro::sleep(std::chrono::milliseconds(100));

    std::vector<std::string> filterKeys{key1};
    auto pub = co_await handler_->co_getKvStoreKeyValsArea(
        std::make_unique<std::vector<std::string>>(std::move(filterKeys)),
        std::make_unique<std::string>(kSpineAreaId));
    auto keyVals_read = *pub->keyVals();
    EXPECT_EQ(1, keyVals_read.size());
    EXPECT_EQ(1, keyVals_read.count(key1));
    EXPECT_EQ(finalValue, *keyVals_read.at(key1).value());
    // Version should be incremented from 1 to 2
    EXPECT_EQ(2, *keyVals_read.at(key1).version());
    EXPECT_EQ(nodeName_, *keyVals_read.at(key1).originatorId());

    // Verify the key is no longer in self-originated key list
    auto selfOriginatedKeys =
        kvStoreWrapper_->dumpAllSelfOriginated(kSpineAreaId);
    EXPECT_EQ(0, selfOriginatedKeys.count(key1));
  }
}
#endif // FOLLY_HAS_COROUTINES

TEST_F(OpenrCtrlFixture, verifyDataPathTest) {
  thrift::KeyVals kvs(
      {{"key1", createThriftValue(1, "node1", std::string("value1"), 30000, 1)},
       {"key11",
        createThriftValue(1, "node1", std::string("value11"), 30000, 1)},
       {"key111",
        createThriftValue(1, "node1", std::string("value11"), 30000, 1)}});

  // will get same thrift publications as OpenrCtrlHandler
  // filters are the same as filters of OpenrCtrlHandlerReader
  auto reader = dispatcher_->getReader();

  // set new keyVals in KvStore
  setKvStoreKeyVals(kvs, kSpineAreaId);

  folly::EventBase evb;
  auto& manager = folly::fibers::getFiberManager(evb);

  manager.addTask([&reader, &kvs]() mutable {
    auto maybePub = reader.get();
    EXPECT_TRUE(maybePub.hasValue());

    auto expectedPublication = createThriftPublication(
        kvs,
        {}, /* expiredKeys */
        {}, /* nodeIds */
        {} /* keysToUpdate */,
        kSpineAreaId);

    folly::variant_match(
        std::move(maybePub).value(),
        [&expectedPublication](thrift::Publication&& pub) {
          EXPECT_TRUE(
              equalPublication(std::move(pub), std::move(expectedPublication)));
        },
        [](thrift::InitializationEvent&&) {});
  });

  evb.loop();
}

TEST_F(OpenrCtrlFixture, dispatcherApiTest) {
  std::vector<std::string> filter1{"adj:", "prefix:"};
  std::vector<std::string> filter2{"key7:", "key10:", "key25"};
  std::vector<std::string> filter3{"key3:", "key7"};

  auto reader1 = dispatcher_->getReader(filter1);
  auto reader2 = dispatcher_->getReader(filter2);
  auto reader3 = dispatcher_->getReader(filter3);

  auto resp = handler_->semifuture_getDispatcherFilters().get();

  // check returned ptr is non-null
  EXPECT_NE(resp, nullptr);

  auto filters = *resp;

  EXPECT_NE(
      std::find(filters.cbegin(), filters.cend(), filter1), filters.cend());
  EXPECT_NE(
      std::find(filters.cbegin(), filters.cend(), filter2), filters.cend());
  EXPECT_NE(
      std::find(filters.cbegin(), filters.cend(), filter3), filters.cend());
}

void
checkKvs(thrift::KeyVals& l, thrift::KeyVals& r) {
  EXPECT_EQ(l.size(), r.size());
  for (const auto& [k, v] : l) {
    EXPECT_TRUE(r.find(k) != r.end());
    auto rv = r[k];
    EXPECT_EQ(*v.version(), *rv.version());
    EXPECT_EQ(*v.ttlVersion(), *rv.ttlVersion());
    EXPECT_EQ(*v.originatorId(), *rv.originatorId());
    EXPECT_EQ(*v.value(), *rv.value());
  }
}

void
checkPublications(thrift::Publication& expected, thrift::Publication& actual) {
  checkKvs(*expected.keyVals(), *actual.keyVals());

  EXPECT_EQ(expected.expiredKeys()->size(), actual.expiredKeys()->size());
  auto sortedExpectedExpiredKey = *expected.expiredKeys();
  auto sortedActualExpiredKey = *actual.expiredKeys();
  std::sort(sortedExpectedExpiredKey.begin(), sortedExpectedExpiredKey.end());
  std::sort(sortedActualExpiredKey.begin(), sortedActualExpiredKey.end());
  EXPECT_EQ(sortedExpectedExpiredKey, sortedActualExpiredKey);
}

// Test Streaming scenario. We inject keys into KvStore and verify that the
// stream is updated accordingly. We also verify that when key expires, we
// stream it to subscribers.
TEST_F(OpenrCtrlFixture, SubscribeAndGetKvStore) {
  std::string kNewEntry = "newEntry";
  std::string kEntry1 = "key1";
  std::string kEntry2 = "key2";

  std::vector<std::string> filterKeys{kEntry1, kEntry2, kNewEntry};
  thrift::KeyVals kvs({
      {kEntry1, createThriftValue(1, "node1", std::string("value1"), 1000)},
      {kEntry2, createThriftValue(1, "node1", std::string("value2"), 3000)},
  });

  setKvStoreKeyVals(kvs, kSpineAreaId);

  // Get AsyncGenerator to process handler client stream response.
  thrift::KeyDumpParams params;
  auto ssit = std::make_unique<apache::thrift::ScopedServerInterfaceThread>(
      handler_, "::1", 0);
  auto client =
      ssit->newClient<apache::thrift::Client<openr::thrift::OpenrCtrlCpp>>();
  auto responseAndStream =
      client->semifuture_subscribeAndGetAreaKvStores(params, kSpineOnlySet)
          .get();

  auto gen = std::move(responseAndStream.stream).toAsyncGenerator();

  auto lUpdateExpectedDeltas =
      [&](const std::string& entry,
          const thrift::Value& val,
          std::vector<thrift::Publication>& expectedDeltas) {
        thrift::Publication p;
        p.keyVals()->insert({entry, val});
        p.area() = kSpineAreaId;
        expectedDeltas.emplace_back(p);
      };

  std::vector<thrift::Publication> expectedDeltas;

  // New key
  auto val0 = createThriftValue(1, "node1", std::string("value1"), 30000);
  kvStoreWrapper_->setKey(kSpineAreaId, kNewEntry, val0, std::nullopt);
  lUpdateExpectedDeltas(kNewEntry, val0, expectedDeltas);

  // Existing value update
  auto val1 = createThriftValue(2, "node1", std::string("value22"), 3000);
  kvStoreWrapper_->setKey(kSpineAreaId, kEntry2, val1, std::nullopt);
  lUpdateExpectedDeltas(kEntry2, val1, expectedDeltas);

  // Existing value update
  auto val2 = createThriftValue(3, "node1", std::string("value2"), 3000);
  kvStoreWrapper_->setKey(kSpineAreaId, kEntry2, val2, std::nullopt);
  lUpdateExpectedDeltas(kEntry2, val2, expectedDeltas);

  // Updating version only
  auto val3 = createThriftValue(3, "node1", std::string("value2"), 3000, 100);
  kvStoreWrapper_->setKey(kSpineAreaId, kEntry2, val3, std::nullopt);
  lUpdateExpectedDeltas(kEntry2, val3, expectedDeltas);

  // key1 expired
  thrift::Publication p;
  p.expiredKeys()->emplace_back(kEntry1);
  p.area() = kSpineAreaId;
  expectedDeltas.emplace_back(p);

  // Check the values from the AsyncGenerator of the client stream in order.
  int verifiedIdx = 0;
  while (auto pub = folly::coro::blockingWait(gen.next())) {
    if (verifiedIdx >= expectedDeltas.size()) {
      break;
    }
    auto pubVal = *pub;
    checkPublications(expectedDeltas[verifiedIdx++], pubVal);
  }
  // Verify that all expected publications were checked.
  EXPECT_EQ(verifiedIdx, expectedDeltas.size());
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  const folly::Init init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
