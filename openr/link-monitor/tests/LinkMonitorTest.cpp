/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <thread>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/mocks/NetlinkEventsInjector.h>

using namespace std;
using namespace openr;

using apache::thrift::FRAGILE;
using ::testing::InSequence;

// node-1 connects node-2 via interface iface_2_1 and iface_2_2, node-3 via
// interface iface_3_1
namespace {

re2::RE2::Options regexOpts;

const auto nb2_v4_addr = "192.168.0.2";
const auto nb3_v4_addr = "192.168.0.3";
const auto nb2_v6_addr = "fe80::2";
const auto nb3_v6_addr = "fe80::3";
const auto if_2_1 = "iface_2_1";
const auto if_2_2 = "iface_2_2";
const auto if_3_1 = "iface_3_1";

const auto kvStoreCmdPort = 10002;

const auto peerSpec_2_1 = createPeerSpec(
    folly::sformat("tcp://[{}%{}]:{}", nb2_v6_addr, if_2_1, kvStoreCmdPort),
    folly::sformat("{}%{}", nb2_v6_addr, if_2_1),
    1);

const auto peerSpec_2_2 = createPeerSpec(
    folly::sformat("tcp://[{}%{}]:{}", nb2_v6_addr, if_2_2, kvStoreCmdPort),
    folly::sformat("{}%{}", nb2_v6_addr, if_2_2),
    1);

const auto peerSpec_3_1 = createPeerSpec(
    folly::sformat("tcp://[{}%{}]:{}", nb3_v6_addr, if_3_1, kvStoreCmdPort),
    folly::sformat("{}%{}", nb3_v6_addr, if_3_1),
    2);

const auto nb2 = createSparkNeighbor(
    "node-2",
    toBinaryAddress(folly::IPAddress(nb2_v4_addr)),
    toBinaryAddress(folly::IPAddress(nb2_v6_addr)),
    kvStoreCmdPort,
    1, // openrCtrlThriftPort
    "");

const auto nb3 = createSparkNeighbor(
    "node-3",
    toBinaryAddress(folly::IPAddress(nb3_v4_addr)),
    toBinaryAddress(folly::IPAddress(nb3_v6_addr)),
    kvStoreCmdPort,
    2, // openrCtrlThriftPort
    "");

const uint64_t kTimestamp{1000000};
const uint64_t kNodeLabel{0};
const std::string kConfigKey = "link-monitor-config";

const auto adj_2_1 = createThriftAdjacency(
    "node-2",
    if_2_1,
    nb2_v6_addr,
    nb2_v4_addr,
    1 /* metric */,
    1 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

const auto adj_2_2 = createThriftAdjacency(
    "node-2",
    if_2_2,
    nb2_v6_addr,
    nb2_v4_addr,
    1 /* metric */,
    2 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

const auto adj_3_1 = createThriftAdjacency(
    "node-3",
    if_3_1,
    nb3_v6_addr,
    nb3_v4_addr,
    1 /* metric */,
    1 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

thrift::SparkNeighborEvent
createNeighborEvent(
    thrift::SparkNeighborEventType eventType,
    const std::string& ifName,
    const thrift::SparkNeighbor& neighbor,
    int64_t rttUs,
    int32_t label,
    const std::string& area = std::string{
        openr::thrift::KvStore_constants::kDefaultArea()}) {
  return createSparkNeighborEvent(
      eventType, ifName, neighbor, rttUs, label, area);
}

thrift::AdjacencyDatabase
createAdjDatabase(
    const std::string& thisNodeName,
    const std::vector<thrift::Adjacency>& adjacencies,
    int32_t nodeLabel,
    const std::string& area =
        openr::thrift::KvStore_constants::kDefaultArea()) {
  return createAdjDb(thisNodeName, adjacencies, nodeLabel, false, area);
}

void
printAdjDb(const thrift::AdjacencyDatabase& adjDb) {
  LOG(INFO) << "Node: " << *adjDb.thisNodeName_ref()
            << ", Overloaded: " << *adjDb.isOverloaded_ref()
            << ", Label: " << *adjDb.nodeLabel_ref()
            << ", area: " << *adjDb.area_ref();
  for (auto const& adj : *adjDb.adjacencies_ref()) {
    LOG(INFO) << "  " << *adj.otherNodeName_ref() << "@" << *adj.ifName_ref()
              << ", metric: " << *adj.metric_ref()
              << ", label: " << *adj.adjLabel_ref()
              << ", overloaded: " << *adj.isOverloaded_ref()
              << ", rtt: " << *adj.rtt_ref() << ", ts: " << *adj.timestamp_ref()
              << ", " << toString(*adj.nextHopV4_ref()) << ", "
              << toString(*adj.nextHopV6_ref());
  }
}

const std::string kTestVethNamePrefix = "vethLMTest";
const std::vector<uint64_t> kTestVethIfIndex = {1240, 1241, 1242};
const std::string kConfigStorePath = "/tmp/lm_ut_config_store.bin";
} // namespace

class LinkMonitorTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {}

  virtual void
  SetUp(
      std::unordered_set<std::string> areas,
      std::chrono::milliseconds flapInitalBackoff =
          std::chrono::milliseconds(1),
      std::chrono::milliseconds flapMaxBackoff = std::chrono::milliseconds(8)) {
    // cleanup any existing config file on disk
    system(folly::sformat("rm -rf {}", kConfigStorePath).c_str());

    // create MockNetlinkProtocolSocket
    nlSock = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&nlEvb_);

    // spin up netlinkEventsInjector to inject LINK/ADDR events
    nlEventsInjector = std::make_shared<NetlinkEventsInjector>(nlSock.get());

    // spin up a config store
    configStore =
        std::make_unique<PersistentStore>(kConfigStorePath, true /* dryrun */);

    configStoreThread = std::make_unique<std::thread>([this]() noexcept {
      LOG(INFO) << "ConfigStore thread starting";
      configStore->run();
      LOG(INFO) << "ConfigStore thread finishing";
    });
    configStore->waitUntilRunning();

    // create config
    config = std::make_shared<Config>(
        getTestOpenrConfig(areas, flapInitalBackoff, flapMaxBackoff));

    // spin up a kvstore
    createKvStore(config);

    // create prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        prefixUpdatesQueue.getReader(),
        routeUpdatesQueue.getReader(),
        config,
        configStore.get(),
        kvStoreWrapper->getKvStore(),
        false,
        std::chrono::seconds(0));
    prefixManagerThread = std::make_unique<std::thread>([this] {
      LOG(INFO) << "prefix manager starting";
      prefixManager->run();
      LOG(INFO) << "prefix manager stopped";
    });

    // start a link monitor
    createLinkMonitor(config);
  }

  void
  TearDown() override {
    LOG(INFO) << "LinkMonitor test/basic operations is done";

    interfaceUpdatesQueue.close();
    peerUpdatesQueue.close();
    neighborUpdatesQueue.close();
    prefixUpdatesQueue.close();
    routeUpdatesQueue.close();
    logSampleQueue.close();
    nlSock->closeQueue();
    kvStoreWrapper->closeQueue();

    LOG(INFO) << "Stopping the LinkMonitor thread";
    linkMonitor->stop();
    linkMonitorThread->join();
    linkMonitor.reset();

    LOG(INFO) << "Stopping prefix manager thread";
    prefixManager->stop();
    prefixManagerThread->join();
    prefixManager.reset();

    // Erase data from config store
    LOG(INFO) << "Erasing data from config store";
    configStore->erase("link-monitor-config").get();

    // stop config store
    LOG(INFO) << "Stopping config store";
    configStore->stop();
    configStoreThread->join();
    configStore.reset();

    // stop the kvStore
    LOG(INFO) << "Stopping KvStoreWrapper";
    kvStoreWrapper->stop();
    kvStoreWrapper.reset();
    LOG(INFO) << "KvStoreWrapper got stopped";

    // stop mocked nl platform
    LOG(INFO) << "Stopping mocked thrift handlers";
    nlEventsInjector.reset();
    nlSock.reset();
    LOG(INFO) << "Mocked thrift handlers got stopped";
  }

  thrift::OpenrConfig
  getTestOpenrConfig(
      std::unordered_set<std::string> areas =
          {openr::thrift::KvStore_constants::kDefaultArea()},
      std::chrono::milliseconds flapInitalBackoff =
          std::chrono ::milliseconds(1),
      std::chrono::milliseconds flapMaxBackoff = std::chrono::milliseconds(8)) {
    // create config
    std::vector<openr::thrift::AreaConfig> areaConfig;
    for (auto id : areas) {
      thrift::AreaConfig area;
      *area.area_id_ref() = id;
      area.neighbor_regexes_ref()->emplace_back(".*");
      areaConfig.emplace_back(std::move(area));
    }
    auto tConfig =
        getBasicOpenrConfig("node-1", "domain", areaConfig, true, true);
    // kvstore config
    tConfig.kvstore_config_ref()->sync_interval_s_ref() = 1;
    // link monitor config
    auto& lmConf = *tConfig.link_monitor_config_ref();
    lmConf.linkflap_initial_backoff_ms_ref() = flapInitalBackoff.count();
    lmConf.linkflap_max_backoff_ms_ref() = flapMaxBackoff.count();
    lmConf.use_rtt_metric_ref() = false;
    *lmConf.include_interface_regexes_ref() = {kTestVethNamePrefix + ".*",
                                               "iface.*"};
    *lmConf.redistribute_interface_regexes_ref() = {"loopback"};
    return tConfig;
  }

  void
  createKvStore(std::shared_ptr<Config> config) {
    kvStoreWrapper = std::make_unique<KvStoreWrapper>(
        context, config, peerUpdatesQueue.getReader());
    kvStoreWrapper->run();
  }

  void
  createLinkMonitor(
      std::shared_ptr<Config> config,
      bool assumeDrained = false,
      bool overrideDrainState = false) {
    linkMonitor = std::make_unique<LinkMonitor>(
        config,
        nlSock.get(),
        kvStoreWrapper->getKvStore(),
        configStore.get(),
        false /* enable perf measurement */,
        interfaceUpdatesQueue,
        prefixUpdatesQueue,
        peerUpdatesQueue,
        logSampleQueue,
        neighborUpdatesQueue.getReader(),
        nlSock->getReader(),
        assumeDrained,
        overrideDrainState,
        std::chrono::seconds(1) /* adjHoldTime */
    );

    linkMonitorThread = std::make_unique<std::thread>([this]() {
      folly::setThreadName("LinkMonitor");
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitor->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitor->waitUntilRunning();
  }

  // Receive and process interface updates from the update queue
  void
  recvAndReplyIfUpdate() {
    auto ifDb = interfaceUpdatesReader.get();
    ASSERT_TRUE(ifDb.hasValue());
    sparkIfDb = std::move(*ifDb.value().interfaces_ref());
    LOG(INFO) << "----------- Interface Updates ----------";
    for (const auto& kv : sparkIfDb) {
      LOG(INFO) << "  Name=" << kv.first << ", Status=" << kv.second.isUp
                << ", IfIndex=" << kv.second.ifIndex
                << ", networks=" << kv.second.networks_ref()->size();
    }
  }

  // check the sparkIfDb has expected number of UP interfaces
  bool
  checkExpectedUPCount(
      const std::map<std::string, thrift::InterfaceInfo>& sparkIfDb,
      int expectedUpCount) {
    int receiveUpCount = 0;
    for (const auto& kv : sparkIfDb) {
      if (kv.second.isUp) {
        receiveUpCount++;
      }
    }
    return receiveUpCount == expectedUpCount;
  }

  // collate Interfaces into a map so UT can run some checks
  using CollatedIfData = struct {
    int isUpCount{0};
    int isDownCount{0};
    int v4AddrsMaxCount{0};
    int v4AddrsMinCount{0};
    int v6LinkLocalAddrsMaxCount{0};
    int v6LinkLocalAddrsMinCount{0};
  };
  using CollatedIfUpdates = std::map<string, CollatedIfData>;

  CollatedIfUpdates
  collateIfUpdates(
      const std::map<std::string, thrift::InterfaceInfo>& interfaces) {
    CollatedIfUpdates res;
    for (const auto& kv : interfaces) {
      const auto& ifName = kv.first;
      if (kv.second.isUp) {
        res[ifName].isUpCount++;
      } else {
        res[ifName].isDownCount++;
      }
      int v4AddrsCount = 0;
      int v6LinkLocalAddrsCount = 0;
      for (const auto& network : *kv.second.networks_ref()) {
        const auto& ipNetwork = toIPNetwork(network);
        if (ipNetwork.first.isV4()) {
          v4AddrsCount++;
        } else if (ipNetwork.first.isV6() && ipNetwork.first.isLinkLocal()) {
          v6LinkLocalAddrsCount++;
        }
      }

      res[ifName].v4AddrsMaxCount =
          max(v4AddrsCount, res[ifName].v4AddrsMaxCount);

      res[ifName].v4AddrsMinCount =
          min(v4AddrsCount, res[ifName].v4AddrsMinCount);

      res[ifName].v6LinkLocalAddrsMaxCount =
          max(v6LinkLocalAddrsCount, res[ifName].v6LinkLocalAddrsMaxCount);

      res[ifName].v6LinkLocalAddrsMinCount =
          min(v6LinkLocalAddrsCount, res[ifName].v6LinkLocalAddrsMinCount);
    }
    return res;
  }

  std::optional<thrift::Value>
  getPublicationValueForKey(
      std::string const& key,
      std::string const& area =
          openr::thrift::KvStore_constants::kDefaultArea()) {
    LOG(INFO) << "Waiting to receive publication for key " << key << " area "
              << area;
    auto pub = kvStoreWrapper->recvPublication();
    if (*pub.area_ref() != area) {
      return std::nullopt;
    }

    VLOG(1) << "Received publication with keys in area " << *pub.area_ref();
    for (auto const& kv : *pub.keyVals_ref()) {
      VLOG(1) << "  " << kv.first;
    }

    auto kv = pub.keyVals_ref()->find(key);
    if (kv == pub.keyVals_ref()->end() or !kv->second.value_ref()) {
      return std::nullopt;
    }

    return kv->second;
  }

  // recv publicatons from kv store until we get what we were
  // expecting for a given key
  void
  checkNextAdjPub(
      std::string const& key,
      std::string const& area =
          openr::thrift::KvStore_constants::kDefaultArea()) {
    CHECK(!expectedAdjDbs.empty());

    LOG(INFO) << "[EXPECTED ADJ]";
    printAdjDb(expectedAdjDbs.front());

    while (true) {
      std::optional<thrift::Value> value;
      try {
        value = getPublicationValueForKey(key, area);
        if (not value.has_value()) {
          continue;
        }
      } catch (std::exception const& e) {
        LOG(ERROR) << "Exception: " << folly::exceptionStr(e);
        EXPECT_TRUE(false);
        return;
      }

      auto adjDb = fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
          value->value_ref().value(), serializer);
      // we can not know what the nodeLabel will be
      adjDb.nodeLabel_ref() = kNodeLabel;
      // nor the timestamp, so we override with our predefinded const values
      for (auto& adj : *adjDb.adjacencies_ref()) {
        adj.timestamp_ref() = kTimestamp;
      }

      LOG(INFO) << "[RECEIVED ADJ]";
      printAdjDb(adjDb);
      auto adjDbstr = fbzmq::util::writeThriftObjStr(adjDb, serializer);
      auto expectedAdjDbstr =
          fbzmq::util::writeThriftObjStr(expectedAdjDbs.front(), serializer);
      if (adjDbstr == expectedAdjDbstr) {
        expectedAdjDbs.pop();
        return;
      }
    }
  }

  // kvstore shall reveive cmd to add/del peers
  void
  checkPeerDump(
      std::string const& nodeName,
      thrift::PeerSpec peerSpec,
      std::string const& area =
          openr::thrift::KvStore_constants::kDefaultArea()) {
    auto const peers = kvStoreWrapper->getPeers(area);
    EXPECT_EQ(peers.count(nodeName), 1);
    if (!peers.count(nodeName)) {
      return;
    }
    EXPECT_EQ(*peers.at(nodeName).cmdUrl_ref(), *peerSpec.cmdUrl_ref());
    EXPECT_EQ(*peers.at(nodeName).peerAddr_ref(), *peerSpec.peerAddr_ref());
    EXPECT_EQ(*peers.at(nodeName).ctrlPort_ref(), *peerSpec.ctrlPort_ref());
  }

  std::unordered_map<thrift::IpPrefix, thrift::PrefixEntry>
  getNextPrefixDb(
      std::string const& originatorId,
      std::string const& area =
          openr::thrift::KvStore_constants::kDefaultArea()) {
    std::unordered_map<thrift::IpPrefix, thrift::PrefixEntry> prefixes;

    // Leverage KvStoreFilter to get `prefix:*` change
    std::optional<KvStoreFilters> kvFilters{KvStoreFilters(
        {Constants::kPrefixDbMarker.toString()}, {originatorId})};
    auto kvs = kvStoreWrapper->dumpAll(std::move(kvFilters), area);
    for (const auto& [key, val] : kvs) {
      if (auto value = val.value_ref()) {
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            *value, serializer);
        // skip prefix entries marked as deleted
        if (*prefixDb.deletePrefix_ref()) {
          continue;
        }
        for (auto const& prefixEntry : *prefixDb.prefixEntries_ref()) {
          prefixes.emplace(*prefixEntry.prefix_ref(), prefixEntry);
        }
      }
    }
    return prefixes;
  }

  void
  stopLinkMonitor() {
    // close queue first
    neighborUpdatesQueue.close();
    nlSock->closeQueue();
    kvStoreWrapper->closeQueue();

    linkMonitor->stop();
    linkMonitorThread->join();
    linkMonitor.reset();
  }

  fbzmq::Context context{};
  folly::EventBase nlEvb_;
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock{nullptr};

  messaging::ReplicateQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue;
  messaging::ReplicateQueue<thrift::PeerUpdateRequest> peerUpdatesQueue;
  messaging::ReplicateQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue;
  messaging::ReplicateQueue<thrift::PrefixUpdateRequest> prefixUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::RQueue<thrift::InterfaceDatabase> interfaceUpdatesReader{
      interfaceUpdatesQueue.getReader()};
  messaging::ReplicateQueue<openr::LogSample> logSampleQueue;

  std::unique_ptr<PersistentStore> configStore;
  std::unique_ptr<std::thread> configStoreThread;

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<Config> config;

  std::unique_ptr<LinkMonitor> linkMonitor;
  std::unique_ptr<std::thread> linkMonitorThread;

  std::unique_ptr<PrefixManager> prefixManager;
  std::unique_ptr<std::thread> prefixManagerThread;

  std::unique_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<NetlinkEventsInjector> nlEventsInjector;

  std::queue<thrift::AdjacencyDatabase> expectedAdjDbs;
  std::map<std::string, thrift::InterfaceInfo> sparkIfDb;
};

// Start LinkMonitor and ensure empty adjacency database and prefixes are
// received upon initial hold-timeout expiry
TEST_F(LinkMonitorTestFixture, NoNeighborEvent) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  // Verify that we receive empty adjacency database
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel));
  checkNextAdjPub("adj:node-1");
}

// Start LinkMonitor and ensure drain state are set correctly according to
// parameters
TEST_F(LinkMonitorTestFixture, DrainState) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});

  // 1. default setup:
  // persistent store == null, assume_drain = false, override_drain_state =
  // isOverloaded should be read from assume_drain, = false
  auto res = linkMonitor->getInterfaces().get();
  ASSERT_NE(nullptr, res);
  EXPECT_FALSE(*res->isOverloaded_ref());

  // 2. restart with persistent store info
  // override_drain_state = false, persistent store has overload = true
  // isOverloaded should be read from persistent store, = true
  stopLinkMonitor();

  {
    // Save isOverloaded to be true in config store
    thrift::LinkMonitorState state;
    state.isOverloaded_ref() = true;
    auto resp = configStore->storeThriftObj(kConfigKey, state).get();
    EXPECT_EQ(folly::Unit(), resp);
  }

  // Create new neighbor update queue. Previous one is closed
  neighborUpdatesQueue.open();
  nlSock->openQueue();
  kvStoreWrapper->openQueue();
  createLinkMonitor(
      config, false /*assumeDrained*/, false /*overrideDrainState*/);
  // checkNextAdjPub("adj:node-1");

  res = linkMonitor->getInterfaces().get();
  ASSERT_NE(nullptr, res);
  EXPECT_TRUE(*res->isOverloaded_ref());

  // 3. restart with override_drain_state = true
  // override_drain_state = true, assume_drain = false, persistent store has
  // overload = true isOverloaded should be read from assume_drain store, =
  // false
  stopLinkMonitor();

  // Create new neighbor update queue. Previous one is closed
  neighborUpdatesQueue.open();
  nlSock->openQueue();
  kvStoreWrapper->openQueue();
  createLinkMonitor(
      config, false /*assumeDrained*/, true /*overrideDrainState*/);

  res = linkMonitor->getInterfaces().get();
  ASSERT_NE(nullptr, res);
  EXPECT_FALSE(*res->isOverloaded_ref());
}

// receive neighbor up/down events from "spark"
// form peer connections and inform KvStore of adjacencies
TEST_F(LinkMonitorTestFixture, BasicOperation) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  const int linkMetric = 123;
  const int adjMetric = 100;

  {
    InSequence dummy;

    {
      // create an interface
      nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
      recvAndReplyIfUpdate();
    }

    {
      // expect neighbor up first
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit set
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;
      adj_2_1_modified.isOverloaded_ref() = true;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;
      adj_2_1_modified.isOverloaded_ref() = true;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value unset
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // set adjacency metric, this should override the link metric
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = adjMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // unset adjacency metric, this will remove the override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // link-monitor and config-store is restarted but state will be
      // retained. expect neighbor up with overrides
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1");
  }

  // testing for set/unset overload bit and custom metric values
  // 1. set overload bit
  // 2. set custom metric on link: No adjacency advertisement
  // 3. set link overload
  // 4. unset overload bit
  // 5. unset link overload bit - custom metric value should be in effect
  // 6. unset custom metric on link
  // 7: set overload bit
  // 8: custom metric on link
  // 9. Set overload bit and link metric value
  // 10. set and unset adjacency metric
  {
    const std::string interfaceName = "iface_2_1";
    const std::string nodeName = "node-2";

    LOG(INFO) << "Testing set node overload command!";
    auto ret = linkMonitor->setNodeOverload(true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    auto res = linkMonitor->getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(*res->isOverloaded_ref());
    EXPECT_EQ(1, res->interfaceDetails_ref()->size());
    EXPECT_FALSE(
        *res->interfaceDetails_ref()->at(interfaceName).isOverloaded_ref());
    EXPECT_FALSE(res->interfaceDetails_ref()
                     ->at(interfaceName)
                     .metricOverride_ref()
                     .has_value());

    LOG(INFO) << "Testing set link metric command!";
    ret = linkMonitor->setLinkMetric(interfaceName, linkMetric).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set link overload command!";
    ret = linkMonitor->setInterfaceOverload(interfaceName, true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = linkMonitor->getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(*res->isOverloaded_ref());
    EXPECT_TRUE(
        *res->interfaceDetails_ref()->at(interfaceName).isOverloaded_ref());
    EXPECT_EQ(
        linkMetric,
        res->interfaceDetails_ref()
            ->at(interfaceName)
            .metricOverride_ref()
            .value());

    LOG(INFO) << "Testing unset node overload command!";
    ret = linkMonitor->setNodeOverload(false).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = linkMonitor->getInterfaces().get();
    EXPECT_FALSE(*res->isOverloaded_ref());
    EXPECT_TRUE(
        *res->interfaceDetails_ref()->at(interfaceName).isOverloaded_ref());
    EXPECT_EQ(
        linkMetric,
        res->interfaceDetails_ref()
            ->at(interfaceName)
            .metricOverride_ref()
            .value());

    LOG(INFO) << "Testing unset link overload command!";
    ret = linkMonitor->setInterfaceOverload(interfaceName, false).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing unset link metric command!";
    ret = linkMonitor->setLinkMetric(interfaceName, std::nullopt).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    res = linkMonitor->getInterfaces().get();
    EXPECT_FALSE(*res->isOverloaded_ref());
    EXPECT_FALSE(
        *res->interfaceDetails_ref()->at(interfaceName).isOverloaded_ref());
    EXPECT_FALSE(res->interfaceDetails_ref()
                     ->at(interfaceName)
                     .metricOverride_ref()
                     .has_value());

    LOG(INFO) << "Testing set node overload command( AGAIN )!";
    ret = linkMonitor->setNodeOverload(true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set link metric command( AGAIN )!";
    ret = linkMonitor->setLinkMetric(interfaceName, linkMetric).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set adj metric command!";
    ret = linkMonitor->setAdjacencyMetric(interfaceName, nodeName, adjMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing unset adj metric command!";
    ret = linkMonitor->setAdjacencyMetric(interfaceName, nodeName, std::nullopt)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
  }

  // 11. neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1");
  }

  // stop linkMonitor
  LOG(INFO) << "Mock restarting link monitor!";
  peerUpdatesQueue.close();
  stopLinkMonitor();
  kvStoreWrapper->stop();
  kvStoreWrapper.reset();

  // Create new neighborUpdatesQ/peerUpdatesQ/platformUpdatesQ.
  neighborUpdatesQueue =
      messaging::ReplicateQueue<thrift::SparkNeighborEvent>();
  peerUpdatesQueue = messaging::ReplicateQueue<thrift::PeerUpdateRequest>();
  nlSock->openQueue();

  // Recreate KvStore as previous kvStoreUpdatesQueue is closed
  createKvStore(config);

  // mock "restarting" link monitor with existing config store
  createLinkMonitor(config);

  // 12. neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor up event!";
    checkNextAdjPub("adj:node-1");
  }

  // 13. neighbor down with empty address
  {
    thrift::BinaryAddress empty;
    auto cp = nb2;
    *cp.transportAddressV4_ref() = empty;
    *cp.transportAddressV6_ref() = empty;
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        cp,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor down event witgh empty address!";
    checkNextAdjPub("adj:node-1");
  }
}

// Test linkMonitor restarts to honor `enableSegmentRouting` flag
TEST_F(LinkMonitorTestFixture, NodeLabelRemoval) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  {
    // Intertionally save nodeLabel to be non-zero value
    thrift::LinkMonitorState state;
    state.nodeLabel_ref() = 1 + rand(); // non-zero random number
    auto resp = configStore->storeThriftObj(kConfigKey, state).get();
    EXPECT_EQ(folly::Unit(), resp);
  }

  {
    // stop linkMonitor
    LOG(INFO) << "Mock restarting link monitor!";
    stopLinkMonitor();

    // Create new neighbor update queue. Previous one is closed
    neighborUpdatesQueue.open();
    nlSock->openQueue();
    kvStoreWrapper->openQueue();

    // ATTN: intentionally set `enableSegmentRouting = false` to test the
    //       config_ load scenario.
    auto tConfigCopy = getTestOpenrConfig();
    tConfigCopy.enable_segment_routing_ref() = false;
    auto configSegmentRoutingDisabled = std::make_shared<Config>(tConfigCopy);
    createLinkMonitor(configSegmentRoutingDisabled);

    // nodeLabel is non-zero value read from config_, override to 0 to
    // honor flag.
    auto thriftAdjDb = linkMonitor->getAdjacencies().get();
    EXPECT_TRUE(thriftAdjDb);
    EXPECT_EQ(0, *thriftAdjDb->nodeLabel_ref());
  }
}

// Test throttling
TEST_F(LinkMonitorTestFixture, Throttle) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  {
    InSequence dummy;

    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // before throttled function kicks in

  // another neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // neighbor 3 down immediately
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  checkNextAdjPub("adj:node-1");
}

// parallel adjacencies between two nodes via different interfaces
TEST_F(LinkMonitorTestFixture, ParallelAdj) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  {
    InSequence dummy;

    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor up on another interface
    // still use iface_2_1 because it's the "min" and will not call addPeers

    {
      // note: adj_2_2 is hashed ahead of adj_2_1
      auto adjDb = createAdjDatabase("node-1", {adj_2_2, adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor down
    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_2}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);

  // neighbor up on another interface
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        2 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);

  // neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_2.otherNodeName_ref(), peerSpec_2_2);
}

// Verify neighbor-restarting event (including parallel case)
TEST_F(LinkMonitorTestFixture, NeighborRestart) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});

  /* Single link case */
  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);

  // neighbor restart
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  // peers should be gone
  EXPECT_TRUE(kvStoreWrapper->getPeers().empty());

  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);

  /* Parallel case */
  // neighbor up on another interface
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        2 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);

  // neighbor restarting on iface_2_1
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_2.otherNodeName_ref(), peerSpec_2_2);

  // neighbor restarted
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);

  // neighbor restarting iface_2_2
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);

  // neighbor restarting iface_2_1
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  // peers should be gone
  EXPECT_TRUE(kvStoreWrapper->getPeers().empty());
}

TEST_F(LinkMonitorTestFixture, DampenLinkFlaps) {
  SetUp(
      {openr::thrift::KvStore_constants::kDefaultArea()},
      std::chrono::milliseconds(2000),
      std::chrono::milliseconds(4000));
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Updates will be coalesced by throttling

  {
    // Both interfaces report as down on creation
    // expect sparkIfDb to have two interfaces DOWN
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }

  VLOG(2) << "*** start link flaps ***";

  // Bringing up the interface
  VLOG(2) << "*** bring up 2 interfaces ***";
  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate(); // Updates will be coalesced by throttling

  // at this point, both interface should have no backoff
  auto links = linkMonitor->getInterfaces().get();
  EXPECT_EQ(2, links->interfaceDetails_ref()->size());
  for (const auto& ifName : ifNames) {
    EXPECT_FALSE(links->interfaceDetails_ref()
                     ->at(ifName)
                     .linkFlapBackOffMs_ref()
                     .has_value());
  }

  VLOG(2) << "*** bring down 2 interfaces ***";
  auto linkDownTs = std::chrono::steady_clock::now();
  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Update will be sent immediately within 1ms
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Update will be sent immediately within 1ms

  // at this point, both interface should have backoff=~2s
  {
    // we expect all interfaces are down at this point because backoff hasn't
    // been cleared up yet
    auto res = collateIfUpdates(sparkIfDb);
    auto links1 = linkMonitor->getInterfaces().get();
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links1->interfaceDetails_ref()->size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_GE(
          links1->interfaceDetails_ref()
              ->at(ifName)
              .linkFlapBackOffMs_ref()
              .value(),
          0);
      EXPECT_LE(
          links1->interfaceDetails_ref()
              ->at(ifName)
              .linkFlapBackOffMs_ref()
              .value(),
          2000);
    }
  }

  VLOG(2) << "*** bring up 2 interfaces ***";
  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate(); // No-op throttling update
  // [linkX - UP update] or [both linkX and linkY - UP update]
  recvAndReplyIfUpdate();
  // if only got [linkX - UP update]
  if (!checkExpectedUPCount(sparkIfDb, 2)) {
    recvAndReplyIfUpdate(); // get [linkY - UP update]
  }
  auto linkUpTs = std::chrono::steady_clock::now();

  // Elapsed total time between interface down->up must be greater than
  // backoff time of 2s. Also ensure upper bound
  EXPECT_LE(std::chrono::seconds(2), linkUpTs - linkDownTs);
  EXPECT_GE(std::chrono::seconds(3), linkUpTs - linkDownTs);

  // Expect interface update after backoff time passes with state=UP
  {
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      // Both report UP twice (link up and addr add)
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      // We get the link local notification
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }
  VLOG(2) << "*** end link flaps ***";

  // Bringing down the interfaces
  VLOG(2) << "*** bring down 2 interfaces ***";
  linkDownTs = std::chrono::steady_clock::now();
  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Update will be sent immediately within 1ms
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // Update will be sent immediately within 1ms

  // at this point, both interface should have backoff=~4sec
  {
    // expect sparkIfDb to have two interfaces DOWN
    auto res = collateIfUpdates(sparkIfDb);
    auto links2 = linkMonitor->getInterfaces().get();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links2->interfaceDetails_ref()->size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_GE(
          links2->interfaceDetails_ref()
              ->at(ifName)
              .linkFlapBackOffMs_ref()
              .value(),
          2000);
      EXPECT_LE(
          links2->interfaceDetails_ref()
              ->at(ifName)
              .linkFlapBackOffMs_ref()
              .value(),
          4000);
    }
  }

  // Bringing up the interfaces
  VLOG(2) << "*** bring up 2 interfaces ***";
  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate(); // No-op throttling update
  // [linkX - UP update] or [both linkX and linkY - UP update]
  recvAndReplyIfUpdate();
  // if only got [linkX - UP update]
  if (!checkExpectedUPCount(sparkIfDb, 2)) {
    recvAndReplyIfUpdate(); // get [linkY - UP update]
  }
  linkUpTs = std::chrono::steady_clock::now();

  // Elapsed total time between interface down->up must be greater than
  // backoff time of 4s. Also ensure upper bound
  EXPECT_LE(std::chrono::seconds(4), linkUpTs - linkDownTs);
  EXPECT_GE(std::chrono::seconds(5), linkUpTs - linkDownTs);

  // at this point, both interface should have backoff back to init value
  {
    // expect sparkIfDb to have two interfaces UP
    // Make sure to wait long enough to clear out backoff timers
    auto res = collateIfUpdates(sparkIfDb);
    auto links3 = linkMonitor->getInterfaces().get();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links3->interfaceDetails_ref()->size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_FALSE(links3->interfaceDetails_ref()
                       ->at(ifName)
                       .linkFlapBackOffMs_ref()
                       .has_value());
    }
  }
}

// Test Interface events to Spark
TEST_F(LinkMonitorTestFixture, verifyLinkEventSubscription) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate();
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate();

  // Both interfaces report as down on creation
  // We receive 2 IfUpUpdates in spark for each interface
  // Both with status as false (DOWN)
  // We let spark return success for each
  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate a link up event publication
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate();
  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  recvAndReplyIfUpdate();

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);

      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });
}

TEST_F(LinkMonitorTestFixture, verifyAddrEventSubscription) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  // Both interfaces report as down on creation
  // We receive 2 IfUpUpdates in spark for each interface
  // Both with status as false (DOWN)
  // We let spark return success for each
  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  nlEventsInjector->sendAddrEvent(linkX, "10.0.0.1/31", true /* is valid */);
  nlEventsInjector->sendAddrEvent(linkY, "10.0.0.2/31", true /* is valid */);
  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  // Emulate add address event: v6 while interfaces are in UP state. Both
  // v4 and v6 addresses should be reported.
  nlEventsInjector->sendAddrEvent(linkX, "fe80::1/128", true /* is valid */);
  nlEventsInjector->sendAddrEvent(linkY, "fe80::2/128", true /* is valid */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(1, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(1, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate delete address event: v4
  nlEventsInjector->sendAddrEvent(linkX, "10.0.0.1/31", false /* is valid */);
  nlEventsInjector->sendAddrEvent(linkY, "10.0.0.2/31", false /* is valid */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(1, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate delete address event: v6
  nlEventsInjector->sendAddrEvent(linkX, "fe80::1/128", false /* is valid */);
  nlEventsInjector->sendAddrEvent(linkY, "fe80::2/128", false /* is valid */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);

      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);

      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate address and link events coming in out of order
  // FixMe NOTE: For interface index to name mapping must exists for address
  // event to be advertised with interface name instead of index. Once we index
  // interfaces by `ifIndex` in LinkMonitor we can send events in any order
  const std::string linkZ = kTestVethNamePrefix + "Z";

  // Link event comes in later - FixMe
  nlEventsInjector->sendLinkEvent(
      linkZ /* link name */,
      kTestVethIfIndex.at(2) /* ifIndex */,
      true /* is up */);
  // Addr event comes in first - FixMe
  nlEventsInjector->sendAddrEvent(linkZ, "fe80::3/128", true /* is valid */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  EXPECT_NO_THROW({
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 3 interfaces
    EXPECT_EQ(3, res.size());
    EXPECT_EQ(1, res.at(linkZ).isUpCount);
    EXPECT_EQ(0, res.at(linkZ).isDownCount);
    EXPECT_EQ(0, res.at(linkZ).v4AddrsMaxCount);
    EXPECT_EQ(0, res.at(linkZ).v4AddrsMinCount);
    EXPECT_EQ(1, res.at(linkZ).v6LinkLocalAddrsMaxCount);
    EXPECT_EQ(0, res.at(linkZ).v6LinkLocalAddrsMinCount);
  });

  // Link goes down
  nlEventsInjector->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  nlEventsInjector->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  nlEventsInjector->sendLinkEvent(
      linkZ /* link name */,
      kTestVethIfIndex[2] /* ifIndex */,
      false /* is up */);
  recvAndReplyIfUpdate(); // coalesced updates by throttling
  {
    // Both interfaces report as down on creation
    // expect sparkIfDb to have two interfaces DOWN
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 3 interfaces
    EXPECT_EQ(3, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }
}

// Test getting unique nodeLabels
TEST_F(LinkMonitorTestFixture, NodeLabelAlloc) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});
  size_t kNumNodesToTest = 10;

  // spin up kNumNodesToTest - 1 new link monitors. 1 is spun up in setup()
  std::vector<std::unique_ptr<LinkMonitor>> linkMonitors;
  std::vector<std::unique_ptr<std::thread>> linkMonitorThreads;
  std::vector<std::shared_ptr<Config>> configs;
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    auto tConfigCopy = getTestOpenrConfig();
    *tConfigCopy.node_name_ref() = folly::sformat("lm{}", i + 1);
    auto currConfig = std::make_shared<Config>(tConfigCopy);
    auto lm = std::make_unique<LinkMonitor>(
        currConfig,
        nlSock.get(),
        kvStoreWrapper->getKvStore(),
        configStore.get(),
        false /* enable perf measurement */,
        interfaceUpdatesQueue,
        prefixUpdatesQueue,
        peerUpdatesQueue,
        logSampleQueue,
        neighborUpdatesQueue.getReader(),
        nlSock->getReader(),
        false, /* assumeDrained */
        false, /* overrideDrainState */
        std::chrono::seconds(1));
    linkMonitors.emplace_back(std::move(lm));
    configs.emplace_back(std::move(currConfig));

    auto lmThread = std::make_unique<std::thread>([&linkMonitors]() {
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitors.back()->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitorThreads.emplace_back(std::move(lmThread));
    linkMonitors.back()->waitUntilRunning();
  }

  // map of nodeId to value allocated
  std::unordered_map<std::string, int32_t> nodeLabels;

  // recv kv store publications until we have valid labels from each node
  while (nodeLabels.size() < kNumNodesToTest) {
    auto pub = kvStoreWrapper->recvPublication();
    for (auto const& kv : *pub.keyVals_ref()) {
      if (kv.first.find("adj:") == 0 and kv.second.value_ref()) {
        auto adjDb = fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
            kv.second.value_ref().value(), serializer);
        nodeLabels[*adjDb.thisNodeName_ref()] = *adjDb.nodeLabel_ref();
        if (*adjDb.nodeLabel_ref() == 0) {
          nodeLabels.erase(*adjDb.thisNodeName_ref());
        }
      }
    }
  }

  // ensure that we have unique values
  std::set<int32_t> labelSet;
  for (auto const& kv : nodeLabels) {
    labelSet.insert(kv.second);
  }

  EXPECT_EQ(kNumNodesToTest, labelSet.size());
  // cleanup
  nlSock->closeQueue();
  neighborUpdatesQueue.close();
  kvStoreWrapper->closeQueue();
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    linkMonitors[i]->stop();
    linkMonitorThreads[i]->join();
  }
}

/**
 * Unit-test to test advertisement of loopback prefixes
 * - add addresses via addrEvent and verify from KvStore prefix-db
 * - remove address via addrEvent and verify from KvStore prefix-db
 * - announce network instead of address via addrEvent and verify it doesn't
 *   change anything
 * - set link to down state and verify that it removes all associated addresses
 */
TEST_F(LinkMonitorTestFixture, LoopbackPrefixAdvertisement) {
  SetUp({thrift::KvStore_constants::kDefaultArea()});

  const std::string nodeName = "node-1";
  const std::string linkLocalAddr1 = "fe80::1/128";
  const std::string linkLocalAddr2 = "fe80::2/64";
  const std::string loopbackAddrV4 = "10.127.240.1/32";
  const std::string loopbackAddrV4Subnet = "10.128.241.1/24";
  const std::string loopbackAddrV6_1 = "2803:cafe:babe::1/128";
  const std::string loopbackAddrV6_2 = "2803:6080:4958:b403::1/128";
  const std::string loopbackAddrV6Subnet = "2803:6080:4958:b403::1/64";

  //
  // Verify that initial DB has empty prefix entries
  //

  std::unordered_map<thrift::IpPrefix, thrift::PrefixEntry> prefixes{};
  prefixes =
      getNextPrefixDb(nodeName, thrift::KvStore_constants::kDefaultArea());
  EXPECT_EQ(0, prefixes.size());

  //
  // Send link UP event(i.e. mixed with VALID and INVALID loopback address)
  //

  nlEventsInjector->sendLinkEvent("loopback", 101, true);

  // 1) push INVALID loopback addresses
  nlEventsInjector->sendAddrEvent("loopback", linkLocalAddr1, true);
  nlEventsInjector->sendAddrEvent("loopback", linkLocalAddr2, true);

  // 2) push VALID loopback addresses WITHOUT subnet
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV4, true);
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV6_1, true);
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV6_2, true);

  // 3) push VALID interface addresses WITH subnet
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV4Subnet, true);
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV6Subnet, true);

  // Get interface updates
  recvAndReplyIfUpdate(); // coalesced updates by throttling

  LOG(INFO) << "Testing address advertisements";

  while (prefixes.size() != 5) {
    prefixes =
        getNextPrefixDb(nodeName, thrift::KvStore_constants::kDefaultArea());
  }

  // verify prefixes with VALID prefixes has been advertised
  EXPECT_EQ(1, prefixes.count(toIpPrefix(loopbackAddrV4)));
  EXPECT_EQ(1, prefixes.count(toIpPrefix(loopbackAddrV4Subnet)));
  EXPECT_EQ(1, prefixes.count(toIpPrefix(loopbackAddrV6_1)));
  EXPECT_EQ(1, prefixes.count(toIpPrefix(loopbackAddrV6_2)));
  EXPECT_EQ(1, prefixes.count(toIpPrefix(loopbackAddrV6Subnet)));

  //
  // Withdraw prefix and see it is being withdrawn
  //

  // 1) withdraw addresses WITHOUT subnet
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV4, false);
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV6_1, false);

  // 2) withdraw addresses WITH subnet
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV4Subnet, false);
  nlEventsInjector->sendAddrEvent("loopback", loopbackAddrV6Subnet, false);

  // Get interface updates
  const auto startTs = std::chrono::steady_clock::now();
  recvAndReplyIfUpdate(); // coalesced updates by throttling
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - startTs);
  // Threshold check to ensure that we react to published events instead of sync
  EXPECT_LT(elapsed.count(), 3);

  LOG(INFO) << "Testing address withdraws";

  while (prefixes.size() != 1) {
    prefixes =
        getNextPrefixDb(nodeName, thrift::KvStore_constants::kDefaultArea());
  }

  ASSERT_EQ(1, prefixes.count(toIpPrefix(loopbackAddrV6_2)));
  auto& prefixEntry = prefixes.at(toIpPrefix(loopbackAddrV6_2));
  EXPECT_EQ(
      Constants::kDefaultPathPreference,
      prefixEntry.metrics_ref()->path_preference_ref());
  EXPECT_EQ(
      Constants::kDefaultSourcePreference,
      prefixEntry.metrics_ref()->source_preference_ref());
  EXPECT_EQ(
      std::set<std::string>(
          {"INTERFACE_SUBNET", folly::sformat("{}:loopback", nodeName)}),
      prefixEntry.tags_ref());

  //
  // Send link down event
  //

  nlEventsInjector->sendLinkEvent("loopback", 101, false);
  recvAndReplyIfUpdate();

  // Verify all addresses are withdrawn on link down event
  while (prefixes.size() != 0) {
    prefixes =
        getNextPrefixDb(nodeName, thrift::KvStore_constants::kDefaultArea());
  }

  LOG(INFO) << "All prefixes get withdrawn.";
}

TEST_F(LinkMonitorTestFixture, GetAllLinks) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea()});

  // Empty links
  auto links = linkMonitor->getAllLinks().get();
  EXPECT_EQ(0, links.size());

  // Set addresses and links
  const std::string ifName = "eth0";
  auto v4Addr = "192.168.0.3/31";
  auto v6Addr = "fc00::3/127";

  EXPECT_EQ(0, nlSock->addLink(fbnl::utils::createLink(1, ifName)).get());
  EXPECT_EQ(
      0, nlSock->addIfAddress(fbnl::utils::createIfAddress(1, v4Addr)).get());
  EXPECT_EQ(
      -ENXIO,
      nlSock->addIfAddress(fbnl::utils::createIfAddress(2, v6Addr)).get());

  // Verify link status and addresses shows up
  links = linkMonitor->getAllLinks().get();
  ASSERT_EQ(1, links.size());

  const auto& link = links.at(0);
  EXPECT_EQ(ifName, link.ifName);
  EXPECT_EQ(1, link.ifIndex);
  EXPECT_TRUE(link.isUp);
  ASSERT_EQ(1, link.networks.size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork(v4Addr, -1, false /* apply mask */),
      link.networks.at(0));
}

TEST(LinkMonitor, GetPeersFromAdjacencies) {
  std::unordered_map<AdjacencyKey, AdjacencyValue> adjacencies;
  std::unordered_map<std::string, thrift::PeerSpec> peers;

  const auto peerSpec0 = createPeerSpec("tcp://[fe80::2%iface0]:10002");
  const auto peerSpec1 = createPeerSpec("tcp://[fe80::2%iface1]:10002");
  const auto peerSpec2 = createPeerSpec("tcp://[fe80::2%iface2]:10002");
  const auto peerSpec3 = createPeerSpec("tcp://[fe80::2%iface3]:10002");

  // Get peer spec
  adjacencies[{"node1", "iface1"}] = {peerSpec1, thrift::Adjacency()};
  adjacencies[{"node2", "iface2"}] = {peerSpec2, thrift::Adjacency()};
  peers["node1"] = peerSpec1;
  peers["node2"] = peerSpec2;
  EXPECT_EQ(2, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Add {node2, iface3} to adjacencies and see no changes peers
  adjacencies[{"node2", "iface3"}] = {peerSpec3, thrift::Adjacency()};
  EXPECT_EQ(3, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Add {node1, iface0} to adjacencies and see node1 changes to peerSpec0
  adjacencies[{"node1", "iface0"}] = {peerSpec0, thrift::Adjacency()};
  peers["node1"] = peerSpec0;
  EXPECT_EQ(4, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Remove {node2, iface2} from adjacencies and see node2 changes to peerSpec3
  adjacencies.erase({"node2", "iface2"});
  peers["node2"] = peerSpec3;
  EXPECT_EQ(3, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Remove {node2, iface3} from adjacencies and see node2 no longer exists
  adjacencies.erase({"node2", "iface3"});
  peers.erase("node2");
  EXPECT_EQ(2, adjacencies.size());
  EXPECT_EQ(1, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Test for empty adjacencies
  adjacencies.clear();
  peers.clear();
  EXPECT_EQ(0, adjacencies.size());
  EXPECT_EQ(0, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // with different areas
  adjacencies[{"node1", "iface1"}] = {
      peerSpec1, thrift::Adjacency(), false, "pod"};
  adjacencies[{"node2", "iface2"}] = {
      peerSpec2, thrift::Adjacency(), false, "pod"};
  peers["node1"] = peerSpec1;
  peers["node2"] = peerSpec2;
  EXPECT_EQ(2, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies, "pod"));

  adjacencies[{"node2", "iface3"}] = {
      peerSpec3, thrift::Adjacency(), false, "plane"};
  EXPECT_EQ(3, adjacencies.size());
  EXPECT_EQ(
      1, LinkMonitor::getPeersFromAdjacencies(adjacencies, "plane").size());
}

TEST_F(LinkMonitorTestFixture, AreaTest) {
  SetUp({openr::thrift::KvStore_constants::kDefaultArea(), "pod", "plane"});

  // Verify that we receive empty adjacency database in all 3 areas
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel, "plane"));
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel, "pod"));
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel));
  checkNextAdjPub("adj:node-1", "plane");
  checkNextAdjPub("adj:node-1", "pod");
  checkNextAdjPub(
      "adj:node-1", openr::thrift::KvStore_constants::kDefaultArea());

  // add link up event. AdjDB should get updated with link interface
  // Will be updated in all areas
  // TODO: Change this when interfaced base areas is implemented, in which
  // case only corresponding area kvstore should get the update

  {
    InSequence dummy;

    nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
    recvAndReplyIfUpdate();
    // expect neighbor up first
    // node-2 neighbor up in iface_2_1
    auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    {
      auto neighborEvent = createNeighborEvent(
          thrift::SparkNeighborEventType::NEIGHBOR_UP,
          "iface_2_1",
          nb2,
          100 /* rtt-us */,
          1 /* label */);
      neighborUpdatesQueue.push(std::move(neighborEvent));
      LOG(INFO) << "Testing neighbor UP event in default area!";

      checkNextAdjPub(
          "adj:node-1", openr::thrift::KvStore_constants::kDefaultArea());
    }

    // bring up iface3_1, neighbor up event in plane area. Adj db in "plane"
    // area should contain only 'adj_3_1'
    nlEventsInjector->sendLinkEvent("iface_3_1", 100, true);
    recvAndReplyIfUpdate();
    adjDb = createAdjDatabase("node-1", {adj_3_1}, kNodeLabel, "plane");
    expectedAdjDbs.push(std::move(adjDb));
    {
      auto neighborEvent = createNeighborEvent(
          thrift::SparkNeighborEventType::NEIGHBOR_UP,
          "iface_3_1",
          nb3,
          100 /* rtt-us */,
          1 /* label */,
          "plane");
      neighborUpdatesQueue.push(std::move(neighborEvent));
      LOG(INFO) << "Testing neighbor UP event in plane area!";

      checkNextAdjPub("adj:node-1", "plane");
    }
  }
  // neighbor up on default area
  {
    auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub(
        "adj:node-1", openr::thrift::KvStore_constants::kDefaultArea());
    checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);
  }
  // neighbor up on "plane" area
  {
    auto adjDb = createAdjDatabase("node-1", {adj_3_1}, kNodeLabel, "plane");
    expectedAdjDbs.push(std::move(adjDb));
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */,
        "plane");
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1", "plane");
    checkPeerDump(*adj_3_1.otherNodeName_ref(), peerSpec_3_1, "plane");
  }
  // verify neighbor down in "plane" area
  {
    auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel, "plane");
    expectedAdjDbs.push(std::move(adjDb));
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */,
        "plane");
    neighborUpdatesQueue.push(std::move(neighborEvent));
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1", "plane");
  }
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
