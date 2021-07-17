/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdio>
#include <thread>

#include <fbzmq/zmq/Context.h>
#include <folly/init/Init.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/Constants.h>
#include <openr/common/OpenrClient.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/OpenrThriftServerWrapper.h>
#include <openr/tests/mocks/NetlinkEventsInjector.h>

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
    std::vector<openr::thrift::AreaConfig> areaConfig;
    for (auto id : {kSpineAreaId, kPlaneAreaId, kPodAreaId}) {
      thrift::AreaConfig area;
      area.area_id_ref() = id;
      area.include_interface_regexes_ref() = {"po.*"};
      area.neighbor_regexes_ref() = {".*"};
      areaConfig.emplace_back(std::move(area));
    }
    // create config
    auto tConfig = getBasicOpenrConfig(
        nodeName_,
        "domain",
        areaConfig,
        true /* enableV4 */,
        true /* enableSegmentRouting */);

    // decision config
    tConfig.decision_config_ref()->debounce_min_ms_ref() = 10;
    tConfig.decision_config_ref()->debounce_max_ms_ref() = 500;
    tConfig.decision_config_ref()->enable_bgp_route_programming_ref() = true;

    // kvstore config
    tConfig.kvstore_config_ref()->sync_interval_s_ref() = 1;
    tConfig.kvstore_config_ref()->enable_flood_optimization_ref() = true;
    tConfig.kvstore_config_ref()->is_flood_root_ref() = true;

    // link monitor config
    auto& lmConf = *tConfig.link_monitor_config_ref();
    lmConf.linkflap_initial_backoff_ms_ref() = 1;
    lmConf.linkflap_max_backoff_ms_ref() = 8;
    lmConf.use_rtt_metric_ref() = false;
    *lmConf.include_interface_regexes_ref() = {"po.*"};
    lmConf.enable_perf_measurement_ref() = false;
    tConfig.assume_drained_ref() = false;

    // persistent config-store config
    std::string const configStoreFile = "/tmp/openr-ctrl-handler-test.bin";
    tConfig.persistent_config_store_path_ref() = configStoreFile;

    // ip_tos config
    tConfig.ip_tos_ref() = 192;

    config = std::make_shared<Config>(tConfig);

    // Create the PersistentStore and start fresh
    std::remove(configStoreFile.data());
    persistentStore = std::make_unique<PersistentStore>(config);
    persistentStoreThread_ = std::thread([&]() { persistentStore->run(); });

    // Create KvStore module
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(
        context_, config, std::nullopt, kvRequestQueue_.getReader());
    kvStoreWrapper_->run();

    // Create Decision module
    decision = std::make_shared<Decision>(
        config,
        kvStoreWrapper_->getReader(),
        staticRoutesUpdatesQueue_.getReader(),
        routeUpdatesQueue_);
    decisionThread_ = std::thread([&]() { decision->run(); });

    // Create Fib moduleKeySyncMultipleArea
    fib = std::make_shared<Fib>(
        config,
        routeUpdatesQueue_.getReader(),
        staticRoutesUpdatesQueue_.getReader(),
        fibRouteUpdatesQueue_,
        logSampleQueue_);
    fibThread_ = std::thread([&]() { fib->run(); });

    // Create PrefixManager module
    prefixManager = std::make_shared<PrefixManager>(
        staticRoutesUpdatesQueue_,
        kvRequestQueue_,
        prefixUpdatesQueue_.getReader(),
        fibRouteUpdatesQueue_.getReader(),
        config,
        kvStoreWrapper_->getKvStore());
    prefixManagerThread_ = std::thread([&]() { prefixManager->run(); });

    // create fakeNetlinkProtocolSocket
    nlSock_ = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&evb_);

    // Create LinkMonitor
    re2::RE2::Options regexOpts;
    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add("po.*", &regexErr);
    includeRegexList->Compile();

    linkMonitor = std::make_shared<LinkMonitor>(
        config,
        nlSock_.get(),
        kvStoreWrapper_->getKvStore(),
        persistentStore.get(),
        interfaceUpdatesQueue_,
        prefixUpdatesQueue_,
        peerUpdatesQueue_,
        logSampleQueue_,
        kvRequestQueue_,
        neighborUpdatesQueue_.getReader(),
        kvStoreWrapper_->getInitialSyncEventsReader(),
        nlSock_->getReader(),
        false /* overrideDrainState */);
    linkMonitorThread_ = std::thread([&]() { linkMonitor->run(); });

    // spin up an openrThriftServer
    openrThriftServerWrapper_ = std::make_shared<OpenrThriftServerWrapper>(
        nodeName_,
        decision.get() /* decision */,
        fib.get() /* fib */,
        kvStoreWrapper_->getKvStore() /* kvStore */,
        linkMonitor.get() /* linkMonitor */,
        monitor.get() /* monitor */,
        persistentStore.get() /* configStore */,
        prefixManager.get() /* prefixManager */,
        nullptr /* spark */,
        config);
    openrThriftServerWrapper_->run();

    // initialize openrCtrlClient talking to server
    client_ = getOpenrCtrlPlainTextClient<apache::thrift::HeaderClientChannel>(
        evb_,
        folly::IPAddress(Constants::kPlatformHost.toString()),
        openrThriftServerWrapper_->getOpenrCtrlThriftPort());
  }

  void
  TearDown() override {
    routeUpdatesQueue_.close();
    staticRoutesUpdatesQueue_.close();
    interfaceUpdatesQueue_.close();
    peerUpdatesQueue_.close();
    neighborUpdatesQueue_.close();
    prefixUpdatesQueue_.close();
    fibRouteUpdatesQueue_.close();
    kvRequestQueue_.close();
    logSampleQueue_.close();
    nlSock_->closeQueue();
    kvStoreWrapper_->closeQueue();

    client_.reset();

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

    kvStoreWrapper_->stop();
    kvStoreWrapper_.reset();

    openrThriftServerWrapper_->stop();
    openrThriftServerWrapper_.reset();
  }

  thrift::PrefixEntry
  createPrefixEntry(const std::string& prefix, thrift::PrefixType prefixType) {
    thrift::PrefixEntry prefixEntry;
    *prefixEntry.prefix_ref() = toIpPrefix(prefix);
    prefixEntry.type_ref() = prefixType;
    return prefixEntry;
  }

  void
  setKvStoreKeyVals(const thrift::KeyVals& keyVals, const std::string& area) {
    thrift::KeySetParams setParams;
    setParams.keyVals_ref() = keyVals;

    client_->sync_setKvStoreKeyVals(setParams, area);
  }

 private:
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue_;
  messaging::ReplicateQueue<InterfaceDatabase> interfaceUpdatesQueue_;
  messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue_;
  messaging::ReplicateQueue<NeighborEvents> neighborUpdatesQueue_;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRoutesUpdatesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue_;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
  // Queue for event logs
  messaging::ReplicateQueue<LogSample> logSampleQueue_;

  fbzmq::Context context_{};
  folly::EventBase evb_;

  std::thread decisionThread_;
  std::thread fibThread_;
  std::thread prefixManagerThread_;
  std::thread persistentStoreThread_;
  std::thread linkMonitorThread_;

  std::shared_ptr<Config> config;
  std::shared_ptr<Decision> decision;
  std::shared_ptr<Fib> fib;
  std::shared_ptr<PrefixManager> prefixManager;
  std::shared_ptr<PersistentStore> persistentStore;
  std::shared_ptr<LinkMonitor> linkMonitor;
  std::shared_ptr<Monitor> monitor;

 public:
  const std::string nodeName_{"thanos@universe"};
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock_{nullptr};
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_{nullptr};
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper_{nullptr};
  std::unique_ptr<openr::thrift::OpenrCtrlCppAsyncClient> client_{nullptr};
};

TEST_F(OpenrCtrlFixture, getMyNodeName) {
  std::string res = "";
  client_->sync_getMyNodeName(res);
  EXPECT_EQ(nodeName_, res);
}

TEST_F(OpenrCtrlFixture, PrefixManagerApis) {
  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("10.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("11.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("20.0.0.0/8", thrift::PrefixType::BGP),
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    client_->sync_advertisePrefixes(
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    client_->sync_withdrawPrefixes(
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
    client_->sync_withdrawPrefixesByType(thrift::PrefixType::LOOPBACK);
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    client_->sync_syncPrefixesByType(
        thrift::PrefixType::BGP,
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
  }

  {
    const std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    std::vector<thrift::PrefixEntry> res;
    client_->sync_getPrefixes(res);
    EXPECT_EQ(prefixes, res);
  }

  {
    std::vector<thrift::PrefixEntry> res;
    client_->sync_getPrefixesByType(res, thrift::PrefixType::LOOPBACK);
    EXPECT_EQ(0, res.size());
  }

  {
    std::vector<thrift::AdvertisedRouteDetail> routes;
    client_->sync_getAdvertisedRoutes(routes);
    EXPECT_EQ(1, routes.size());
  }
}

TEST_F(OpenrCtrlFixture, RouteApis) {
  {
    thrift::RouteDatabase db;
    client_->sync_getRouteDb(db);
    EXPECT_EQ(nodeName_, db.thisNodeName_ref());
    EXPECT_EQ(0, db.unicastRoutes_ref()->size());
    EXPECT_EQ(0, db.mplsRoutes_ref()->size());
  }

  {
    thrift::RouteDatabase db;
    client_->sync_getRouteDbComputed(db, nodeName_);
    EXPECT_EQ(nodeName_, db.thisNodeName_ref());
    EXPECT_EQ(0, db.unicastRoutes_ref()->size());
    EXPECT_EQ(0, db.mplsRoutes_ref()->size());
  }

  {
    const std::string testNode("avengers@universe");
    thrift::RouteDatabase db;
    client_->sync_getRouteDbComputed(db, testNode);
    EXPECT_EQ(testNode, *db.thisNodeName_ref());
    EXPECT_EQ(0, db.unicastRoutes_ref()->size());
    EXPECT_EQ(0, db.mplsRoutes_ref()->size());
  }

  {
    std::vector<thrift::UnicastRoute> filterRet;
    std::vector<std::string> prefixes{"10.46.2.0", "10.46.2.0/24"};
    client_->sync_getUnicastRoutesFiltered(filterRet, prefixes);
    EXPECT_EQ(0, filterRet.size());
  }

  {
    std::vector<thrift::UnicastRoute> allRouteRet;
    client_->sync_getUnicastRoutes(allRouteRet);
    EXPECT_EQ(0, allRouteRet.size());
  }
  {
    std::vector<thrift::MplsRoute> filterRet;
    std::vector<std::int32_t> labels{1, 2};
    client_->sync_getMplsRoutesFiltered(filterRet, labels);
    EXPECT_EQ(0, filterRet.size());
  }
  {
    std::vector<thrift::MplsRoute> allRouteRet;
    client_->sync_getMplsRoutes(allRouteRet);
    EXPECT_EQ(0, allRouteRet.size());
  }
}

TEST_F(OpenrCtrlFixture, PerfApis) {
  thrift::PerfDatabase db;
  client_->sync_getPerfDb(db);
  EXPECT_EQ(nodeName_, db.thisNodeName_ref());
}

TEST_F(OpenrCtrlFixture, DecisionApis) {
  {
    std::vector<thrift::AdjacencyDatabase> dbs;
    client_->sync_getDecisionAdjacenciesFiltered(dbs, {});
    EXPECT_EQ(0, dbs.size());
  }

  {
    std::vector<thrift::ReceivedRouteDetail> routes;
    client_->sync_getReceivedRoutes(routes);
    EXPECT_EQ(0, routes.size());
  }

  {
    // Positive Test
    std::vector<thrift::ReceivedRouteDetail> routes;
    client_->sync_getReceivedRoutesFiltered(routes, {});
    EXPECT_EQ(0, routes.size());

    // Nevative Test
    thrift::ReceivedRouteFilter filter;
    folly::IPAddress v4Addr = folly::IPAddress("11.0.0.1");
    folly::IPAddress v6Addr = folly::IPAddress("fe80::1");
    thrift::IpPrefix v4Prefix, v6Prefix;
    v4Prefix.prefixAddress_ref() = toBinaryAddress(v4Addr);
    v4Prefix.prefixLength_ref() = 36; // ATTN: max mask length is 32 for IPV4
    v6Prefix.prefixAddress_ref() = toBinaryAddress(v6Addr);
    v6Prefix.prefixLength_ref() = 130; // ATTN: max mask length is 32 for IPV4

    filter.prefixes_ref() = {v4Prefix};
    EXPECT_THROW(
        client_->sync_getReceivedRoutesFiltered(routes, filter),
        thrift::OpenrError);

    filter.prefixes_ref() = {v6Prefix};
    EXPECT_THROW(
        client_->sync_getReceivedRoutesFiltered(routes, filter),
        thrift::OpenrError);
  }
}

TEST_F(OpenrCtrlFixture, KvStoreApis) {
  thrift::KeyVals keyVals;
  keyVals["key1"] = createThriftValue(1, "node1", std::string("value1"));
  keyVals["key11"] = createThriftValue(1, "node1", std::string("value11"));
  keyVals["key111"] = createThriftValue(1, "node1", std::string("value111"));
  keyVals["key2"] = createThriftValue(1, "node1", std::string("value2"));
  keyVals["key22"] = createThriftValue(1, "node1", std::string("value22"));
  keyVals["key222"] = createThriftValue(1, "node1", std::string("value222"));
  keyVals["key3"] = createThriftValue(1, "node3", std::string("value3"));
  keyVals["key33"] = createThriftValue(1, "node33", std::string("value33"));
  keyVals["key333"] = createThriftValue(1, "node33", std::string("value333"));

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
    thrift::OpenrConfig config;
    client_->sync_getRunningConfigThrift(config);
    std::unordered_set<std::string> areas;
    for (auto const& area : config.get_areas()) {
      areas.insert(area.get_area_id());
    }
    EXPECT_THAT(areas, testing::SizeIs(3));
    EXPECT_THAT(
        areas,
        testing::UnorderedElementsAre(kPodAreaId, kPlaneAreaId, kSpineAreaId));
  }

  // Key set/get
  {
    setKvStoreKeyVals(keyVals, kSpineAreaId);
    setKvStoreKeyVals(keyValsPod, kPodAreaId);
    setKvStoreKeyVals(keyValsPlane, kPlaneAreaId);
  }

  {
    std::vector<std::string> filterKeys{"key11", "key2"};
    thrift::Publication pub;
    client_->sync_getKvStoreKeyValsArea(pub, filterKeys, kSpineAreaId);
    EXPECT_EQ(2, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key2"), pub.keyVals_ref()["key2"]);
    EXPECT_EQ(keyVals.at("key11"), pub.keyVals_ref()["key11"]);
  }

  // pod keys
  {
    std::vector<std::string> filterKeys{"keyPod1"};
    thrift::Publication pub;
    client_->sync_getKvStoreKeyValsArea(pub, filterKeys, kPodAreaId);
    EXPECT_EQ(1, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyValsPod.at("keyPod1"), pub.keyVals_ref()["keyPod1"]);
  }

  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    *params.prefix_ref() = "key3";
    params.originatorIds_ref()->insert("node3");
    params.keys_ref() = {"key3"};

    client_->sync_getKvStoreKeyValsFilteredArea(pub, params, kSpineAreaId);
    EXPECT_EQ(3, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key3"), pub.keyVals_ref()["key3"]);
    EXPECT_EQ(keyVals.at("key33"), pub.keyVals_ref()["key33"]);
    EXPECT_EQ(keyVals.at("key333"), pub.keyVals_ref()["key333"]);
  }

  // with areas
  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    *params.prefix_ref() = "keyP";
    params.originatorIds_ref()->insert("node1");
    params.keys_ref() = {"keyP"};

    client_->sync_getKvStoreKeyValsFilteredArea(pub, params, kPlaneAreaId);
    EXPECT_EQ(2, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyValsPlane.at("keyPlane1"), pub.keyVals_ref()["keyPlane1"]);
    EXPECT_EQ(keyValsPlane.at("keyPlane2"), pub.keyVals_ref()["keyPlane2"]);
  }

  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    *params.prefix_ref() = "key3";
    params.originatorIds_ref()->insert("node3");
    params.keys_ref() = {"key3"};

    client_->sync_getKvStoreHashFilteredArea(pub, params, kSpineAreaId);
    EXPECT_EQ(3, (*pub.keyVals_ref()).size());
    auto value3 = keyVals.at("key3");
    value3.value_ref().reset();
    auto value33 = keyVals.at("key33");
    value33.value_ref().reset();
    auto value333 = keyVals.at("key333");
    value333.value_ref().reset();
    EXPECT_EQ(value3, pub.keyVals_ref()["key3"]);
    EXPECT_EQ(value33, pub.keyVals_ref()["key33"]);
    EXPECT_EQ(value333, pub.keyVals_ref()["key333"]);
  }

  //
  // getKvStoreAreaSummary() related
  //
  {
    std::set<std::string> areaSetAll{
        kPodAreaId, kPlaneAreaId, kSpineAreaId, kTestingAreaName};
    std::map<std::string, int> areaKVCountMap{};
    std::vector<thrift::KvStoreAreaSummary> summary;

    // get summary from KvStore for all configured areas (one extra
    // non-existent area is provided)
    client_->sync_getKvStoreAreaSummary(summary, areaSetAll);
    EXPECT_THAT(summary, testing::SizeIs(3));
    // map each area to the # of keyVals in each area
    areaKVCountMap[summary.at(0).get_area()] = summary.at(0).get_keyValsCount();
    areaKVCountMap[summary.at(1).get_area()] = summary.at(1).get_keyValsCount();
    areaKVCountMap[summary.at(2).get_area()] = summary.at(2).get_keyValsCount();
    // test # of keyVals for each area, as per config above.
    // area names are being implicitly tested as well
    EXPECT_EQ(9, areaKVCountMap[kSpineAreaId]);
    EXPECT_EQ(2, areaKVCountMap[kPodAreaId]);
    EXPECT_EQ(2, areaKVCountMap[kPlaneAreaId]);
  }
  //
  // Dual and Flooding APIs
  //
  {
    thrift::DualMessages messages;
    client_->sync_processKvStoreDualMessage(messages, kSpineAreaId);
  }

  {
    thrift::FloodTopoSetParams params;
    params.rootId_ref() = nodeName_;
    client_->sync_updateFloodTopologyChild(params, kSpineAreaId);
  }

  {
    thrift::SptInfos ret;
    client_->sync_getSpanningTreeInfos(ret, kSpineAreaId);
    EXPECT_EQ(1, ret.infos_ref()->size());
    ASSERT_NE(ret.infos_ref()->end(), ret.infos_ref()->find(nodeName_));
    EXPECT_EQ(0, ret.counters_ref()->neighborCounters_ref()->size());
    EXPECT_EQ(1, ret.counters_ref()->rootCounters_ref()->size());
    EXPECT_EQ(nodeName_, *ret.floodRootId_ref());
    EXPECT_EQ(0, ret.floodPeers_ref()->size());

    thrift::SptInfo sptInfo = ret.infos_ref()->at(nodeName_);
    EXPECT_EQ(0, *sptInfo.cost_ref());
    ASSERT_TRUE(sptInfo.parent_ref().has_value());
    EXPECT_EQ(nodeName_, sptInfo.parent_ref().value());
    EXPECT_EQ(0, sptInfo.children_ref()->size());
  }

  //
  // Peers APIs
  //
  const thrift::PeersMap peers{
      {"peer1",
       createPeerSpec(
           "inproc://peer1-cmd", Constants::kPlatformHost.toString())},
      {"peer2",
       createPeerSpec(
           "inproc://peer2-cmd", Constants::kPlatformHost.toString())},
      {"peer3",
       createPeerSpec(
           "inproc://peer3-cmd", Constants::kPlatformHost.toString())}};

  // do the same with non-default area
  const thrift::PeersMap peersPod{
      {"peer11",
       createPeerSpec(
           "inproc://peer11-cmd", Constants::kPlatformHost.toString())},
      {"peer21",
       createPeerSpec(
           "inproc://peer21-cmd", Constants::kPlatformHost.toString())},
  };

  {
    for (auto& peer : peers) {
      kvStoreWrapper_->addPeer(kSpineAreaId, peer.first, peer.second);
    }
    for (auto& peerPod : peersPod) {
      kvStoreWrapper_->addPeer(kPodAreaId, peerPod.first, peerPod.second);
    }

    thrift::PeersMap ret;
    client_->sync_getKvStorePeersArea(ret, kSpineAreaId);

    EXPECT_EQ(3, ret.size());
    EXPECT_TRUE(ret.count("peer1"));
    EXPECT_TRUE(ret.count("peer2"));
    EXPECT_TRUE(ret.count("peer3"));
  }

  {
    kvStoreWrapper_->delPeer(kSpineAreaId, "peer2");

    thrift::PeersMap ret;
    client_->sync_getKvStorePeersArea(ret, kSpineAreaId);
    EXPECT_EQ(2, ret.size());
    EXPECT_TRUE(ret.count("peer1"));
    EXPECT_TRUE(ret.count("peer3"));
  }

  {
    thrift::PeersMap ret;
    client_->sync_getKvStorePeersArea(ret, kPodAreaId);

    EXPECT_EQ(2, ret.size());
    EXPECT_TRUE(ret.count("peer11"));
    EXPECT_TRUE(ret.count("peer21"));
  }

  {
    kvStoreWrapper_->delPeer(kPodAreaId, "peer21");

    thrift::PeersMap ret;
    client_->sync_getKvStorePeersArea(ret, kPodAreaId);
    EXPECT_EQ(1, ret.size());
    EXPECT_TRUE(ret.count("peer11"));
  }

  // Not using params.prefix. Instead using keys. params.prefix will be
  // deprecated soon. There are three sub-tests with different prefix
  // key values.
  {
    thrift::Publication pub;
    thrift::Publication pub33;
    thrift::Publication pub333;
    thrift::KeyDumpParams params;
    thrift::KeyDumpParams params33;
    thrift::KeyDumpParams params333;
    params.originatorIds_ref()->insert("node3");
    params.keys_ref() = {"key3"};

    client_->sync_getKvStoreKeyValsFilteredArea(pub, params, kSpineAreaId);
    EXPECT_EQ(3, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key3"), (*pub.keyVals_ref())["key3"]);
    EXPECT_EQ(keyVals.at("key33"), (*pub.keyVals_ref())["key33"]);
    EXPECT_EQ(keyVals.at("key333"), (*pub.keyVals_ref())["key333"]);

    params33.originatorIds_ref() = {"node33"};
    params33.keys_ref() = {"key33"};
    client_->sync_getKvStoreKeyValsFilteredArea(pub33, params33, kSpineAreaId);
    EXPECT_EQ(2, (*pub33.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key33"), (*pub33.keyVals_ref())["key33"]);
    EXPECT_EQ(keyVals.at("key333"), (*pub33.keyVals_ref())["key333"]);

    // Two updates because the operator is OR and originator ids for keys
    // key33 and key333 are same.
    params333.originatorIds_ref() = {"node33"};
    params333.keys_ref() = {"key333"};
    client_->sync_getKvStoreKeyValsFilteredArea(
        pub333, params333, kSpineAreaId);
    EXPECT_EQ(2, (*pub333.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key33"), (*pub33.keyVals_ref())["key33"]);
    EXPECT_EQ(keyVals.at("key333"), (*pub333.keyVals_ref())["key333"]);
  }

  // with areas but do not use prefix (to be deprecated). use prefixes/keys
  // instead.
  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    params.originatorIds_ref()->insert("node1");
    params.keys_ref() = {"keyP", "keyPl"};

    client_->sync_getKvStoreKeyValsFilteredArea(pub, params, kPlaneAreaId);
    EXPECT_EQ(2, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyValsPlane.at("keyPlane1"), (*pub.keyVals_ref())["keyPlane1"]);
    EXPECT_EQ(keyValsPlane.at("keyPlane2"), (*pub.keyVals_ref())["keyPlane2"]);
  }

  // Operator is OR and params.prefix is empty.
  // Use HashFiltered
  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    params.originatorIds_ref() = {"node3"};
    params.keys_ref() = {"key3"};

    client_->sync_getKvStoreHashFilteredArea(pub, params, kSpineAreaId);
    EXPECT_EQ(3, (*pub.keyVals_ref()).size());
    auto value3 = keyVals.at("key3");
    value3.value_ref().reset();
    auto value33 = keyVals.at("key33");
    value33.value_ref().reset();
    auto value333 = keyVals.at("key333");
    value333.value_ref().reset();
    EXPECT_EQ(value3, (*pub.keyVals_ref())["key3"]);
    EXPECT_EQ(value33, (*pub.keyVals_ref())["key33"]);
    EXPECT_EQ(value333, (*pub.keyVals_ref())["key333"]);
  }
}

TEST_F(OpenrCtrlFixture, subscribeAndGetKvStoreFilteredWithKeysNoTtlUpdate) {
  thrift::KeyVals keyVals;
  keyVals["key1"] =
      createThriftValue(1, "node1", std::string("value1"), 30000, 1);
  keyVals["key11"] =
      createThriftValue(1, "node1", std::string("value11"), 30000, 1);
  keyVals["key111"] =
      createThriftValue(1, "node1", std::string("value111"), 30000, 1);
  keyVals["key2"] =
      createThriftValue(1, "node1", std::string("value2"), 30000, 1);
  keyVals["key22"] =
      createThriftValue(1, "node1", std::string("value22"), 30000, 1);
  keyVals["key222"] =
      createThriftValue(1, "node1", std::string("value222"), 30000, 1);
  keyVals["key3"] =
      createThriftValue(1, "node3", std::string("value3"), 30000, 1);
  keyVals["key33"] =
      createThriftValue(1, "node33", std::string("value33"), 30000, 1);
  keyVals["key333"] =
      createThriftValue(1, "node33", std::string("value333"), 30000, 1);

  // Key set
  setKvStoreKeyVals(keyVals, kSpineAreaId);

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
    thrift::Publication pub;
    client_->sync_getKvStoreKeyValsArea(pub, filterKeys, kSpineAreaId);
    EXPECT_EQ(1, (*pub.keyVals_ref()).size());
    EXPECT_EQ(3, *((*pub.keyVals_ref()).at(key).version_ref()));
    EXPECT_EQ("value1", (*pub.keyVals_ref()).at(key).value_ref().value());
  }

  {
    const std::string key{"snoop-key"};
    std::atomic<int> received{0};
    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();
    // Expect 10 keys in the initial dump
    // NOTE: there may be extra keys from PrefixManager & LinkMonitor)
    EXPECT_LE(
        10, (*responseAndSubscription.response.begin()->keyVals_ref()).size());
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals_ref()).count(key));
    EXPECT_EQ(
        responseAndSubscription.response.begin()->keyVals_ref()->at(key),
        createThriftValue(3, "node1", std::string("value1")));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() or not t->keyVals_ref()->count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals_ref()).size());
              ASSERT_EQ(1, (*pub.keyVals_ref()).count(key));
              EXPECT_EQ(
                  "value1", (*pub.keyVals_ref()).at(key).value_ref().value());
              EXPECT_EQ(
                  received + 4, *(*pub.keyVals_ref()).at(key).version_ref());
              received++;
            });
    EXPECT_EQ(1, handler->getNumKvStorePublishers());
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
    while (handler->getNumKvStorePublishers() != 0) {
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
    filter.keys_ref() = keys;
    filter.originatorIds_ref() = {"node1", "node2", "node3", "node33"};

    filter.oper_ref() = thrift::FilterOperator::AND;
    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto handler_other = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
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
    EXPECT_LE(
        0, (*responseAndSubscription.response.begin()->keyVals_ref()).size());
    ASSERT_EQ(
        0,
        (*responseAndSubscription.response.begin()->keyVals_ref()).count(key));
    EXPECT_LE(
        0,
        (*responseAndSubscription_other.response.begin()->keyVals_ref())
            .size());
    ASSERT_EQ(
        0,
        (*responseAndSubscription_other.response.begin()->keyVals_ref())
            .count(key));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() or not t->keyVals_ref()->count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals_ref()).size());
              ASSERT_EQ(1, (*pub.keyVals_ref()).count(key));
              EXPECT_EQ(
                  "value4", (*pub.keyVals_ref()).at(key).value_ref().value());
              received++;
            });

    auto subscription_other =
        std::move(responseAndSubscription_other.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, random_key](auto&& t) {
                  if (!t.hasValue() or
                      not t->keyVals_ref()->count(random_key)) {
                    return;
                  }
                  auto& pub = *t;
                  EXPECT_EQ(1, (*pub.keyVals_ref()).size());
                  ASSERT_EQ(1, (*pub.keyVals_ref()).count(random_key));
                  EXPECT_EQ(
                      "value_random",
                      (*pub.keyVals_ref()).at(random_key).value_ref().value());
                  received++;
                });

    /* There are two clients */
    EXPECT_EQ(2, handler->getNumKvStorePublishers());
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
    while (handler->getNumKvStorePublishers() != 0) {
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
    filter.keys_ref() = {"key33"};
    filter.originatorIds_ref() = {"node1", "node2", "node3", "node33"};
    filter.oper_ref() = thrift::FilterOperator::AND;

    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    /* prefix key is key33. kv store has key33 and key333 */
    EXPECT_LE(
        2, responseAndSubscription.response.begin()->keyVals_ref()->size());
    ASSERT_EQ(
        1, responseAndSubscription.response.begin()->keyVals_ref()->count(key));
    ASSERT_EQ(
        1,
        responseAndSubscription.response.begin()->keyVals_ref()->count(
            "key333"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() or not t->keyVals_ref()->count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals_ref()).size());
              ASSERT_EQ(1, (*pub.keyVals_ref()).count(key));
              // Validates value is set with KeyDumpParams.doNotPublishValue =
              // false
              EXPECT_EQ(
                  "value333", (*pub.keyVals_ref()).at(key).value_ref().value());
              received++;
            });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());
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
    while (handler->getNumKvStorePublishers() != 0) {
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
    filter.keys_ref() = keys;
    filter.originatorIds_ref() = {"node1", "node2", "node3", "node33"};
    std::unordered_map<std::string, std::string> keyvals;
    keyvals["key33"] = "value33";
    keyvals["key333"] = "value333";

    filter.oper_ref() = thrift::FilterOperator::OR;

    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(
        2, (*responseAndSubscription.response.begin()->keyVals_ref()).size());
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals_ref())
            .count("key33"));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals_ref())
            .count("key333"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (not t.hasValue()) {
                    return;
                  }

                  for (const auto& kv : keyvals) {
                    if (not t->keyVals_ref()->count(kv.first)) {
                      continue;
                    }
                    auto& pub = *t;
                    EXPECT_EQ(1, (*pub.keyVals_ref()).size());
                    ASSERT_EQ(1, (*pub.keyVals_ref()).count(kv.first));
                    EXPECT_EQ(
                        kv.second,
                        (*pub.keyVals_ref()).at(kv.first).value_ref().value());
                    received++;
                  }
                });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());
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
    while (handler->getNumKvStorePublishers() != 0) {
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
    filter.keys_ref() = keys;
    filter.originatorIds_ref() = {"node1", "node2", "node3", "node33"};
    filter.oper_ref() = thrift::FilterOperator::AND;

    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(
        3, (*responseAndSubscription.response.begin()->keyVals_ref()).size());
    EXPECT_EQ(
        0,
        (*responseAndSubscription.response.begin()->keyVals_ref()).count(key));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals_ref())
            .count("key1"));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals_ref())
            .count("key3"));

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
                    if (t->keyVals_ref()->count(kv.first)) {
                      EXPECT_EQ(1, (*pub.keyVals_ref()).size());
                      ASSERT_EQ(1, (*pub.keyVals_ref()).count(kv.first));
                      EXPECT_EQ(
                          kv.second,
                          (*pub.keyVals_ref())
                              .at(kv.first)
                              .value_ref()
                              .value());
                      received++;
                      found = true;
                    }
                  }
                  if (not found) {
                    return;
                  }
                });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());
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
    while (handler->getNumKvStorePublishers() != 0) {
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
    filter.keys_ref() = keys;
    filter.originatorIds_ref() = {"node1", "node2", "node3", "node33"};
    filter.oper_ref() = thrift::FilterOperator::OR;

    std::unordered_map<std::string, std::string> keyvals;
    keyvals["key1"] = "value1";
    keyvals["key3"] = "value3";
    keyvals[key] = "value1";
    keyvals["random-prefix"] = "value1";

    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(
        3, (*responseAndSubscription.response.begin()->keyVals_ref()).size());
    EXPECT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals_ref()).count(key));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals_ref())
            .count("key1"));
    ASSERT_EQ(
        1,
        (*responseAndSubscription.response.begin()->keyVals_ref())
            .count("key3"));

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
                    if (t->keyVals_ref()->count(kv.first)) {
                      EXPECT_EQ(1, (*pub.keyVals_ref()).size());
                      ASSERT_EQ(1, (*pub.keyVals_ref()).count(kv.first));
                      EXPECT_EQ(
                          kv.second,
                          (*pub.keyVals_ref())
                              .at(kv.first)
                              .value_ref()
                              .value());
                      received++;
                      found = true;
                    }
                  }
                  if (not found) {
                    return;
                  }
                });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());
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
    while (handler->getNumKvStorePublishers() != 0) {
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
    filter.keys_ref() = keys;
    filter.originatorIds_ref()->insert("node10");
    filter.oper_ref() = thrift::FilterOperator::AND;

    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    /* The key is not in kv store */
    EXPECT_LE(
        0, (*responseAndSubscription.response.begin()->keyVals_ref()).size());

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              if (!t.hasValue() or not t->keyVals_ref()->count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals_ref()).size());
              ASSERT_EQ(1, (*pub.keyVals_ref()).count(key));
              EXPECT_EQ(
                  "value1", (*pub.keyVals_ref()).at(key).value_ref().value());
              received++;
            });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());
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
    while (handler->getNumKvStorePublishers() != 0) {
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
    filter.keys_ref() = {"key1", "key2", "key3", key};
    filter.originatorIds_ref()->insert("node10");
    std::unordered_map<std::string, std::string> keyvals;
    keyvals["key1"] = "value1";
    keyvals["key2"] = "value2";
    keyvals["key3"] = "value3";
    keyvals[key] = "value1";
    keyvals["random-prefix-2"] = "value1";

    filter.oper_ref() = thrift::FilterOperator::OR;

    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(
        0, (*responseAndSubscription.response.begin()->keyVals_ref()).size());

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (not t.hasValue()) {
                    return;
                  }

                  for (const auto& kv : keyvals) {
                    if (not t->keyVals_ref()->count(kv.first)) {
                      continue;
                    }
                    auto& pub = *t;
                    EXPECT_EQ(1, (*pub.keyVals_ref()).size());
                    ASSERT_EQ(1, (*pub.keyVals_ref()).count(kv.first));
                    EXPECT_EQ(
                        kv.second,
                        (*pub.keyVals_ref()).at(kv.first).value_ref().value());
                    received++;
                  }
                });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());
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
    while (handler->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }
}

TEST_F(
    OpenrCtrlFixture, subscribeAndGetKvStoreFilteredWithKeysTtlUpdateOption) {
  thrift::KeyVals keyVals;
  keyVals["key1"] =
      createThriftValue(1, "node1", std::string("value1"), 30000, 1);
  keyVals["key11"] =
      createThriftValue(1, "node1", std::string("value11"), 30000, 1);
  keyVals["key111"] =
      createThriftValue(1, "node1", std::string("value111"), 30000, 1);
  keyVals["key2"] =
      createThriftValue(1, "node1", std::string("value2"), 30000, 1);
  keyVals["key22"] =
      createThriftValue(1, "node1", std::string("value22"), 30000, 1);
  keyVals["key222"] =
      createThriftValue(1, "node1", std::string("value222"), 30000, 1);
  keyVals["key3"] =
      createThriftValue(1, "node3", std::string("value3"), 30000, 1);
  keyVals["key33"] =
      createThriftValue(1, "node33", std::string("value33"), 30000, 1);
  keyVals["key333"] =
      createThriftValue(1, "node33", std::string("value333"), 30000, 1);

  // Key set
  setKvStoreKeyVals(keyVals, kSpineAreaId);

  // ignoreTTL = false is specified in filter.
  // Client should receive publication associated with TTL update
  {
    const std::string key{"key1"};
    thrift::KeyDumpParams filter;
    filter.keys_ref() = {key};
    filter.ignoreTtl_ref() = false;
    filter.originatorIds_ref()->insert("node1");
    std::unordered_map<std::string, std::string> keyvals;
    keyvals[key] = "value1";
    filter.oper_ref() = thrift::FilterOperator::AND;

    const auto value = createThriftValue(
        1 /* version */,
        "node1",
        "value1",
        30000 /* ttl*/,
        5 /* ttl version */,
        0 /* hash */);

    auto thriftValue = value;
    thriftValue.value_ref().reset();
    kvStoreWrapper_->setKey(kSpineAreaId, "key1", thriftValue);
    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();

    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(filter),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(
        3, (*responseAndSubscription.response.begin()->keyVals_ref()).size());
    for (const auto& key_ : {"key1", "key11", "key111"}) {
      EXPECT_EQ(
          1,
          (*responseAndSubscription.response.begin()->keyVals_ref())
              .count(key_));
    }

    EXPECT_EQ(
        0,
        (*responseAndSubscription.response.begin()->keyVals_ref())
            .count("key2"));
    const auto& val1 =
        (*responseAndSubscription.response.begin()->keyVals_ref())["key1"];
    ASSERT_EQ(true, val1.value_ref().has_value()); /* value is non-null */
    EXPECT_EQ(1, *val1.version_ref());
    EXPECT_LT(10000, *val1.ttl_ref());
    EXPECT_EQ(5, *val1.ttlVersion_ref()); /* Reflects updated TTL version */

    std::atomic<bool> newTtlVersionSeen = false;
    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(
                folly::getEventBase(), [keyvals, &newTtlVersionSeen](auto&& t) {
                  if (not t.hasValue()) {
                    return;
                  }

                  for (const auto& kv : keyvals) {
                    if (not t->keyVals_ref()->count(kv.first)) {
                      continue;
                    }
                    auto& pub = *t;
                    ASSERT_EQ(1, (*pub.keyVals_ref()).count(kv.first));
                    if ((*pub.keyVals_ref()).count("key1") == 1) {
                      const auto& val = (*pub.keyVals_ref())["key1"];
                      if (*val.ttlVersion_ref() == 6) {
                        newTtlVersionSeen = true;
                        /* TTL update has no value */
                        EXPECT_EQ(false, val.value_ref().has_value());
                        EXPECT_EQ(1, (*pub.keyVals_ref()).size());
                      }
                    }
                  }
                });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());

    // TTL update
    auto thriftValue2 = value;
    thriftValue2.value_ref().reset();
    thriftValue2.ttl_ref() = 50000;
    *thriftValue2.ttlVersion_ref() += 1;
    kvStoreWrapper_->setKey(kSpineAreaId, key, thriftValue2);

    // Wait until new TTL version is seen.
    while (not newTtlVersionSeen) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  // ignoreTTL = true is specified in filter.
  // Client should not receive publication associated with TTL update
  {
    const std::string key{"key3"};
    thrift::KeyDumpParams filter;
    filter.keys_ref() = {key};
    filter.ignoreTtl_ref() = true;
    filter.originatorIds_ref()->insert("node3");
    filter.originatorIds_ref()->insert("node33");
    std::unordered_map<std::string, std::string> keyvals;
    keyvals[key] = "value3";
    filter.oper_ref() = thrift::FilterOperator::AND;

    const auto value = createThriftValue(
        1 /* version */,
        "node3",
        "value3",
        20000 /* ttl*/,
        5 /* ttl version */,
        0 /* hash */);

    auto thriftValue = value;
    thriftValue.value_ref().reset();
    kvStoreWrapper_->setKey(kSpineAreaId, "key3", thriftValue);
    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetAreaKvStores(
                std::make_unique<thrift::KeyDumpParams>(filter),
                std::make_unique<std::set<std::string>>(kSpineOnlySet))
            .get();

    EXPECT_LE(
        3, (*responseAndSubscription.response.begin()->keyVals_ref()).size());
    for (const auto& key_ : {"key3", "key33", "key333"}) {
      EXPECT_EQ(
          1,
          (*responseAndSubscription.response.begin()->keyVals_ref())
              .count(key_));
    }

    EXPECT_EQ(
        0,
        (*responseAndSubscription.response.begin()->keyVals_ref())
            .count("key2"));
    const auto& val1 =
        (*responseAndSubscription.response.begin()->keyVals_ref())["key3"];
    ASSERT_EQ(true, val1.value_ref().has_value());
    EXPECT_EQ(1, *val1.version_ref());
    EXPECT_LT(10000, *val1.ttl_ref());
    EXPECT_EQ(5, *val1.ttlVersion_ref()); /* Reflects updated TTL version */

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStreamUnsafeDoNotUse()
            .subscribeExTry(folly::getEventBase(), [keyvals](auto&& t) {
              if (not t.hasValue()) {
                return;
              }

              for (const auto& kv : keyvals) {
                if (not t->keyVals_ref()->count(kv.first)) {
                  continue;
                }
                auto& pub = *t;
                EXPECT_EQ(1, (*pub.keyVals_ref()).size());
                ASSERT_EQ(1, (*pub.keyVals_ref()).count(kv.first));
                if ((*pub.keyVals_ref()).count("key3") == 1) {
                  const auto& val = (*pub.keyVals_ref())["key3"];
                  EXPECT_LE(6, *val.ttlVersion_ref());
                }
              }
            });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());

    // TTL update
    auto thriftValue2 = value;
    thriftValue2.value_ref().reset();
    thriftValue2.ttl_ref() = 30000;
    *thriftValue2.ttlVersion_ref() += 1;
    /* No TTL update message should be received */
    kvStoreWrapper_->setKey(kSpineAreaId, key, thriftValue2);

    /* Check that the TTL version is updated */
    std::vector<std::string> filterKeys{key};
    thrift::Publication pub;
    client_->sync_getKvStoreKeyValsArea(pub, filterKeys, kSpineAreaId);
    EXPECT_EQ(1, (*pub.keyVals_ref()).size());
    EXPECT_EQ(1, *((*pub.keyVals_ref()).at(key).version_ref()));
    EXPECT_EQ(true, (*pub.keyVals_ref()).at(key).value_ref().has_value());
    EXPECT_EQ(
        thriftValue2.ttlVersion_ref(),
        (*pub.keyVals_ref()).at(key).ttlVersion_ref());

    // Check we should receive 0 update.
    std::this_thread::yield();

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler->getNumKvStorePublishers() != 0) {
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
  filter.ignoreTtl_ref() = false;
  filter.doNotPublishValue_ref() = true;

  auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
  auto responseAndSubscription =
      handler
          ->semifuture_subscribeAndGetAreaKvStores(
              std::make_unique<thrift::KeyDumpParams>(filter),
              std::make_unique<std::set<std::string>>(kSpineOnlySet))
          .get();

  auto initialPub = responseAndSubscription.response.begin();
  EXPECT_EQ(2, (*initialPub->keyVals_ref()).size());
  // Verify timestamp is set
  EXPECT_TRUE(initialPub->timestamp_ms_ref().has_value());
  for (const auto& key_ : {"key1", "key2"}) {
    EXPECT_EQ(1, (*initialPub->keyVals_ref()).count(key_));
    const auto& val1 = (*initialPub->keyVals_ref())[key_];
    ASSERT_EQ(false, val1.value_ref().has_value()); /* value is null */
    EXPECT_EQ(1, *val1.version_ref());
    EXPECT_LT(10000, *val1.ttl_ref());
    EXPECT_EQ(1, *val1.ttlVersion_ref());
  }

  std::atomic<bool> newUpdateSeen = false;
  // Test key which gets updated.
  auto test_key = "key1";

  auto subscription =
      std::move(responseAndSubscription.stream)
          .toClientStreamUnsafeDoNotUse()
          .subscribeExTry(
              folly::getEventBase(), [&test_key, &newUpdateSeen](auto&& t) {
                if (not t.hasValue()) {
                  return;
                }

                auto& pub = *t;
                ASSERT_EQ(1, (*pub.keyVals_ref()).count(test_key));
                const auto& val = (*pub.keyVals_ref())[test_key];
                newUpdateSeen = true;
                // Verify no value seen in update
                ASSERT_EQ(false, val.value_ref().has_value());
                EXPECT_EQ(2, *val.ttlVersion_ref());
                // Verify timestamp is set
                EXPECT_TRUE(pub.timestamp_ms_ref().has_value());
              });

  EXPECT_EQ(1, handler->getNumKvStorePublishers());

  // Update value and publish to verify incremental update also filters value
  auto thriftValue2 = keyVals[test_key];
  thriftValue2.value_ref() = "value_updated";
  thriftValue2.ttl_ref() = 50000;
  *thriftValue2.ttlVersion_ref() += 1;
  kvStoreWrapper_->setKey(kSpineAreaId, test_key, thriftValue2);

  // Wait until new update is seen by stream subscriber
  while (not newUpdateSeen) {
    std::this_thread::yield();
  }

  // Cancel subscription
  subscription.cancel();
  std::move(subscription).detach();

  // Wait until publisher is destroyed
  while (handler->getNumKvStorePublishers() != 0) {
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
    client_->sync_setNodeOverload();
    client_->sync_unsetNodeOverload();
  }

  {
    client_->sync_setInterfaceOverload(ifName);
    client_->sync_unsetInterfaceOverload(ifName);
  }

  {
    client_->sync_setInterfaceMetric(ifName, 110);
    client_->sync_unsetInterfaceMetric(ifName);
  }

  {
    client_->sync_setAdjacencyMetric(ifName, adjName, 110);
    client_->sync_unsetAdjacencyMetric(ifName, adjName);
  }

  {
    thrift::DumpLinksReply reply;
    client_->sync_getInterfaces(reply);
    EXPECT_EQ(nodeName_, reply.thisNodeName_ref());
    EXPECT_FALSE(*reply.isOverloaded_ref());
    EXPECT_EQ(1, reply.interfaceDetails_ref()->size());
  }

  {
    thrift::OpenrVersions ret;
    client_->sync_getOpenrVersion(ret);
    EXPECT_LE(*ret.lowestSupportedVersion_ref(), *ret.version_ref());
  }

  {
    thrift::BuildInfo info;
    client_->sync_getBuildInfo(info);
    EXPECT_NE("", *info.buildMode_ref());
  }

  {
    std::vector<thrift::AdjacencyDatabase> adjDbs;
    thrift::AdjacenciesFilter filter;
    filter.selectAreas_ref() = {kSpineAreaId};
    client_->sync_getLinkMonitorAdjacenciesFiltered(adjDbs, filter);
    EXPECT_EQ(0, adjDbs.begin()->get_adjacencies().size());
  }
}

TEST_F(OpenrCtrlFixture, PersistentStoreApis) {
  {
    const std::string key = "key1";
    const std::string value = "value1";
    client_->sync_setConfigKey(key, value);
  }

  {
    const std::string key = "key2";
    const std::string value = "value2";
    client_->sync_setConfigKey(key, value);
  }

  {
    const std::string key = "key1";
    client_->sync_eraseConfigKey(key);
  }

  {
    const std::string key = "key2";
    std::string ret = "";
    client_->sync_getConfigKey(ret, key);
    EXPECT_EQ("value2", ret);
  }

  {
    const std::string key = "key1";
    std::string ret = "";
    EXPECT_THROW(client_->sync_getConfigKey(ret, key), thrift::OpenrError);
  }
}

TEST_F(OpenrCtrlFixture, RibPolicy) {
  // Set API
  {
    // Create valid rib policy
    thrift::RibRouteActionWeight actionWeight;
    actionWeight.area_to_weight_ref()->emplace("test-area", 2);
    actionWeight.neighbor_to_weight_ref()->emplace("nbr", 3);
    thrift::RibPolicyStatement policyStatement;
    policyStatement.matcher_ref()->prefixes_ref() =
        std::vector<thrift::IpPrefix>();
    policyStatement.action_ref()->set_weight_ref() = actionWeight;
    thrift::RibPolicy policy;
    policy.statements_ref()->emplace_back(policyStatement);
    policy.ttl_secs_ref() = 1;

    EXPECT_NO_THROW(client_->sync_setRibPolicy(policy));
  }

  // Get API
  {
    thrift::RibPolicy policy;
    EXPECT_NO_THROW(client_->sync_getRibPolicy(policy));
  }

  // Clear API
  {
    // Clear Rib Policy and expect no error as rib policy exists
    EXPECT_NO_THROW(client_->sync_clearRibPolicy());

    // An attempt to clear non-existing rib policy will show a message.
    EXPECT_THROW(client_->sync_clearRibPolicy(), thrift::OpenrError);
  }

  // Using Get API after clearing rib policy will show a message.
  {
    thrift::RibPolicy policy;
    EXPECT_THROW(client_->sync_getRibPolicy(policy), thrift::OpenrError);
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
