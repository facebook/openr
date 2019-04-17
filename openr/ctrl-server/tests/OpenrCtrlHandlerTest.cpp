/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

#include <fbzmq/service/monitor/ZmqMonitor.h>
#include <fbzmq/zmq/Context.h>
#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/common/Constants.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/health-checker/HealthChecker.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/link-monitor/tests/MockNetlinkSystemHandler.h>
#include <openr/prefix-manager/PrefixManager.h>

using namespace openr;

class OpenrCtrlFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    const std::unordered_set<std::string> acceptablePeerNames;

    // Create zmq-monitor
    zmqMonitor = std::make_unique<fbzmq::ZmqMonitor>(
        monitorSubmitUrl_, "inproc://monitor_pub_url", context_);
    zmqMonitorThread_ = std::thread([&]() { zmqMonitor->run(); });

    // Create modules
    std::unordered_map<thrift::OpenrModuleType, std::shared_ptr<OpenrEventLoop>>
        moduleTypeToEvl;

    // Create KvStore module
    kvStoreWrapper = std::make_unique<KvStoreWrapper>(
        context_,
        nodeName,
        std::chrono::seconds(1),
        std::chrono::seconds(1),
        std::unordered_map<std::string, thrift::PeerSpec>(),
        folly::none,
        folly::none,
        std::chrono::milliseconds(1),
        true /* enableFloodOptimization */,
        true /* isFloodRoot */);
    kvStoreWrapper->run();
    moduleTypeToEvl[thrift::OpenrModuleType::KVSTORE] =
        kvStoreWrapper->getKvStore();

    // Create Decision module
    decision = std::make_shared<Decision>(
        nodeName, /* node name */
        true, /* enable v4 */
        true, /* computeLfaPaths */
        AdjacencyDbMarker{"adj:"},
        PrefixDbMarker{"prefix:"},
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(500),
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::string{"inproc://decision-rep"},
        DecisionPubUrl{decisionPubUrl_},
        monitorSubmitUrl_,
        context_);
    decisionThread_ = std::thread([&]() { decision->run(); });
    moduleTypeToEvl[thrift::OpenrModuleType::DECISION] = decision;

    // Create Fib module
    fib = std::make_shared<Fib>(
        nodeName,
        -1, /* thrift port */
        true, /* dryrun */
        false, /* periodic syncFib */
        true, /* enableSegmentRouting */
        std::chrono::seconds(2),
        DecisionPubUrl{"inproc://decision-pub"},
        std::string{"inproc://fib-cmd"},
        LinkMonitorGlobalPubUrl{"inproc://lm-pub"},
        MonitorSubmitUrl{"inproc://monitor-sub"},
        context_);
    fibThread_ = std::thread([&]() { fib->run(); });
    moduleTypeToEvl[thrift::OpenrModuleType::FIB] = fib;

    // Create HealthChecker module
    healthChecker = std::make_shared<HealthChecker>(
        nodeName,
        thrift::HealthCheckOption::PingNeighborOfNeighbor,
        uint32_t{50}, /* health check pct */
        uint16_t{0}, /* make sure it binds to some open port */
        std::chrono::seconds(2),
        folly::none, /* maybeIpTos */
        AdjacencyDbMarker{Constants::kAdjDbMarker.str()},
        PrefixDbMarker{Constants::kPrefixDbMarker.str()},
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        folly::none, // command-url
        monitorSubmitUrl_,
        context_);
    healthCheckerThread_ = std::thread([&]() { healthChecker->run(); });
    moduleTypeToEvl[thrift::OpenrModuleType::HEALTH_CHECKER] = healthChecker;

    // Create PrefixManager module
    prefixManager = std::make_shared<PrefixManager>(
        nodeName,
        prefixManagerUrl_,
        persistentStoreUrl_,
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        monitorSubmitUrl_,
        PrefixDbMarker{Constants::kPrefixDbMarker.str()},
        false,
        std::chrono::seconds(0),
        Constants::kKvStoreDbTtl,
        context_);
    prefixManagerThread_ = std::thread([&]() { prefixManager->run(); });
    moduleTypeToEvl[thrift::OpenrModuleType::PREFIX_MANAGER] = prefixManager;

    // Create MockNetlinkSystemHandler
    mockNlHandler =
        std::make_shared<MockNetlinkSystemHandler>(context_, platformPubUrl_);
    systemServer = std::make_shared<apache::thrift::ThriftServer>();
    systemServer->setNumIOWorkerThreads(1);
    systemServer->setNumAcceptThreads(1);
    systemServer->setPort(0);
    systemServer->setInterface(mockNlHandler);
    systemThriftThread.start(systemServer);

    // Create LinkMonitor
    re2::RE2::Options regexOpts;
    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add("po.*", &regexErr);
    includeRegexList->Compile();

    linkMonitor = std::make_shared<LinkMonitor>(
        context_,
        nodeName,
        systemThriftThread.getAddress()->getPort(),
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        nullptr,
        nullptr, // redistribute interface name
        std::vector<thrift::IpPrefix>{},
        false /* useRttMetric */,
        false /* enable perf measurement */,
        true /* enable v4 */,
        true /* enable segment routing */,
        false /* prefix type mpls */,
        AdjacencyDbMarker{Constants::kAdjDbMarker.str()},
        sparkCmdUrl_,
        sparkReportUrl_,
        monitorSubmitUrl_,
        persistentStoreUrl_,
        false,
        PrefixManagerLocalCmdUrl{prefixManager->inprocCmdUrl},
        platformPubUrl_,
        lmPubUrl_,
        lmCmdUrl_,
        std::chrono::seconds(1),
        // link flap backoffs, set low to keep UT runtime low
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8),
        Constants::kKvStoreDbTtl);
    linkMonitorThread_ = std::thread([&]() { linkMonitor->run(); });
    moduleTypeToEvl[thrift::OpenrModuleType::LINK_MONITOR] = linkMonitor;

    // Create PersistentStore
    persistentStore = std::make_unique<PersistentStore>(
        nodeName,
        "/tmp/openr-ctrl-handler-test.bin",
        persistentStoreUrl_,
        context_);
    persistentStoreThread_ = std::thread([&]() { persistentStore->run(); });
    moduleTypeToEvl[thrift::OpenrModuleType::PERSISTENT_STORE] =
        persistentStore;

    // Create open/r handler
    handler = std::make_unique<OpenrCtrlHandler>(
        nodeName,
        acceptablePeerNames,
        moduleTypeToEvl,
        monitorSubmitUrl_,
        context_);
  }

  void
  TearDown() override {
    handler.reset();

    linkMonitor->stop();
    linkMonitorThread_.join();

    persistentStore->stop();
    persistentStoreThread_.join();

    prefixManager->stop();
    prefixManagerThread_.join();

    mockNlHandler->stop();
    systemThriftThread.stop();

    healthChecker->stop();
    healthCheckerThread_.join();

    fib->stop();
    fibThread_.join();

    decision->stop();
    decisionThread_.join();

    kvStoreWrapper->stop();

    zmqMonitor->stop();
    zmqMonitorThread_.join();
  }

  thrift::Value
  createThriftValue(
      int64_t version,
      const std::string& originatorId,
      const std::string& value) {
    thrift::Value thriftValue;
    thriftValue.version = version;
    thriftValue.originatorId = originatorId;
    thriftValue.value = value;
    thriftValue.ttl = Constants::kTtlInfinity;
    thriftValue.hash = generateHash(version, originatorId, value);
    return thriftValue;
  }

  thrift::PeerSpec
  createPeerSpec(const std::string& pubUrl, const std::string& cmdUrl) {
    thrift::PeerSpec peerSpec;
    peerSpec.pubUrl = pubUrl;
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
  const DecisionPubUrl decisionPubUrl_{"inproc://decision-pub"};
  const PersistentStoreUrl persistentStoreUrl_{"inproc://persistent-store-url"};
  const SparkCmdUrl sparkCmdUrl_{"inproc://spark-req"};
  const SparkReportUrl sparkReportUrl_{"inproc://spark-report"};
  const PlatformPublisherUrl platformPubUrl_{"inproc://platform-pub-url"};
  const LinkMonitorGlobalPubUrl lmPubUrl_{"inproc://link-monitor-pub-url"};
  const std::string lmCmdUrl_{"inproc://link-monitor-cmd-url"};
  const std::string prefixManagerUrl_{"inproc://prefix-mngr-global-cmd"};
  fbzmq::Context context_;
  std::thread zmqMonitorThread_;
  std::thread decisionThread_;
  std::thread fibThread_;
  std::thread healthCheckerThread_;
  std::thread prefixManagerThread_;
  std::thread persistentStoreThread_;
  std::thread linkMonitorThread_;
  apache::thrift::util::ScopedServerThread systemThriftThread;

 public:
  const std::string nodeName{"thanos@universe"};
  std::unique_ptr<fbzmq::ZmqMonitor> zmqMonitor;
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<Decision> decision;
  std::shared_ptr<Fib> fib;
  std::shared_ptr<HealthChecker> healthChecker;
  std::shared_ptr<MockNetlinkSystemHandler> mockNlHandler;
  std::shared_ptr<apache::thrift::ThriftServer> systemServer;
  std::shared_ptr<PrefixManager> prefixManager;
  std::shared_ptr<PersistentStore> persistentStore;
  std::shared_ptr<LinkMonitor> linkMonitor;
  std::unique_ptr<OpenrCtrlHandler> handler;
};

TEST_F(OpenrCtrlFixture, PrefixManagerApis) {
  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("10.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("11.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("20.0.0.0/8", thrift::PrefixType::BGP),
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto ret = handler
                   ->semifuture_advertisePrefixes(
                       std::make_unique<std::vector<thrift::PrefixEntry>>(
                           std::move(prefixes)))
                   .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto ret = handler
                   ->semifuture_withdrawPrefixes(
                       std::make_unique<std::vector<thrift::PrefixEntry>>(
                           std::move(prefixes)))
                   .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    auto ret =
        handler->semifuture_withdrawPrefixesByType(thrift::PrefixType::LOOPBACK)
            .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto ret = handler
                   ->semifuture_syncPrefixesByType(
                       thrift::PrefixType::BGP,
                       std::make_unique<std::vector<thrift::PrefixEntry>>(
                           std::move(prefixes)))
                   .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    const std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto ret = handler->semifuture_getPrefixes().get();
    EXPECT_NE(nullptr, ret);
    EXPECT_EQ(prefixes, *ret);
  }

  {
    auto ret =
        handler->semifuture_getPrefixesByType(thrift::PrefixType::LOOPBACK)
            .get();
    EXPECT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->size());
  }
}

TEST_F(OpenrCtrlFixture, RouteApis) {
  {
    auto ret = handler->semifuture_getRouteDb().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }

  {
    auto ret = handler
                   ->semifuture_getRouteDbComputed(
                       std::make_unique<std::string>(nodeName))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }

  {
    const std::string testNode("avengers@universe");
    auto ret = handler
                   ->semifuture_getRouteDbComputed(
                       std::make_unique<std::string>(testNode))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(testNode, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }
}

TEST_F(OpenrCtrlFixture, PerfApis) {
  {
    auto ret = handler->semifuture_getPerfDb().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
  }
}

TEST_F(OpenrCtrlFixture, DecisionApis) {
  {
    auto ret = handler->semifuture_getDecisionAdjacencyDbs().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->size());
  }

  {
    auto ret = handler->semifuture_getDecisionPrefixDbs().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->size());
  }
}

TEST_F(OpenrCtrlFixture, HealthCheckerApis) {
  {
    auto ret = handler->semifuture_getHealthCheckerInfo().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->nodeInfo.size());
  }
}

TEST_F(OpenrCtrlFixture, KvStoreApis) {
  thrift::KeyVals keyVals;
  keyVals["key1"] = createThriftValue(1, "node1", "value1");
  keyVals["key11"] = createThriftValue(1, "node1", "value11");
  keyVals["key111"] = createThriftValue(1, "node1", "value111");
  keyVals["key2"] = createThriftValue(1, "node1", "value2");
  keyVals["key22"] = createThriftValue(1, "node1", "value22");
  keyVals["key222"] = createThriftValue(1, "node1", "value222");
  keyVals["key3"] = createThriftValue(1, "node3", "value3");
  keyVals["key33"] = createThriftValue(1, "node33", "value33");
  keyVals["key333"] = createThriftValue(1, "node33", "value333");

  //
  // Key set/get
  //

  {
    thrift::KeySetParams setParams;
    setParams.keyVals = keyVals;

    auto ret = handler
                   ->semifuture_setKvStoreKeyVals(
                       std::make_unique<thrift::KeySetParams>(setParams))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);

    setParams.solicitResponse = false;
    ret = handler
              ->semifuture_setKvStoreKeyVals(
                  std::make_unique<thrift::KeySetParams>(setParams))
              .get();
    ASSERT_TRUE(folly::Unit() == ret);

    ret = handler
              ->semifuture_setKvStoreKeyValsOneWay(
                  std::make_unique<thrift::KeySetParams>(setParams))
              .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    std::vector<std::string> filterKeys{"key11", "key2"};
    auto ret = handler
                   ->semifuture_getKvStoreKeyVals(
                       std::make_unique<std::vector<std::string>>(filterKeys))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(2, ret->keyVals.size());
    EXPECT_EQ(keyVals.at("key2"), ret->keyVals["key2"]);
    EXPECT_EQ(keyVals.at("key11"), ret->keyVals["key11"]);
  }

  {
    thrift::KeyDumpParams params;
    params.prefix = "key3";
    params.originatorIds.insert("node3");

    auto ret = handler
                   ->semifuture_getKvStoreKeyValsFiltered(
                       std::make_unique<thrift::KeyDumpParams>(params))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(3, ret->keyVals.size());
    EXPECT_EQ(keyVals.at("key3"), ret->keyVals["key3"]);
    EXPECT_EQ(keyVals.at("key33"), ret->keyVals["key33"]);
    EXPECT_EQ(keyVals.at("key333"), ret->keyVals["key333"]);
  }

  {
    thrift::KeyDumpParams params;
    params.prefix = "key3";
    params.originatorIds.insert("node3");

    auto ret = handler
                   ->semifuture_getKvStoreHashFiltered(
                       std::make_unique<thrift::KeyDumpParams>(params))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(3, ret->keyVals.size());
    auto value3 = keyVals.at("key3");
    value3.value = folly::none;
    auto value33 = keyVals.at("key33");
    value33.value = folly::none;
    auto value333 = keyVals.at("key333");
    value333.value = folly::none;
    EXPECT_EQ(value3, ret->keyVals["key3"]);
    EXPECT_EQ(value33, ret->keyVals["key33"]);
    EXPECT_EQ(value333, ret->keyVals["key333"]);
  }

  //
  // Dual and Flooding APIs
  //

  {
    thrift::DualMessages messages;
    auto ret = handler
                   ->semifuture_processKvStoreDualMessage(
                       std::make_unique<thrift::DualMessages>(messages))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    thrift::FloodTopoSetParams params;
    auto ret = handler
                   ->semifuture_updateFloodTopologyChild(
                       std::make_unique<thrift::FloodTopoSetParams>(params))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_getSpanningTreeInfo().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_FALSE(ret->passive);
    EXPECT_EQ(0, ret->cost);
    ASSERT_TRUE(ret->parent.hasValue());
    EXPECT_EQ(nodeName, ret->parent.value());
    EXPECT_EQ(0, ret->children.size());
  }

  //
  // Peers APIs
  //

  const thrift::PeersMap peers{
      {"peer1", createPeerSpec("inproc:://peer1-pub", "inproc://peer1-cmd")},
      {"peer2", createPeerSpec("inproc:://peer2-pub", "inproc://peer2-cmd")},
      {"peer3", createPeerSpec("inproc:://peer3-pub", "inproc://peer3-cmd")}};

  {
    auto ret = handler
                   ->semifuture_addUpdateKvStorePeers(
                       std::make_unique<thrift::PeersMap>(peers))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_getKvStorePeers().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(3, ret->size());
    EXPECT_EQ(peers.at("peer1"), (*ret)["peer1"]);
    EXPECT_EQ(peers.at("peer2"), (*ret)["peer2"]);
    EXPECT_EQ(peers.at("peer3"), (*ret)["peer3"]);
  }

  {
    std::vector<std::string> peersToDel{"peer2"};
    auto ret = handler
                   ->semifuture_deleteKvStorePeers(
                       std::make_unique<std::vector<std::string>>(peersToDel))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_getKvStorePeers().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(2, ret->size());
    EXPECT_EQ(peers.at("peer1"), (*ret)["peer1"]);
    EXPECT_EQ(peers.at("peer3"), (*ret)["peer3"]);
  }
}

TEST_F(OpenrCtrlFixture, LinkMonitorApis) {
  // create an interface
  mockNlHandler->sendLinkEvent("po1011", 100, true);

  {
    auto ret = handler->semifuture_setNodeOverload().get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_unsetNodeOverload().get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto ret =
        handler->semifuture_setInterfaceOverload(std::move(ifName)).get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto ret =
        handler->semifuture_unsetInterfaceOverload(std::move(ifName)).get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto ret =
        handler->semifuture_setInterfaceMetric(std::move(ifName), 110).get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto ret =
        handler->semifuture_unsetInterfaceMetric(std::move(ifName)).get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto adjName = std::make_unique<std::string>("night@king");
    auto ret = handler
                   ->semifuture_setAdjacencyMetric(
                       std::move(ifName), std::move(adjName), 110)
                   .get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto adjName = std::make_unique<std::string>("night@king");
    auto ret = handler
                   ->semifuture_unsetAdjacencyMetric(
                       std::move(ifName), std::move(adjName))
                   .get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
    EXPECT_FALSE(ret->isOverloaded);
    EXPECT_EQ(1, ret->interfaceDetails.size());
  }

  {
    auto ret = handler->semifuture_getOpenrVersion().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_LE(ret->lowestSupportedVersion, ret->version);
  }

  {
    auto ret = handler->semifuture_getBuildInfo().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_NE("", ret->buildMode);
  }
}

TEST_F(OpenrCtrlFixture, PersistentStoreApis) {
  {
    auto key = std::make_unique<std::string>("key1");
    auto value = std::make_unique<std::string>("value1");
    auto ret =
        handler->semifuture_setConfigKey(std::move(key), std::move(value))
            .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    auto key = std::make_unique<std::string>("key2");
    auto value = std::make_unique<std::string>("value2");
    auto ret =
        handler->semifuture_setConfigKey(std::move(key), std::move(value))
            .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    auto key = std::make_unique<std::string>("key1");
    auto ret = handler->semifuture_eraseConfigKey(std::move(key)).get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    auto key = std::make_unique<std::string>("key2");
    auto ret = handler->semifuture_getConfigKey(std::move(key)).get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ("value2", *ret);
  }

  {
    auto key = std::make_unique<std::string>("key1");
    auto ret = handler->semifuture_getConfigKey(std::move(key));
    EXPECT_THROW(std::move(ret).get(), thrift::OpenrError);
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
