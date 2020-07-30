/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdio>
#include <thread>

#include <fbzmq/zmq/Context.h>
#include <folly/init/Init.h>
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
#include <openr/platform/PlatformPublisher.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/OpenrThriftServerWrapper.h>
#include <openr/tests/mocks/MockNetlinkSystemHandler.h>

using namespace openr;

class OpenrCtrlFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    std::vector<openr::thrift::AreaConfig> areaConfig;
    for (auto id : {"0", "plane", "pod"}) {
      thrift::AreaConfig area;
      area.area_id = id;
      area.neighbor_regexes.emplace_back(".*");
      areaConfig.emplace_back(std::move(area));
    }
    // create config
    auto tConfig = getBasicOpenrConfig(
        nodeName,
        "domain",
        areaConfig,
        true /* enableV4 */,
        true /* enableSegmentRouting */);

    // kvstore config
    tConfig.kvstore_config.sync_interval_s = 1;
    tConfig.kvstore_config.enable_flood_optimization_ref() = true;
    tConfig.kvstore_config.is_flood_root_ref() = true;
    // link monitor config
    auto& lmConf = tConfig.link_monitor_config;
    lmConf.linkflap_initial_backoff_ms = 1;
    lmConf.linkflap_max_backoff_ms = 8;
    lmConf.use_rtt_metric = false;
    lmConf.include_interface_regexes = {"po.*"};
    config = std::make_shared<Config>(tConfig);

    // Create PersistentStore
    std::string const configStoreFile = "/tmp/openr-ctrl-handler-test.bin";
    // start fresh
    std::remove(configStoreFile.data());
    persistentStore =
        std::make_unique<PersistentStore>(configStoreFile, true /* dryrun */);
    persistentStoreThread_ = std::thread([&]() { persistentStore->run(); });

    // Create KvStore module
    kvStoreWrapper = std::make_unique<KvStoreWrapper>(context_, config);
    kvStoreWrapper->run();

    // Create Decision module
    decision = std::make_shared<Decision>(
        config,
        true, /* computeLfaPaths */
        false, /* bgpDryRun */
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(500),
        kvStoreWrapper->getReader(),
        staticRoutesUpdatesQueue_.getReader(),
        routeUpdatesQueue_);
    decisionThread_ = std::thread([&]() { decision->run(); });

    // Create Fib module
    fib = std::make_shared<Fib>(
        config,
        -1, /* thrift port */
        std::chrono::seconds(2),
        routeUpdatesQueue_.getReader(),
        interfaceUpdatesQueue_.getReader(),
        MonitorSubmitUrl{"inproc://monitor-sub"},
        kvStoreWrapper->getKvStore(),
        context_);
    fibThread_ = std::thread([&]() { fib->run(); });

    // Create PrefixManager module
    prefixManager = std::make_shared<PrefixManager>(
        prefixUpdatesQueue_.getReader(),
        config,
        persistentStore.get(),
        kvStoreWrapper->getKvStore(),
        false,
        std::chrono::seconds(0));
    prefixManagerThread_ = std::thread([&]() { prefixManager->run(); });

    // create fakeNetlinkProtocolSocket
    nlSock_ = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&evb_);

    // Create MockNetlinkSystemHandler
    mockNlHandler_ = std::make_shared<MockNetlinkSystemHandler>(nlSock_.get());

    // Setup PlatformPublisher
    platformPublisher_ = std::make_unique<PlatformPublisher>(
        context_, platformPubUrl_, nlSock_.get());

    // Create LinkMonitor
    re2::RE2::Options regexOpts;
    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add("po.*", &regexErr);
    includeRegexList->Compile();

    linkMonitor = std::make_shared<LinkMonitor>(
        context_,
        config,
        mockNlHandler_,
        kvStoreWrapper->getKvStore(),
        false /* enable perf measurement */,
        interfaceUpdatesQueue_,
        peerUpdatesQueue_,
        neighborUpdatesQueue_.getReader(),
        monitorSubmitUrl_,
        persistentStore.get(),
        false,
        prefixUpdatesQueue_,
        platformPubUrl_,
        std::chrono::seconds(1));
    linkMonitorThread_ = std::thread([&]() { linkMonitor->run(); });

    // spin up an openrThriftServer
    openrThriftServerWrapper_ = std::make_shared<OpenrThriftServerWrapper>(
        nodeName,
        decision.get() /* decision */,
        fib.get() /* fib */,
        kvStoreWrapper->getKvStore() /* kvStore */,
        linkMonitor.get() /* linkMonitor */,
        persistentStore.get() /* configStore */,
        prefixManager.get() /* prefixManager */,
        nullptr /* config */,
        monitorSubmitUrl_,
        context_);
    openrThriftServerWrapper_->run();

    // initialize openrCtrlClient talking to server
    openrCtrlThriftClient_ =
        getOpenrCtrlPlainTextClient<apache::thrift::HeaderClientChannel>(
            evb_,
            folly::IPAddress("::1"),
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
    kvStoreWrapper->closeQueue();

    openrThriftServerWrapper_->stop();

    linkMonitor->stop();
    linkMonitorThread_.join();

    persistentStore->stop();
    persistentStoreThread_.join();

    prefixManager->stop();
    prefixManagerThread_.join();

    platformPublisher_->stop();
    mockNlHandler_.reset();
    nlSock_.reset();

    fib->stop();
    fibThread_.join();

    decision->stop();
    decisionThread_.join();

    kvStoreWrapper->stop();
  }

  thrift::PeerSpec
  createPeerSpec(const std::string& cmdUrl) {
    thrift::PeerSpec peerSpec;
    peerSpec.cmdUrl = cmdUrl;
    return peerSpec;
  }

  thrift::PrefixEntry
  createPrefixEntry(const std::string& prefix, thrift::PrefixType prefixType) {
    thrift::PrefixEntry prefixEntry;
    prefixEntry.prefix = toIpPrefix(prefix);
    prefixEntry.type = prefixType;
    return prefixEntry;
  }

 private:
  const MonitorSubmitUrl monitorSubmitUrl_{"inproc://monitor-submit-url"};
  const PlatformPublisherUrl platformPubUrl_{"inproc://platform-pub-url"};

  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue_;
  messaging::ReplicateQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue_;
  messaging::ReplicateQueue<thrift::PeerUpdateRequest> peerUpdatesQueue_;
  messaging::ReplicateQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue_;
  messaging::ReplicateQueue<thrift::PrefixUpdateRequest> prefixUpdatesQueue_;
  messaging::ReplicateQueue<thrift::RouteDatabaseDelta>
      staticRoutesUpdatesQueue_;

  fbzmq::Context context_{};
  folly::EventBase evb_;
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock_{nullptr};
  std::unique_ptr<PlatformPublisher> platformPublisher_{nullptr};

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

 public:
  const std::string nodeName{"thanos@universe"};
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<MockNetlinkSystemHandler> mockNlHandler_;
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper_{nullptr};
  std::unique_ptr<openr::thrift::OpenrCtrlCppAsyncClient>
      openrCtrlThriftClient_{nullptr};
};

TEST_F(OpenrCtrlFixture, getMyNodeName) {
  std::string res = "";
  openrCtrlThriftClient_->sync_getMyNodeName(res);
  EXPECT_EQ(nodeName, res);
}

TEST_F(OpenrCtrlFixture, PrefixManagerApis) {
  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("10.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("11.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("20.0.0.0/8", thrift::PrefixType::BGP),
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    openrCtrlThriftClient_->sync_advertisePrefixes(
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    openrCtrlThriftClient_->sync_withdrawPrefixes(
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
    openrCtrlThriftClient_->sync_withdrawPrefixesByType(
        thrift::PrefixType::LOOPBACK);
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    openrCtrlThriftClient_->sync_syncPrefixesByType(
        thrift::PrefixType::BGP,
        std::vector<thrift::PrefixEntry>{std::move(prefixes)});
  }

  {
    const std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    std::vector<thrift::PrefixEntry> res;
    openrCtrlThriftClient_->sync_getPrefixes(res);
    EXPECT_EQ(prefixes, res);
  }

  {
    std::vector<thrift::PrefixEntry> res;
    openrCtrlThriftClient_->sync_getPrefixesByType(
        res, thrift::PrefixType::LOOPBACK);
    EXPECT_EQ(0, res.size());
  }
}

TEST_F(OpenrCtrlFixture, RouteApis) {
  {
    thrift::RouteDatabase db;
    openrCtrlThriftClient_->sync_getRouteDb(db);
    EXPECT_EQ(nodeName, db.thisNodeName);
    EXPECT_EQ(0, db.unicastRoutes.size());
    EXPECT_EQ(0, db.mplsRoutes.size());
  }

  {
    thrift::RouteDatabase db;
    openrCtrlThriftClient_->sync_getRouteDbComputed(db, nodeName);
    EXPECT_EQ(nodeName, db.thisNodeName);
    EXPECT_EQ(0, db.unicastRoutes.size());
    EXPECT_EQ(0, db.mplsRoutes.size());
  }

  {
    const std::string testNode("avengers@universe");
    thrift::RouteDatabase db;
    openrCtrlThriftClient_->sync_getRouteDbComputed(db, testNode);
    EXPECT_EQ(testNode, db.thisNodeName);
    EXPECT_EQ(0, db.unicastRoutes.size());
    EXPECT_EQ(0, db.mplsRoutes.size());
  }

  {
    std::vector<thrift::UnicastRoute> filterRet;
    std::vector<std::string> prefixes{"10.46.2.0", "10.46.2.0/24"};
    openrCtrlThriftClient_->sync_getUnicastRoutesFiltered(filterRet, prefixes);
    EXPECT_EQ(0, filterRet.size());
  }

  {
    std::vector<thrift::UnicastRoute> allRouteRet;
    openrCtrlThriftClient_->sync_getUnicastRoutes(allRouteRet);
    EXPECT_EQ(0, allRouteRet.size());
  }
  {
    std::vector<thrift::MplsRoute> filterRet;
    std::vector<std::int32_t> labels{1, 2};
    openrCtrlThriftClient_->sync_getMplsRoutesFiltered(filterRet, labels);
    EXPECT_EQ(0, filterRet.size());
  }
  {
    std::vector<thrift::MplsRoute> allRouteRet;
    openrCtrlThriftClient_->sync_getMplsRoutes(allRouteRet);
    EXPECT_EQ(0, allRouteRet.size());
  }
}

TEST_F(OpenrCtrlFixture, PerfApis) {
  thrift::PerfDatabase db;
  openrCtrlThriftClient_->sync_getPerfDb(db);
  EXPECT_EQ(nodeName, db.thisNodeName);
}

TEST_F(OpenrCtrlFixture, DecisionApis) {
  {
    thrift::AdjDbs db;
    openrCtrlThriftClient_->sync_getDecisionAdjacencyDbs(db);
    EXPECT_EQ(0, db.size());
  }

  {
    thrift::PrefixDbs db;
    openrCtrlThriftClient_->sync_getDecisionPrefixDbs(db);
    EXPECT_EQ(0, db.size());
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
    thrift::AreasConfig area;
    openrCtrlThriftClient_->sync_getAreasConfig(area);
    EXPECT_EQ(3, (*area.areas_ref()).size());
    EXPECT_EQ((*area.areas_ref()).count("plane"), 1);
    EXPECT_EQ((*area.areas_ref()).count("pod"), 1);
    EXPECT_EQ(
        (*area.areas_ref()).count(thrift::KvStore_constants::kDefaultArea()),
        1);
    EXPECT_EQ((*area.areas_ref()).count("none"), 0);
  }

  //
  // Key set/get
  //
  {
    thrift::KeySetParams setParams;
    setParams.keyVals_ref() = keyVals;

    openrCtrlThriftClient_->sync_setKvStoreKeyVals(
        setParams, thrift::KvStore_constants::kDefaultArea());

    setParams.solicitResponse_ref() = false;
    openrCtrlThriftClient_->sync_setKvStoreKeyVals(
        setParams, thrift::KvStore_constants::kDefaultArea());

    thrift::KeySetParams setParamsPod;
    setParamsPod.keyVals_ref() = keyValsPod;
    openrCtrlThriftClient_->sync_setKvStoreKeyVals(setParamsPod, "pod");

    thrift::KeySetParams setParamsPlane;
    setParamsPlane.keyVals_ref() = keyValsPlane;
    openrCtrlThriftClient_->sync_setKvStoreKeyVals(setParamsPlane, "plane");
  }

  {
    std::vector<std::string> filterKeys{"key11", "key2"};
    thrift::Publication pub;
    openrCtrlThriftClient_->sync_getKvStoreKeyVals(pub, filterKeys);
    EXPECT_EQ(2, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key2"), pub.keyVals["key2"]);
    EXPECT_EQ(keyVals.at("key11"), pub.keyVals["key11"]);
  }

  // pod keys
  {
    std::vector<std::string> filterKeys{"keyPod1"};
    thrift::Publication pub;
    openrCtrlThriftClient_->sync_getKvStoreKeyValsArea(pub, filterKeys, "pod");
    EXPECT_EQ(1, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyValsPod.at("keyPod1"), pub.keyVals["keyPod1"]);
  }

  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    params.prefix = "key3";
    params.originatorIds.insert("node3");
    params.keys_ref() = {"key3"};

    openrCtrlThriftClient_->sync_getKvStoreKeyValsFiltered(pub, params);
    EXPECT_EQ(3, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key3"), pub.keyVals["key3"]);
    EXPECT_EQ(keyVals.at("key33"), pub.keyVals["key33"]);
    EXPECT_EQ(keyVals.at("key333"), pub.keyVals["key333"]);
  }

  // with areas
  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    params.prefix = "keyP";
    params.originatorIds.insert("node1");
    params.keys_ref() = {"keyP"};

    openrCtrlThriftClient_->sync_getKvStoreKeyValsFilteredArea(
        pub, params, "plane");
    EXPECT_EQ(2, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyValsPlane.at("keyPlane1"), pub.keyVals["keyPlane1"]);
    EXPECT_EQ(keyValsPlane.at("keyPlane2"), pub.keyVals["keyPlane2"]);
  }

  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    params.prefix = "key3";
    params.originatorIds.insert("node3");
    params.keys_ref() = {"key3"};

    openrCtrlThriftClient_->sync_getKvStoreHashFiltered(pub, params);
    EXPECT_EQ(3, (*pub.keyVals_ref()).size());
    auto value3 = keyVals.at("key3");
    value3.value_ref().reset();
    auto value33 = keyVals.at("key33");
    value33.value_ref().reset();
    auto value333 = keyVals.at("key333");
    value333.value_ref().reset();
    EXPECT_EQ(value3, pub.keyVals["key3"]);
    EXPECT_EQ(value33, pub.keyVals["key33"]);
    EXPECT_EQ(value333, pub.keyVals["key333"]);
  }

  //
  // Dual and Flooding APIs
  //
  {
    thrift::DualMessages messages;
    openrCtrlThriftClient_->sync_processKvStoreDualMessage(
        messages, thrift::KvStore_constants::kDefaultArea());
  }

  {
    thrift::FloodTopoSetParams params;
    params.rootId = nodeName;
    openrCtrlThriftClient_->sync_updateFloodTopologyChild(
        params, thrift::KvStore_constants::kDefaultArea());
  }

  {
    thrift::SptInfos ret;
    openrCtrlThriftClient_->sync_getSpanningTreeInfos(
        ret, thrift::KvStore_constants::kDefaultArea());
    EXPECT_EQ(1, ret.infos.size());
    ASSERT_NE(ret.infos.end(), ret.infos.find(nodeName));
    EXPECT_EQ(0, ret.counters.neighborCounters.size());
    EXPECT_EQ(1, ret.counters.rootCounters.size());
    EXPECT_EQ(nodeName, ret.floodRootId_ref());
    EXPECT_EQ(0, ret.floodPeers.size());

    thrift::SptInfo sptInfo = ret.infos.at(nodeName);
    EXPECT_EQ(0, sptInfo.cost);
    ASSERT_TRUE(sptInfo.parent_ref().has_value());
    EXPECT_EQ(nodeName, sptInfo.parent_ref().value());
    EXPECT_EQ(0, sptInfo.children.size());
  }

  //
  // Peers APIs
  //
  const thrift::PeersMap peers{{"peer1", createPeerSpec("inproc://peer1-cmd")},
                               {"peer2", createPeerSpec("inproc://peer2-cmd")},
                               {"peer3", createPeerSpec("inproc://peer3-cmd")}};

  // do the same with non-default area
  const thrift::PeersMap peersPod{
      {"peer11", createPeerSpec("inproc://peer11-cmd")},
      {"peer21", createPeerSpec("inproc://peer21-cmd")},
  };

  {
    for (auto& peer : peers) {
      kvStoreWrapper->addPeer(peer.first, peer.second);
    }
    for (auto& peerPod : peersPod) {
      kvStoreWrapper->addPeer(peerPod.first, peerPod.second, "pod");
    }

    thrift::PeersMap ret;
    openrCtrlThriftClient_->sync_getKvStorePeersArea(
        ret, thrift::KvStore_constants::kDefaultArea());

    EXPECT_EQ(3, ret.size());
    EXPECT_EQ(peers.at("peer1"), ret.at("peer1"));
    EXPECT_EQ(peers.at("peer2"), ret.at("peer2"));
    EXPECT_EQ(peers.at("peer3"), ret.at("peer3"));
  }

  {
    kvStoreWrapper->delPeer("peer2");

    thrift::PeersMap ret;
    openrCtrlThriftClient_->sync_getKvStorePeersArea(
        ret, thrift::KvStore_constants::kDefaultArea());
    EXPECT_EQ(2, ret.size());
    EXPECT_EQ(peers.at("peer1"), ret.at("peer1"));
    EXPECT_EQ(peers.at("peer3"), ret.at("peer3"));
  }

  {
    thrift::PeersMap ret;
    openrCtrlThriftClient_->sync_getKvStorePeersArea(ret, "pod");

    EXPECT_EQ(2, ret.size());
    EXPECT_EQ(peersPod.at("peer11"), ret.at("peer11"));
    EXPECT_EQ(peersPod.at("peer21"), ret.at("peer21"));
  }

  {
    kvStoreWrapper->delPeer("peer21", "pod");

    thrift::PeersMap ret;
    openrCtrlThriftClient_->sync_getKvStorePeersArea(ret, "pod");
    EXPECT_EQ(1, ret.size());
    EXPECT_EQ(peersPod.at("peer11"), ret.at("peer11"));
    EXPECT_EQ(ret.count("peer21"), 0);
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
    (*params.originatorIds_ref()).insert("node3");
    params.keys_ref() = {"key3"};

    openrCtrlThriftClient_->sync_getKvStoreKeyValsFiltered(pub, params);
    EXPECT_EQ(3, (*pub.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key3"), (*pub.keyVals_ref())["key3"]);
    EXPECT_EQ(keyVals.at("key33"), (*pub.keyVals_ref())["key33"]);
    EXPECT_EQ(keyVals.at("key333"), (*pub.keyVals_ref())["key333"]);

    params33.originatorIds_ref() = {"node33"};
    params33.keys_ref() = {"key33"};
    openrCtrlThriftClient_->sync_getKvStoreKeyValsFiltered(pub33, params33);
    EXPECT_EQ(2, (*pub33.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key33"), (*pub33.keyVals_ref())["key33"]);
    EXPECT_EQ(keyVals.at("key333"), (*pub33.keyVals_ref())["key333"]);

    // Two updates because the operator is OR and originator ids for keys
    // key33 and key333 are same.
    params333.originatorIds_ref() = {"node33"};
    params333.keys_ref() = {"key333"};
    openrCtrlThriftClient_->sync_getKvStoreKeyValsFiltered(pub333, params333);
    EXPECT_EQ(2, (*pub333.keyVals_ref()).size());
    EXPECT_EQ(keyVals.at("key33"), (*pub33.keyVals_ref())["key33"]);
    EXPECT_EQ(keyVals.at("key333"), (*pub333.keyVals_ref())["key333"]);
  }

  // with areas but do not use prefix (to be deprecated). use prefixes/keys
  // instead.
  {
    thrift::Publication pub;
    thrift::KeyDumpParams params;
    (*params.originatorIds_ref()).insert("node1");
    params.keys_ref() = {"keyP", "keyPl"};

    openrCtrlThriftClient_->sync_getKvStoreKeyValsFilteredArea(
        pub, params, "plane");
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

    openrCtrlThriftClient_->sync_getKvStoreHashFiltered(pub, params);
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

  //
  // Subscribe and Get API
  //
  {
    // Add more keys and values
    const std::string key{"snoop-key"};
    kvStoreWrapper->setKey(
        key, createThriftValue(1, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(1, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(2, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(3, "node1", std::string("value1")));

    std::vector<std::string> filterKeys{key};
    thrift::Publication pub;
    openrCtrlThriftClient_->sync_getKvStoreKeyVals(pub, filterKeys);
    EXPECT_EQ(1, (*pub.keyVals_ref()).size());
    EXPECT_EQ(3, *((*pub.keyVals_ref()).at(key).version_ref()));
    EXPECT_EQ("value1", (*pub.keyVals_ref()).at(key).value_ref().value());
  }

  {
    const std::string key{"snoop-key"};

    std::atomic<int> received{0};
    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler->semifuture_subscribeAndGetKvStore().get();
    // Expect 10 keys in the initial dump
    // NOTE: there may be extra keys from PrefixManager & LinkMonitor)
    EXPECT_LE(10, (*responseAndSubscription.response.keyVals_ref()).size());
    ASSERT_EQ(1, (*responseAndSubscription.response.keyVals_ref()).count(key));
    EXPECT_EQ(
        responseAndSubscription.response.keyVals.at(key),
        createThriftValue(3, "node1", std::string("value1")));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() or not t->keyVals.count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals_ref()).size());
              ASSERT_EQ(1, (*pub.keyVals_ref()).count(key));
              EXPECT_EQ(
                  "value1", (*pub.keyVals_ref()).at(key).value_ref().value());
              EXPECT_EQ(received + 4, (*pub.keyVals_ref()).at(key).version);
              received++;
            });
    EXPECT_EQ(1, handler->getNumKvStorePublishers());
    kvStoreWrapper->setKey(
        key, createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(5, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(6, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key,
        createThriftValue(7, "node1", std::string("value1")),
        std::nullopt,
        "pod");
    kvStoreWrapper->setKey(
        key,
        createThriftValue(8, "node1", std::string("value1")),
        std::nullopt,
        "plane");

    // Check we should receive-3 updates
    while (received < 5) {
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
  // AND operator is used.
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
            ->semifuture_subscribeAndGetKvStoreFiltered(
                std::make_unique<thrift::KeyDumpParams>(filter))
            .get();

    auto responseAndSubscription_other =
        handler_other
            ->semifuture_subscribeAndGetKvStoreFiltered(
                std::make_unique<thrift::KeyDumpParams>(filter))
            .get();

    EXPECT_LE(0, (*responseAndSubscription.response.keyVals_ref()).size());
    ASSERT_EQ(0, (*responseAndSubscription.response.keyVals_ref()).count(key));
    EXPECT_LE(
        0, (*responseAndSubscription_other.response.keyVals_ref()).size());
    ASSERT_EQ(
        0, (*responseAndSubscription_other.response.keyVals_ref()).count(key));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() or not t->keyVals.count(key)) {
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
            .toClientStream()
            .subscribeExTry(
                folly::getEventBase(), [&received, random_key](auto&& t) {
                  if (!t.hasValue() or not t->keyVals.count(random_key)) {
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

    EXPECT_EQ(2, handler->getNumKvStorePublishers());
    EXPECT_EQ(2, handler_other->getNumKvStorePublishers());
    kvStoreWrapper->setKey(
        key, createThriftValue(1, "node1", std::string("value4")));
    kvStoreWrapper->setKey(
        random_key, createThriftValue(1, "node1", std::string("value_random")));

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
    std::vector<std::string> keys = {"key33"};
    thrift::KeyDumpParams filter;
    filter.keys_ref() = keys;
    filter.originatorIds_ref() = {"node1", "node2", "node3", "node33"};

    filter.oper_ref() = thrift::FilterOperator::AND;

    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetKvStoreFiltered(
                std::make_unique<thrift::KeyDumpParams>(filter))
            .get();

    EXPECT_LE(2, responseAndSubscription.response.keyVals.size());
    ASSERT_EQ(1, responseAndSubscription.response.keyVals.count(key));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              // Consider publication only if `key` is present
              // NOTE: There can be updates to prefix or adj keys
              if (!t.hasValue() or not t->keyVals.count(key)) {
                return;
              }
              auto& pub = *t;
              EXPECT_EQ(1, (*pub.keyVals_ref()).size());
              ASSERT_EQ(1, (*pub.keyVals_ref()).count(key));
              EXPECT_EQ(
                  "value333", (*pub.keyVals_ref()).at(key).value_ref().value());
              received++;
            });

    EXPECT_EQ(1, handler->getNumKvStorePublishers());
    kvStoreWrapper->setKey(
        key, createThriftValue(2, "node33", std::string("value333")));

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
            ->semifuture_subscribeAndGetKvStoreFiltered(
                std::make_unique<thrift::KeyDumpParams>(filter))
            .get();

    EXPECT_LE(2, (*responseAndSubscription.response.keyVals_ref()).size());
    ASSERT_EQ(
        1, (*responseAndSubscription.response.keyVals_ref()).count("key33"));
    ASSERT_EQ(
        1, (*responseAndSubscription.response.keyVals_ref()).count("key333"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (not t.hasValue()) {
                    return;
                  }

                  for (const auto& kv : keyvals) {
                    if (not t->keyVals.count(kv.first)) {
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
    kvStoreWrapper->setKey(
        "key333", createThriftValue(3, "node33", std::string("value333")));
    kvStoreWrapper->setKey(
        "key33", createThriftValue(3, "node33", std::string("value33")));

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
            ->semifuture_subscribeAndGetKvStoreFiltered(
                std::make_unique<thrift::KeyDumpParams>(filter))
            .get();

    EXPECT_LE(3, (*responseAndSubscription.response.keyVals_ref()).size());
    EXPECT_EQ(0, (*responseAndSubscription.response.keyVals_ref()).count(key));
    ASSERT_EQ(
        1, (*responseAndSubscription.response.keyVals_ref()).count("key1"));
    ASSERT_EQ(
        1, (*responseAndSubscription.response.keyVals_ref()).count("key3"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (!t.hasValue()) {
                    return;
                  }

                  bool found = false;
                  auto& pub = *t;
                  for (const auto& kv : keyvals) {
                    if (t->keyVals.count(kv.first)) {
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
    kvStoreWrapper->setKey(
        key, createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        "key1", createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        "key3", createThriftValue(4, "node3", std::string("value3")));

    // Check we should receive-1 updates
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
            ->semifuture_subscribeAndGetKvStoreFiltered(
                std::make_unique<thrift::KeyDumpParams>(filter))
            .get();

    EXPECT_LE(3, (*responseAndSubscription.response.keyVals_ref()).size());
    EXPECT_EQ(1, (*responseAndSubscription.response.keyVals_ref()).count(key));
    ASSERT_EQ(
        1, (*responseAndSubscription.response.keyVals_ref()).count("key1"));
    ASSERT_EQ(
        1, (*responseAndSubscription.response.keyVals_ref()).count("key3"));

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (!t.hasValue()) {
                    return;
                  }

                  bool found = false;
                  auto& pub = *t;
                  for (const auto& kv : keyvals) {
                    if (t->keyVals.count(kv.first)) {
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
    kvStoreWrapper->setKey(
        key, createThriftValue(5, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        "key1", createThriftValue(5, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        "key3", createThriftValue(5, "node3", std::string("value3")));
    kvStoreWrapper->setKey(
        "random-prefix", createThriftValue(1, "node1", std::string("value1")));

    // Check we should receive-3 updates
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
    (*filter.originatorIds_ref()).insert("node10");
    filter.oper_ref() = thrift::FilterOperator::AND;

    auto handler = openrThriftServerWrapper_->getOpenrCtrlHandler();
    auto responseAndSubscription =
        handler
            ->semifuture_subscribeAndGetKvStoreFiltered(
                std::make_unique<thrift::KeyDumpParams>(filter))
            .get();

    EXPECT_LE(0, (*responseAndSubscription.response.keyVals_ref()).size());

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(folly::getEventBase(), [&received, key](auto&& t) {
              if (!t.hasValue() or not t->keyVals.count(key)) {
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
    kvStoreWrapper->setKey(
        key, createThriftValue(10, "node10", std::string("value1")));

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
  // No matching originator id
  // Operator OR is used. Matching is based on prefix keys only
  {
    std::atomic<int> received{0};
    const std::string key{"test_key"};
    std::vector<std::string> keys = {"key1", "key2", "key3", key};
    thrift::KeyDumpParams filter;
    filter.keys_ref() = keys;
    (*filter.originatorIds_ref()).insert("node10");
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
            ->semifuture_subscribeAndGetKvStoreFiltered(
                std::make_unique<thrift::KeyDumpParams>(filter))
            .get();

    EXPECT_LE(0, (*responseAndSubscription.response.keyVals_ref()).size());

    auto subscription =
        std::move(responseAndSubscription.stream)
            .toClientStream()
            .subscribeExTry(
                folly::getEventBase(), [&received, keyvals](auto&& t) {
                  if (not t.hasValue()) {
                    return;
                  }

                  for (const auto& kv : keyvals) {
                    if (not t->keyVals.count(kv.first)) {
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
    kvStoreWrapper->setKey(
        "key1", createThriftValue(20, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        "key2", createThriftValue(20, "node2", std::string("value2")));
    kvStoreWrapper->setKey(
        "key3", createThriftValue(20, "node3", std::string("value3")));
    kvStoreWrapper->setKey(
        key, createThriftValue(20, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
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

TEST_F(OpenrCtrlFixture, LinkMonitorApis) {
  // create an interface
  mockNlHandler_->sendLinkEvent("po1011", 100, true);
  const std::string ifName = "po1011";
  const std::string adjName = "night@king";

  {
    openrCtrlThriftClient_->sync_setNodeOverload();
    openrCtrlThriftClient_->sync_unsetNodeOverload();
  }

  {
    openrCtrlThriftClient_->sync_setInterfaceOverload(ifName);
    openrCtrlThriftClient_->sync_unsetInterfaceOverload(ifName);
  }

  {
    openrCtrlThriftClient_->sync_setInterfaceMetric(ifName, 110);
    openrCtrlThriftClient_->sync_unsetInterfaceMetric(ifName);
  }

  {
    openrCtrlThriftClient_->sync_setAdjacencyMetric(ifName, adjName, 110);
    openrCtrlThriftClient_->sync_unsetAdjacencyMetric(ifName, adjName);
  }

  {
    thrift::DumpLinksReply reply;
    openrCtrlThriftClient_->sync_getInterfaces(reply);
    EXPECT_EQ(nodeName, reply.thisNodeName);
    EXPECT_FALSE(reply.isOverloaded);
    EXPECT_EQ(1, reply.interfaceDetails.size());
  }

  {
    thrift::OpenrVersions ret;
    openrCtrlThriftClient_->sync_getOpenrVersion(ret);
    EXPECT_LE(ret.lowestSupportedVersion, ret.version);
  }

  {
    thrift::BuildInfo info;
    openrCtrlThriftClient_->sync_getBuildInfo(info);
    EXPECT_NE("", info.buildMode);
  }

  {
    thrift::AdjacencyDatabase adjDb;
    openrCtrlThriftClient_->sync_getLinkMonitorAdjacencies(adjDb);
    EXPECT_EQ(0, adjDb.adjacencies.size());
  }
}

TEST_F(OpenrCtrlFixture, PersistentStoreApis) {
  {
    const std::string key = "key1";
    const std::string value = "value1";
    openrCtrlThriftClient_->sync_setConfigKey(key, value);
  }

  {
    const std::string key = "key2";
    const std::string value = "value2";
    openrCtrlThriftClient_->sync_setConfigKey(key, value);
  }

  {
    const std::string key = "key1";
    openrCtrlThriftClient_->sync_eraseConfigKey(key);
  }

  {
    const std::string key = "key2";
    std::string ret = "";
    openrCtrlThriftClient_->sync_getConfigKey(ret, key);
    EXPECT_EQ("value2", ret);
  }

  {
    const std::string key = "key1";
    std::string ret = "";
    EXPECT_THROW(
        openrCtrlThriftClient_->sync_getConfigKey(ret, key),
        thrift::OpenrError);
  }
}

TEST_F(OpenrCtrlFixture, RibPolicy) {
  // Set API
  {
    // Create valid rib policy
    thrift::RibRouteActionWeight actionWeight;
    actionWeight.area_to_weight.emplace("test-area", 2);
    thrift::RibPolicyStatement policyStatement;
    policyStatement.matcher.prefixes_ref() = std::vector<thrift::IpPrefix>();
    policyStatement.action.set_weight_ref() = actionWeight;
    thrift::RibPolicy policy;
    policy.statements.emplace_back(policyStatement);
    policy.ttl_secs = 1;

    EXPECT_NO_THROW(openrCtrlThriftClient_->sync_setRibPolicy(policy));
  }

  // Get API
  {
    thrift::RibPolicy policy;
    EXPECT_NO_THROW(openrCtrlThriftClient_->sync_getRibPolicy(policy));
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
