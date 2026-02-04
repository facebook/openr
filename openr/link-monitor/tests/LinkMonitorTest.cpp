/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Subprocess.h>
#include <folly/container/F14Map.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <folly/system/Shell.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/mocks/NetlinkEventsInjector.h>
#include <openr/tests/utils/Utils.h>
#include <exception>

using namespace openr;
using namespace folly::literals::shell_literals;

using ::testing::InSequence;

// node-1 connects node-2 via interface iface_2_1 and iface_2_2, node-3 via
// interface iface_3_1
namespace {

const auto nb2_v4_addr = "192.168.0.2";
const auto nb3_v4_addr = "192.168.0.3";
const auto nb2_v6_addr = "fe80::2";
const auto nb3_v6_addr = "fe80::3";
const auto if_2_1 = "iface_2_1";
const auto if_2_2 = "iface_2_2";
const auto if_3_1 = "iface_3_1";
const auto if_3_2 = "iface_3_2";

const auto peerSpec_2_1 =
    createPeerSpec(fmt::format("{}%{}", nb2_v6_addr, if_2_1), 1);

const auto peerSpec_2_2 =
    createPeerSpec(fmt::format("{}%{}", nb2_v6_addr, if_2_2), 1);

const auto peerSpec_3_1 =
    createPeerSpec(fmt::format("{}%{}", nb3_v6_addr, if_3_1), 2);

const auto nb2_up_event = NeighborEvent(
    NeighborEventType::NEIGHBOR_UP,
    "node-2",
    toBinaryAddress(folly::IPAddress(nb2_v4_addr)),
    toBinaryAddress(folly::IPAddress(nb2_v6_addr)),
    if_2_1, /* local interface name */
    "", /* remote interface name */
    kTestingAreaName, /* area */
    1, /* openrCtrlThriftPort */
    100 /* rtt */);

const auto nb2_down_event = NeighborEvent(
    NeighborEventType::NEIGHBOR_DOWN,
    "node-2",
    toBinaryAddress(folly::IPAddress(nb2_v4_addr)),
    toBinaryAddress(folly::IPAddress(nb2_v6_addr)),
    if_2_1, /* local interface name */
    "", /* remote interface name */
    kTestingAreaName, /* area */
    1, /* openrCtrlThriftPort */
    100 /* rtt */);

const auto nb3_up_event = NeighborEvent(
    NeighborEventType::NEIGHBOR_UP,
    "node-3",
    toBinaryAddress(folly::IPAddress(nb3_v4_addr)),
    toBinaryAddress(folly::IPAddress(nb3_v6_addr)),
    if_3_1, /* local interface name */
    "", /* remote interface name */
    kTestingAreaName, /* area */
    2, /* openrCtrlThriftPort */
    100 /* rtt */);

const auto nb3_down_event = NeighborEvent(
    NeighborEventType::NEIGHBOR_DOWN,
    "node-3",
    toBinaryAddress(folly::IPAddress(nb3_v4_addr)),
    toBinaryAddress(folly::IPAddress(nb3_v6_addr)),
    if_3_1, /* local interface name */
    "", /* remote interface name */
    kTestingAreaName, /* area */
    2, /* openrCtrlThriftPort */
    100 /* rtt */);

const uint64_t kTimestamp{1000000};
const uint64_t kNodeLabel{0};
const std::string kConfigKey = "link-monitor-config";

const auto adj_2_1 = createThriftAdjacency(
    "node-2",
    if_2_1,
    nb2_v6_addr,
    nb2_v4_addr,
    1 /* metric */,
    0 /* label */,
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
    0 /* label */,
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
    0 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

const auto adj_3_2 = createThriftAdjacency(
    "node-3",
    if_3_2,
    nb3_v6_addr,
    nb3_v4_addr,
    1 /* metric */,
    0 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

void
printAdjDb(const thrift::AdjacencyDatabase& adjDb) {
  std::string linkStatusRecordStr;
  if (adjDb.linkStatusRecords().has_value()) {
    for (auto const& [ifName, linkStatusRecord] :
         *adjDb.linkStatusRecords()->linkStatusMap()) {
      linkStatusRecordStr += fmt::format(
          "{} {} at {},",
          ifName,
          *linkStatusRecord.status() == thrift::LinkStatusEnum::UP ? "UP"
                                                                   : "DOWN",
          *linkStatusRecord.unixTs());
    }
  }
  LOG(INFO) << "Node: " << *adjDb.thisNodeName()
            << ", Overloaded: " << *adjDb.isOverloaded()
            << ", Node Metric Increment: " << *adjDb.nodeMetricIncrementVal()
            << ", Label: " << *adjDb.nodeLabel() << ", area: " << *adjDb.area()
            << ", Link Status Records: " << linkStatusRecordStr;
  for (auto const& adj : *adjDb.adjacencies()) {
    LOG(INFO) << "  " << *adj.otherNodeName() << "@" << *adj.ifName()
              << ", metric: " << *adj.metric() << ", label: " << *adj.adjLabel()
              << ", overloaded: " << *adj.isOverloaded()
              << ", Link Metric Increment: " << *adj.linkMetricIncrementVal()
              << ", rtt: " << *adj.rtt() << ", ts: " << *adj.timestamp() << ", "
              << toString(*adj.nextHopV4()) << ", "
              << toString(*adj.nextHopV6());
  }
}

const std::string kTestVethNamePrefix = "vethLMTest";
const std::vector<uint64_t> kTestVethIfIndex = {1240, 1241, 1242};
const std::string kConfigStorePath = "/tmp/lm_ut_config_store.bin";
} // namespace

class LinkMonitorTestFixture : public testing::Test {
 public:
  void
  SetUp() override {
    // cleanup any fb303 data
    facebook::fb303::fbData->resetAllData();

    // cleanup any existing config file on disk
    auto cmd = "rm -rf {}"_shellify(kConfigStorePath.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();

    // create MockNetlinkProtocolSocket
    nlSock = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&nlEvb_);

    // spin up netlinkEventsInjector to inject LINK/ADDR events
    nlEventsInjector = std::make_shared<NetlinkEventsInjector>(nlSock.get());

    // create config
    config = std::make_shared<Config>(createConfig());

    // spin up a config store
    configStore = std::make_unique<PersistentStore>(config, true /* dryrun */);

    configStoreThread = std::make_unique<std::thread>([this]() noexcept {
      LOG(INFO) << "ConfigStore thread starting";
      configStore->run();
      LOG(INFO) << "ConfigStore thread finishing";
    });
    configStore->waitUntilRunning();

    // spin up a kvstore
    createKvStore();

    // start a prefixManager
    createPrefixManager();

    // start a link monitor
    createLinkMonitor();
  }

  void
  TearDown() override {
    LOG(INFO) << "LinkMonitor test/basic operations is done";

    interfaceUpdatesQueue.close();
    peerUpdatesQueue.close();
    kvRequestQueue.close();
    neighborUpdatesQueue.close();
    initializationEventQueue.close();
    prefixUpdatesQueue.close();
    fibRouteUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
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

  /*
   * [Overridable method for config management]
   *
   * Methods can be inherited and overridden by child test fixtures for
   * customize config setup.
   */
  virtual thrift::OpenrConfig
  createConfig() {
    // create list of thrift::AreaConfig
    auto areaCfgs = createAreaConfigs();

    // create basic openr config
    auto tConfig = getBasicOpenrConfig("node-1", areaCfgs, true, true);
    tConfig.persistent_config_store_path() = kConfigStorePath;

    // override LM config
    tConfig.link_monitor_config()->linkflap_initial_backoff_ms() = 1;
    tConfig.link_monitor_config()->linkflap_max_backoff_ms() = 8;
    tConfig.link_monitor_config()->use_rtt_metric() = false;

    return tConfig;
  }

  virtual std::vector<thrift::AreaConfig>
  createAreaConfigs() {
    return populateAreaConfigs({});
  }

  /*
   * [Util methods]
   *
   * Methods serves for util purpose to do:
   *  1) thread/module creation/destruction;
   *  2) validation method for unit-test usage;
   *  3) etc.
   */
  std::vector<thrift::AreaConfig>
  populateAreaConfigs(folly::F14FastSet<std::string> areas) {
    // Use kTestingAreaName by default
    if (areas.empty()) {
      areas.insert(kTestingAreaName.t);
    }

    std::vector<openr::thrift::AreaConfig> areaConfig;

    // iterate and construct config
    for (auto areaId : areas) {
      thrift::AreaConfig cfg;
      cfg.area_id() = areaId;
      cfg.include_interface_regexes() = {kTestVethNamePrefix + ".*", "iface.*"};
      cfg.redistribute_interface_regexes() = {"loopback"};

      areaConfig.emplace_back(std::move(cfg));
    }
    return areaConfig;
  }

  void
  createKvStore() {
    kvStoreWrapper =
        std::make_unique<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            config->getAreaIds(), /* areaId collection */
            config->toThriftKvStoreConfig(), /* thrift::KvStoreConfig */
            peerUpdatesQueue.getReader(), /* peerUpdatesQueue */
            kvRequestQueue.getReader() /* kvRequestQueue */);
    kvStoreWrapper->run();
  }

  void
  createLinkMonitor() {
    linkMonitor = std::make_unique<LinkMonitor>(
        config,
        nlSock.get(),
        configStore.get(),
        interfaceUpdatesQueue,
        prefixUpdatesQueue,
        peerUpdatesQueue,
        logSampleQueue,
        kvRequestQueue,
        kvStoreWrapper->getReader(),
        neighborUpdatesQueue.getReader(),
        nlSock->getReader());

    linkMonitorThread = std::make_unique<std::thread>([this]() {
      folly::setThreadName("LinkMonitor");
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitor->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitor->waitUntilRunning();
  }

  void
  createPrefixManager() {
    prefixManager = std::make_unique<PrefixManager>(
        staticRouteUpdatesQueue,
        kvRequestQueue,
        initializationEventQueue,
        kvStoreWrapper->getReader(),
        prefixUpdatesQueue.getReader(),
        fibRouteUpdatesQueue.getReader(),
        config);
    prefixManagerThread = std::make_unique<std::thread>([this] {
      LOG(INFO) << "prefix manager starting";
      prefixManager->run();
      LOG(INFO) << "prefix manager stopped";
    });

    // Trigger PrefixManager initialization event
    triggerInitializationEventForPrefixManager(
        fibRouteUpdatesQueue, kvStoreWrapper->getKvStoreUpdatesQueueWriter());
  }

  // Receive and process interface updates from the update queue
  void
  recvAndReplyIfUpdate() {
    auto ifDb = interfaceUpdatesReader.get();
    ASSERT_TRUE(ifDb.hasValue());

    // ATTN: update class variable `sparkIfDb` for later verification
    sparkIfDb = ifDb.value();
    LOG(INFO) << "----------- Interface Updates ----------";
    for (const auto& info : sparkIfDb) {
      LOG(INFO) << "  Name=" << info.ifName << ", Status=" << info.isUp
                << ", IfIndex=" << info.ifIndex
                << ", networks=" << info.networks.size();
    }
  }

  // check the ifDb has expected number of UP interfaces
  bool
  checkExpectedUPCount(const InterfaceDatabase& ifDb, int expectedUpCount) {
    int receiveUpCount = 0;
    for (const auto& info : ifDb) {
      if (info.isUp) {
        receiveUpCount++;
      }
    }
    return receiveUpCount == expectedUpCount;
  }

  // collate Interfaces into a map so UT can run some checks
  struct CollatedIfData {
    int isUpCount{0};
    int isDownCount{0};
    int v4AddrsMaxCount{0};
    int v4AddrsMinCount{0};
    int v6LinkLocalAddrsMaxCount{0};
    int v6LinkLocalAddrsMinCount{0};
  };
  using CollatedIfUpdates = std::map<std::string, CollatedIfData>;

  CollatedIfUpdates
  collateIfUpdates(const InterfaceDatabase& interfaces) {
    CollatedIfUpdates res;
    for (const auto& info : interfaces) {
      const auto& ifName = info.ifName;
      if (info.isUp) {
        res[ifName].isUpCount++;
      } else {
        res[ifName].isDownCount++;
      }
      int v4AddrsCount = 0;
      int v6LinkLocalAddrsCount = 0;
      for (const auto& network : info.networks) {
        if (network.first.isV4()) {
          v4AddrsCount++;
        } else if (network.first.isV6() && network.first.isLinkLocal()) {
          v6LinkLocalAddrsCount++;
        }
      }

      res[ifName].v4AddrsMaxCount =
          std::max(v4AddrsCount, res[ifName].v4AddrsMaxCount);

      res[ifName].v4AddrsMinCount =
          std::min(v4AddrsCount, res[ifName].v4AddrsMinCount);

      res[ifName].v6LinkLocalAddrsMaxCount =
          std::max(v6LinkLocalAddrsCount, res[ifName].v6LinkLocalAddrsMaxCount);

      res[ifName].v6LinkLocalAddrsMinCount =
          std::min(v6LinkLocalAddrsCount, res[ifName].v6LinkLocalAddrsMinCount);
    }
    return res;
  }

  std::optional<thrift::Value>
  getPublicationValueForKey(
      std::string const& key, std::string const& area = kTestingAreaName) {
    LOG(INFO) << "Waiting to receive publication for key " << key << " area "
              << area;
    auto pub = kvStoreWrapper->recvPublication();
    if (*pub.area() != area) {
      return std::nullopt;
    }

    XLOG(DBG1) << "Received publication with keys in area " << *pub.area();
    for (auto const& kv : *pub.keyVals()) {
      XLOG(DBG1) << "  " << kv.first;
    }

    auto kv = pub.keyVals()->find(key);
    if (kv == pub.keyVals()->end() || !kv->second.value()) {
      return std::nullopt;
    }

    return kv->second;
  }

  // recv publicatons from kv store until we get what we were
  // expecting for a given key
  void
  checkNextAdjPub(
      std::string const& key, std::string const& area = kTestingAreaName) {
    CHECK(!expectedAdjDbs.empty());

    LOG(INFO) << "[EXPECTED ADJ]";
    printAdjDb(expectedAdjDbs.front());

    while (true) {
      std::optional<thrift::Value> value;
      try {
        value = getPublicationValueForKey(key, area);
        if (!value.has_value()) {
          continue;
        }
      } catch (std::exception const& e) {
        LOG(ERROR) << "Exception: " << folly::exceptionStr(e);
        EXPECT_TRUE(false);
        return;
      }

      auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
          value->value().value(), serializer);
      // we can not know what the nodeLabel will be
      adjDb.nodeLabel() = kNodeLabel;
      // nor the timestamp, so we override with our predefinded const values
      for (auto& adj : *adjDb.adjacencies()) {
        adj.timestamp() = kTimestamp;
      }

      LOG(INFO) << "[RECEIVED ADJ]";
      printAdjDb(adjDb);
      LOG(INFO) << "[EXPECTED ADJ]";
      printAdjDb(expectedAdjDbs.front());
      sortAdjacencyDbByNodeNameAndInterface(adjDb);
      sortAdjacencyDbByNodeNameAndInterface(expectedAdjDbs.front());
      auto adjDbstr = writeThriftObjStr(adjDb, serializer);
      auto expectedAdjDbstr =
          writeThriftObjStr(expectedAdjDbs.front(), serializer);
      if (adjDbstr == expectedAdjDbstr) {
        expectedAdjDbs.pop();
        return;
      }
    }
  }

  void
  sortAdjacencyDbByNodeNameAndInterface(thrift::AdjacencyDatabase& adjDb) {
    auto adjs = adjDb.adjacencies();
    std::stable_sort(
        adjs->begin(),
        adjs->end(),
        [&](::openr::thrift::Adjacency left, ::openr::thrift::Adjacency right) {
          return (*left.otherNodeName() + *left.ifName()) <
              (*right.otherNodeName() + *right.ifName());
        });
  }

  // kvstore shall reveive cmd to add/del peers
  void
  checkPeerDump(
      std::string const& nodeName,
      thrift::PeerSpec peerSpec,
      AreaId const& area = kTestingAreaName) {
    auto const peers = kvStoreWrapper->getPeers(area);
    EXPECT_EQ(peers.count(nodeName), 1);
    if (!peers.count(nodeName)) {
      return;
    }
    EXPECT_EQ(*peers.at(nodeName).peerAddr(), *peerSpec.peerAddr());
    EXPECT_EQ(*peers.at(nodeName).ctrlPort(), *peerSpec.ctrlPort());
  }

  folly::F14FastMap<folly::CIDRNetwork, thrift::PrefixEntry>
  getNextPrefixDb(
      std::string const& originatorId, AreaId const& area = kTestingAreaName) {
    folly::F14FastMap<folly::CIDRNetwork, thrift::PrefixEntry> prefixes;

    // Leverage KvStoreFilter to get `prefix:*` change
    std::optional<KvStoreFilters> kvFilters{KvStoreFilters(
        {Constants::kPrefixDbMarker.toString()}, {originatorId})};
    auto kvs = kvStoreWrapper->dumpAll(area, std::move(kvFilters));
    for (const auto& [key, val] : kvs) {
      if (auto value = val.value()) {
        auto prefixDb =
            readThriftObjStr<thrift::PrefixDatabase>(*value, serializer);
        // skip prefix entries marked as deleted
        if (*prefixDb.deletePrefix()) {
          continue;
        }
        for (auto const& prefixEntry : *prefixDb.prefixEntries()) {
          prefixes.emplace(toIPNetwork(*prefixEntry.prefix()), prefixEntry);
        }
      }
    }
    return prefixes;
  }

  void
  stopLinkMonitor() {
    // close queue first
    neighborUpdatesQueue.close();
    initializationEventQueue.close();
    nlSock->closeQueue();
    kvStoreWrapper->closeQueue();

    linkMonitor->stop();
    linkMonitorThread->join();
    linkMonitor.reset();
  }

  folly::EventBase nlEvb_;
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock{nullptr};

  messaging::ReplicateQueue<InterfaceDatabase> interfaceUpdatesQueue;
  messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue;
  messaging::ReplicateQueue<NeighborInitEvent> neighborUpdatesQueue;
  messaging::ReplicateQueue<thrift::InitializationEvent>
      initializationEventQueue;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  messaging::RQueue<InterfaceDatabase> interfaceUpdatesReader{
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

  std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>
      kvStoreWrapper;
  std::shared_ptr<NetlinkEventsInjector> nlEventsInjector;

  std::queue<thrift::AdjacencyDatabase> expectedAdjDbs;
  InterfaceDatabase sparkIfDb;

  // vector of thrift::AreaConfig
  std::vector<thrift::AreaConfig> areaConfigs_;
};

// Test communication between LinkMonitor and KvStore via KeyValueRequeust
// queue.
TEST_F(LinkMonitorTestFixture, BasicKeyValueRequestQueue) {
  const auto nodeKey = "adj:myNode";
  const auto adjacencies = "adjacencies-for-myNode";

  // Persist key from LinkMonitor
  auto persistPrefixKeyVal =
      PersistKeyValueRequest(kTestingAreaName, nodeKey, adjacencies);
  kvRequestQueue.push(std::move(persistPrefixKeyVal));

  // Check that key was correctly persisted.
  auto maybeValue = getPublicationValueForKey(nodeKey);
  EXPECT_TRUE(maybeValue.has_value());
  EXPECT_EQ(*maybeValue.value().version(), 1);
  EXPECT_EQ(*maybeValue.value().value(), adjacencies);
}

// Start LinkMonitor and ensure empty adjacency database and prefixes are
// received upon initial hold-timeout expiry
TEST_F(LinkMonitorTestFixture, NoNeighborEvent) {
  // Verify that we receive empty adjacency database
  expectedAdjDbs.push(createAdjDb("node-1", {}, kNodeLabel));
  // Trigger KVSTORE_SYNCED initialization event
  triggerInitializationEventKvStoreSynced(
      kvStoreWrapper->getKvStoreUpdatesQueueWriter());
  checkNextAdjPub("adj:node-1");
}

// Start LinkMonitor and ensure drain state are set correctly according to
// parameters
TEST_F(LinkMonitorTestFixture, DrainState) {
  /*
   * Test 1: start with empty persistent store
   *  - persistent store is empty, assume_drained = false
   * Expect:
   *  - `isOverloaded` should be read from assume_drained = false
   */
  {
    auto res = linkMonitor->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_FALSE(*res->isOverloaded());
  }

  /*
   * Test 2: restart with non-empty persistent store
   *  - manually set persistent store with isOverloaded = true
   * Expect:
   *  - `isOverloaded` should be read from persistent store
   */
  {
    stopLinkMonitor();

    // Save isOverloaded to be true in config store
    thrift::LinkMonitorState state;
    state.isOverloaded() = true;
    auto resp = configStore->storeThriftObj(kConfigKey, state).get();
    EXPECT_EQ(folly::Unit(), resp);

    // Create new neighbor update queue. Previous one is closed
    neighborUpdatesQueue.open();
    initializationEventQueue.open();
    nlSock->openQueue();
    kvStoreWrapper->openQueue();
    createLinkMonitor();

    auto res = linkMonitor->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(*res->isOverloaded());
  }

  /*
   * Test 3: restart with empty persistent store again.
   *  - persistent store is empty, assume_drained = false
   * Expect:
   *  - `isOverloaded` should be read from assume_drained = false
   */
  {
    stopLinkMonitor();

    // Erase data from config store
    auto resp = configStore->erase(kConfigKey).get();
    EXPECT_EQ(true, resp);

    // Create new neighbor update queue. Previous one is closed
    neighborUpdatesQueue.open();
    initializationEventQueue.open();
    nlSock->openQueue();
    kvStoreWrapper->openQueue();
    createLinkMonitor();

    auto res = linkMonitor->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_FALSE(*res->isOverloaded());
  }
}

// receive neighbor up/down events from "spark"
// form peer connections and inform KvStore of adjacencies
TEST_F(LinkMonitorTestFixture, BasicOperation) {
  const int linkMetric = 123;
  const int adjMetric = 100;
  const int nodeMetric = 200;
  const int changeNodeMetric = 300;
  const int linkIncMetric = 1000;
  const int changelinkIncMetric = 1500;

  {
    InSequence dummy;

    {
      // create an interface
      nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
      recvAndReplyIfUpdate();
    }

    // Trigger KVSTORE_SYNCED initialization event
    triggerInitializationEventKvStoreSynced(
        kvStoreWrapper->getKvStoreUpdatesQueueWriter());

    {
      // 0. expect neighbor up first
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 1. expect node overload bit set
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 2. expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 3. expect link overloaded bit set
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric;
      adj_2_1_modified.isOverloaded() = true;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 4. expect node overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric;
      adj_2_1_modified.isOverloaded() = true;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 5. expect link overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 6. expect link metric value unset
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 7. expect node overload bit set
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 8. expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 9. set adjacency metric, this should override the link metric
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = adjMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 10. unset adjacency metric, this will remove the override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 11.1 expect node-level metric increment value set on the link override
      // metric
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = nodeMetric + linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      adjDb.nodeMetricIncrementVal() = nodeMetric;
      expectedAdjDbs.push(std::move(adjDb));

      // 11.2 change node-level metric
      // it will add the new metric to the previous the link override metric
      adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = changeNodeMetric + linkMetric;

      adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      adjDb.nodeMetricIncrementVal() = changeNodeMetric;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 11.3 add interface-level metric
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = changeNodeMetric + linkMetric + linkIncMetric;
      adj_2_1_modified.linkMetricIncrementVal() = linkIncMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      adjDb.nodeMetricIncrementVal() = changeNodeMetric;
      expectedAdjDbs.push(std::move(adjDb));

      // 11.4 change interface-level metric
      adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() =
          changeNodeMetric + linkMetric + changelinkIncMetric;
      adj_2_1_modified.linkMetricIncrementVal() = changelinkIncMetric;

      adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      adjDb.nodeMetricIncrementVal() = changeNodeMetric;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // 12.1 expect node-level metric increment value unset, only keeping
      // metric override + interface-level metric increment
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric + changelinkIncMetric;
      adj_2_1_modified.linkMetricIncrementVal() = changelinkIncMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
      // 12.2 expect interface-level metric increment value unset, only keeping
      // metric override
      adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric;

      adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDb("node-1", {}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // link-monitor and config-store is restarted but state will be
      // retained. expect neighbor up with overrides
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDb("node-1", {}, kNodeLabel);
      adjDb.isOverloaded() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
    neighborUpdatesQueue.push(
        NeighborInitEvent(thrift::InitializationEvent::NEIGHBOR_DISCOVERED));

    LOG(INFO) << "Sent neighbor UP event.";

    // Makes sure peerUpdatesQueue gets PeerEvent.
    auto peerEvent = peerUpdatesQueue.getReader().get();
    ASSERT_TRUE(peerEvent.hasValue());
    EXPECT_EQ(peerEvent.value().size(), 1);

    CHECK_EQ(0, kvStoreWrapper->getReader().size());
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
  // 11. set node-level/interface-level metric increment
  // 12. unset node-level/interface-level metric increment
  // 13. neighbor down
  {
    const std::string interfaceName = "iface_2_1";
    const std::string nodeName = "node-2";

    LOG(INFO) << "1. Testing set node overload command!";
    auto ret = linkMonitor->semifuture_setNodeOverload(true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    auto res = linkMonitor->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(*res->isOverloaded());
    EXPECT_EQ(1, res->interfaceDetails()->size());
    EXPECT_FALSE(*res->interfaceDetails()->at(interfaceName).isOverloaded());
    EXPECT_FALSE(res->interfaceDetails()
                     ->at(interfaceName)
                     .metricOverride()
                     .has_value());

    LOG(INFO) << "2. Testing set link metric command!";
    ret =
        linkMonitor->semifuture_setLinkMetric(interfaceName, linkMetric).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "3. Testing set link overload command!";
    ret =
        linkMonitor->semifuture_setInterfaceOverload(interfaceName, true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = linkMonitor->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(*res->isOverloaded());
    EXPECT_TRUE(*res->interfaceDetails()->at(interfaceName).isOverloaded());
    EXPECT_EQ(
        linkMetric,
        res->interfaceDetails()->at(interfaceName).metricOverride().value());

    LOG(INFO) << "4. Testing unset node overload command!";
    ret = linkMonitor->semifuture_setNodeOverload(false).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = linkMonitor->semifuture_getInterfaces().get();
    EXPECT_FALSE(*res->isOverloaded());
    EXPECT_TRUE(*res->interfaceDetails()->at(interfaceName).isOverloaded());
    EXPECT_EQ(
        linkMetric,
        res->interfaceDetails()->at(interfaceName).metricOverride().value());

    LOG(INFO) << "5. Testing unset link overload command!";
    ret = linkMonitor->semifuture_setInterfaceOverload(interfaceName, false)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "6. Testing unset link metric command!";
    ret = linkMonitor->semifuture_setLinkMetric(interfaceName, std::nullopt)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    res = linkMonitor->semifuture_getInterfaces().get();
    EXPECT_FALSE(*res->isOverloaded());
    EXPECT_FALSE(*res->interfaceDetails()->at(interfaceName).isOverloaded());
    EXPECT_FALSE(res->interfaceDetails()
                     ->at(interfaceName)
                     .metricOverride()
                     .has_value());

    LOG(INFO) << "7. Testing set node overload command( AGAIN )!";
    ret = linkMonitor->semifuture_setNodeOverload(true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "8. Testing set link metric command( AGAIN )!";
    ret =
        linkMonitor->semifuture_setLinkMetric(interfaceName, linkMetric).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "9. Testing set adj metric command!";
    ret =
        linkMonitor
            ->semifuture_setAdjacencyMetric(interfaceName, nodeName, adjMetric)
            .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "10. Testing unset adj metric command!";
    ret = linkMonitor
              ->semifuture_setAdjacencyMetric(
                  interfaceName, nodeName, std::nullopt)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "11.1 Testing node-level metric increment";
    ret = linkMonitor->semifuture_setNodeInterfaceMetricIncrement(nodeMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    // Setting the same metric again will skip advertising adjacencies again
    ret = linkMonitor->semifuture_setNodeInterfaceMetricIncrement(nodeMetric)
              .get();
    // Setting an invalid metric will skip
    ret = linkMonitor->semifuture_setNodeInterfaceMetricIncrement(-1).get();

    LOG(INFO) << "11.2 Change node-level metric increment";
    ret = linkMonitor
              ->semifuture_setNodeInterfaceMetricIncrement(changeNodeMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "11.3 Add interface-level metric increment";
    ret = linkMonitor
              ->semifuture_setInterfaceMetricIncrementMulti(
                  {interfaceName}, linkIncMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "11.4 change interface-level metric increment";
    ret = linkMonitor
              ->semifuture_setInterfaceMetricIncrementMulti(
                  {interfaceName}, changelinkIncMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    // Setting the same metric again will skip advertising adjacencies again
    ret = linkMonitor
              ->semifuture_setInterfaceMetricIncrementMulti(
                  {interfaceName}, changelinkIncMetric)
              .get();
    // Setting an invalid metric will skip
    ret = linkMonitor
              ->semifuture_setInterfaceMetricIncrementMulti({interfaceName}, -1)
              .get();

    // Setting containing an valid interface will skip
    ret = linkMonitor
              ->semifuture_setInterfaceMetricIncrementMulti(
                  {interfaceName, "unrecognized"}, changelinkIncMetric)
              .get();

    LOG(INFO) << "12.1 Testing unset node-level metric increment";
    ret = linkMonitor->semifuture_unsetNodeInterfaceMetricIncrement().get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    // trigger the unset again will just skip
    ret = linkMonitor->semifuture_unsetNodeInterfaceMetricIncrement().get();
    EXPECT_TRUE(folly::Unit() == ret);

    LOG(INFO) << "12.2 Testing unset interface-level metric increment";
    ret = linkMonitor
              ->semifuture_unsetInterfaceMetricIncrementMulti({interfaceName})
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    // trigger the unset again will just skip
    ret = linkMonitor->semifuture_unsetNodeInterfaceMetricIncrement().get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  // 13. neighbor down
  {
    auto neighborEvent = nb2_down_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
    LOG(INFO) << "13. Testing neighbor down event!";
    checkNextAdjPub("adj:node-1");
  }

  // stop linkMonitor
  LOG(INFO) << "Mock restarting link monitor!";
  peerUpdatesQueue.close();
  kvRequestQueue.close();
  stopLinkMonitor();
  kvStoreWrapper->stop();
  kvStoreWrapper.reset();

  // Create new
  // neighborUpdatesQ/initialSyncEventsQ/peerUpdatesQ/platformUpdatesQ.
  neighborUpdatesQueue = messaging::ReplicateQueue<NeighborInitEvent>();
  initializationEventQueue =
      messaging::ReplicateQueue<thrift::InitializationEvent>(),
  peerUpdatesQueue = messaging::ReplicateQueue<PeerEvent>();
  kvRequestQueue = messaging::ReplicateQueue<KeyValueRequest>();
  nlSock->openQueue();

  // Recreate KvStore as previous kvStoreUpdatesQueue is closed
  createKvStore();

  // mock "restarting" link monitor with existing config store
  createLinkMonitor();

  // 14. neighbor up
  {
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    neighborUpdatesQueue.push(
        NeighborInitEvent(thrift::InitializationEvent::NEIGHBOR_DISCOVERED));
    LOG(INFO) << "14. Testing adj up event!";
    checkNextAdjPub("adj:node-1");
  }

  // 15. neighbor down with empty address
  {
    thrift::BinaryAddress empty;
    auto neighborEvent = nb2_down_event;
    neighborEvent.neighborAddrV4 = empty;
    neighborEvent.neighborAddrV6 = empty;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
    LOG(INFO) << "15. Testing neighbor down event with empty address!";
    checkNextAdjPub("adj:node-1");
  }
}

// Test throttling
TEST_F(LinkMonitorTestFixture, Throttle) {
  {
    InSequence dummy;

    {
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up on nb2 and nb3
  {
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  {
    auto neighborEvent = nb3_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }

  // before throttled function kicks in

  // neighbor 3 down immediately
  {
    auto neighborEvent = nb3_down_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }

  checkNextAdjPub("adj:node-1");
}

// parallel adjacencies between two nodes via different interfaces
TEST_F(LinkMonitorTestFixture, ParallelAdj) {
  {
    InSequence dummy;

    // neighbor 2 up on iface_2_2
    {
      auto adjDb = createAdjDb("node-1", {adj_2_2}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor 2 up on iface_2_1
    // still use iface_2_2 because it's the first established adj
    {
      // note: adj_2_1 is hashed ahead of adj_2_2
      auto adjDb = createAdjDb("node-1", {adj_2_1, adj_2_2}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor 2 down on iface_2_2
    {
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor 2 down on iface_2_1
    {
      auto adjDb = createAdjDb("node-1", {}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor 2 up on iface_2_1
    // make sure new kvstore peer is created, peer add request is sent out
    {
      auto adjDb = createAdjDb("node-1", {adj_2_2}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor 3: parallel link up at same time
    {
      // note: adj_3_1 adj_3_2
      auto adjDb =
          createAdjDb("node-1", {adj_3_2, adj_2_2, adj_3_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor 2 up on iface_2_2
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    LOG(INFO) << "Sent neighbor UP event.";

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    neighborUpdatesQueue.push(
        NeighborInitEvent(thrift::InitializationEvent::NEIGHBOR_DISCOVERED));

    // kvstore peer initial sync
    checkNextAdjPub("adj:node-1");

    // check kvstore peer events: peerSpec_2_2
    checkPeerDump(*adj_2_2.otherNodeName(), peerSpec_2_2);
  }

  // neighbor 2 up on iface_2_1
  {
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // KvStore Peer has reached to the initial sync state,
    // publish Adj UP immediately
    checkNextAdjPub("adj:node-1");

    // kvstore still have peerSpec_2_2
    checkPeerDump(*adj_2_2.otherNodeName(), peerSpec_2_2);
  }

  // neighbor 2 down on iface_2_2
  {
    auto neighborEvent = nb2_down_event;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // only iface_2_1 in adj pub
    checkNextAdjPub("adj:node-1");

    // check kvstore peer spec change to peerSpec_2_1
    checkPeerDump(*adj_2_1.otherNodeName(), peerSpec_2_1);
  }

  // neighbor 2 down on iface_2_1
  {
    auto neighborEvent = nb2_down_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // check node-1 adj is empty
    checkNextAdjPub("adj:node-1");

    // check kvstore peer should be gone
    EXPECT_TRUE(kvStoreWrapper->getPeers(kTestingAreaName).empty());
  }

  // neighbor 2 up on iface_2_2
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    LOG(INFO) << "Sent neighbor UP event.";

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    checkNextAdjPub("adj:node-1");

    // check kvstore peer events: peerSpec_2_2
    checkPeerDump(*adj_2_2.otherNodeName(), peerSpec_2_2);
  }

  // neighbor 3: both adj up
  {
    auto neighborEvent = nb3_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  {
    auto neighborEvent = nb3_up_event;
    neighborEvent.localIfName = if_3_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  {
    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    checkNextAdjPub("adj:node-1");
  }
}

// Verify neighbor-restarting event (parallel link)
//
// Event Sequence:
//
// neighbor 2 up on adj_2_1
// neighbor 2 up on adj_2_2
// neighbor 2 kvstore initial sync
// check kvstore publication have both adj_2_1 and adj_2_2
//
// neighbor restarting on iface_2_1 (GR)
// neighbor restarting on iface_2_2 (GR)
// check no new publication
//
// neighbor restarted on iface_2_1 (GR Success)
// neighbor restarted on iface_2_1 (GR Success)
// check no new publication
//
// before neighbor 2 finish initial sync, make sure additional events will
// not accidentally trigger adj withdrawn
// - send neighbor 3 up;
// - adj_2_1 rtt change;
// check kvstore publication still have adj_2_1 and adj_2_2
//
// kvstore initial sync
// check no new publication
//
TEST_F(LinkMonitorTestFixture, NeighborGracefulRestartSuccess) {
  // neighbor 2 up on adj_2_1
  {
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    neighborUpdatesQueue.push(
        NeighborInitEvent(thrift::InitializationEvent::NEIGHBOR_DISCOVERED));

    // check for adj update: initial adjacency db includes adj_2_1
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");

    // check kvstore received peer event
    checkPeerDump(*adj_2_1.otherNodeName(), peerSpec_2_1);
  }

  // neighbor 2 up on adj_2_2
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // check adj_2_2 is published
    auto adjDb = createAdjDb("node-1", {adj_2_2, adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");

    // check kvstore peer spec still have peerSpec_2_1
    checkPeerDump(*adj_2_1.otherNodeName(), peerSpec_2_1);
  }

  // neighbor restarting on iface_2_1 (GR)
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.eventType = NeighborEventType::NEIGHBOR_RESTARTING;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  // neighbor restarting iface_2_2 (GR)
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.eventType = NeighborEventType::NEIGHBOR_RESTARTING;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  {
    // wait for this peer change to propogate
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // neighbor 2 kvstore peer2 should be gone
    EXPECT_TRUE(kvStoreWrapper->getPeers(kTestingAreaName).empty());

    // neighbor started GR, no adj update should happen
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }

  // neighbor restarted on iface_2_1 (GR Success)
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.eventType = NeighborEventType::NEIGHBOR_RESTARTED;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  // neighbor restarted on iface_2_2 (GR Success)
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.eventType = NeighborEventType::NEIGHBOR_RESTARTED;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  {
    // wait for this peer change to propogate
    std::this_thread::sleep_for(std::chrono::seconds(1));
    checkPeerDump(*adj_2_1.otherNodeName(), peerSpec_2_1);
    // neighbor started GR, no adj update should happen
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }

  // before neighbor 2 finish initial sync, make sure additional events will
  // not accidentally trigger adj withdrawn
  // send neighbor 3 up
  {
    auto neighborEvent = nb3_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // adj_2_1 still in adj publication
    auto adjDb = createAdjDb("node-1", {adj_3_1, adj_2_1, adj_2_2}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");
  }

  // send adj_2_1 rtt event
  // make sure adj_2_1 is still getting advertised
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.eventType = NeighborEventType::NEIGHBOR_RTT_CHANGE;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // wait for this rtt change to propogate
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // no adj events
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }

  // neighbor 2 kvstore initial sync adj_2_1, adj_2_2 exit GR mode
  {
    // wait for this peer change to propogate
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // no change in adj, so no new publication
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }
}

// Verify neighbor-restarting failure event (parallel link)
//
// Event Sequence:
//
// neighbor 2 up on adj_2_1
// neighbor 2 up on adj_2_2
// neighbor 2 kvstore initial sync
// check kvstore publication have both adj_2_1 and adj_2_2
//
// neighbor restarting on iface_2_1 (GR)
// neighbor restarting on iface_2_2 (GR)
// check no new publication
//
// make sure additional events will not accidentally trigger adj withdrawn
// send neighbor 3 up
// check kvstore publication still have adj_2_1 and adj_2_2
//
// neighbor down on iface_2_1 (GR Success)
// neighbor down on iface_2_2 (GR Success)
// check kvstore publication withdraw iface_2_1 and iface_2_2
//
TEST_F(LinkMonitorTestFixture, NeighborGracefulRestartFailure) {
  // neighbor 2 up on adj_2_1
  {
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    neighborUpdatesQueue.push(
        NeighborInitEvent(thrift::InitializationEvent::NEIGHBOR_DISCOVERED));

    // check for adj update: initial adjacency db includes adj_2_1
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");

    // check kvstore received peer event
    checkPeerDump(*adj_2_1.otherNodeName(), peerSpec_2_1);
  }

  // neighbor 2 up on adj_2_2
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // check adj_2_2 is published
    auto adjDb = createAdjDb("node-1", {adj_2_2, adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");

    // check kvstore peer spec still have peerSpec_2_1
    checkPeerDump(*adj_2_1.otherNodeName(), peerSpec_2_1);
  }

  // neighbor restarting on iface_2_1 (GR)
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.eventType = NeighborEventType::NEIGHBOR_RESTARTING;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  // neighbor restarting iface_2_2 (GR)
  {
    auto neighborEvent = nb2_up_event;
    neighborEvent.eventType = NeighborEventType::NEIGHBOR_RESTARTING;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
  }
  {
    // wait for this peer change to propogate
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // neighbor 2 kvstore peer2 should be gone
    EXPECT_TRUE(kvStoreWrapper->getPeers(kTestingAreaName).empty());

    // neighbor started GR, no adj update should happen
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }

  // make sure additional events will
  // not accidentally trigger adj withdrawn
  // send neighbor 3 up
  {
    auto neighborEvent = nb3_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // adj_2_1 still in adj publication
    auto adjDb = createAdjDb("node-1", {adj_3_1, adj_2_1, adj_2_2}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");
  }

  // neighbor down on iface_2_1 (GR Failure)
  {
    auto neighborEvent = nb2_down_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // adj_2_1 removed
    auto adjDb = createAdjDb("node-1", {adj_3_1, adj_2_2}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");
  }

  // neighbor down on iface_2_2 (GR Failure)
  {
    auto neighborEvent = nb2_down_event;
    neighborEvent.localIfName = if_2_2;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // adj_2_2 removed
    auto adjDb = createAdjDb("node-1", {adj_3_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");
  }
}

class DampenLinkTestFixture : public LinkMonitorTestFixture {
 public:
  thrift::OpenrConfig
  createConfig() override {
    auto tConfig = LinkMonitorTestFixture::createConfig();

    // override LM config
    tConfig.link_monitor_config()->linkflap_initial_backoff_ms() = 2000;
    tConfig.link_monitor_config()->linkflap_max_backoff_ms() = 4000;

    return tConfig;
  }
};

TEST_F(DampenLinkTestFixture, DampenLinkFlaps) {
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

  XLOG(DBG2) << "*** start link flaps ***";

  // Bringing up the interface
  XLOG(DBG2) << "*** bring up 2 interfaces ***";
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
  auto links = linkMonitor->semifuture_getInterfaces().get();
  EXPECT_EQ(2, links->interfaceDetails()->size());
  for (const auto& ifName : ifNames) {
    EXPECT_FALSE(
        links->interfaceDetails()->at(ifName).linkFlapBackOffMs().has_value());
  }

  XLOG(DBG2) << "*** bring down 2 interfaces ***";
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
    auto links1 = linkMonitor->semifuture_getInterfaces().get();
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links1->interfaceDetails()->size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_GE(
          links1->interfaceDetails()->at(ifName).linkFlapBackOffMs().value(),
          0);
      EXPECT_LE(
          links1->interfaceDetails()->at(ifName).linkFlapBackOffMs().value(),
          2000);
    }
  }

  XLOG(DBG2) << "*** bring up 2 interfaces ***";
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
  XLOG(DBG2) << "*** end link flaps ***";

  // Bringing down the interfaces
  XLOG(DBG2) << "*** bring down 2 interfaces ***";
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
    auto links2 = linkMonitor->semifuture_getInterfaces().get();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links2->interfaceDetails()->size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_GE(
          links2->interfaceDetails()->at(ifName).linkFlapBackOffMs().value(),
          2000);
      EXPECT_LE(
          links2->interfaceDetails()->at(ifName).linkFlapBackOffMs().value(),
          4000);
    }
  }

  // Bringing up the interfaces
  XLOG(DBG2) << "*** bring up 2 interfaces ***";
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
    auto links3 = linkMonitor->semifuture_getInterfaces().get();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links3->interfaceDetails()->size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_FALSE(links3->interfaceDetails()
                       ->at(ifName)
                       .linkFlapBackOffMs()
                       .has_value());
    }
  }
}

// Test Interface events to Spark
TEST_F(LinkMonitorTestFixture, verifyLinkEventSubscription) {
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

class TwoAreaTestFixture : public LinkMonitorTestFixture {
 public:
  std::vector<thrift::AreaConfig>
  createAreaConfigs() override {
    return populateAreaConfigs({area1_.t, area2_.t});
  }

 protected:
  const AreaId area1_{"area1"};
  const AreaId area2_{"area2"};
};

/**
 * Unit-test to test advertisement of loopback prefixes
 * - add addresses via addrEvent and verify from KvStore prefix-db
 * - remove address via addrEvent and verify from KvStore prefix-db
 * - announce network instead of address via addrEvent and verify it doesn't
 *   change anything
 * - set link to down state and verify that it removes all associated addresses
 */
TEST_F(TwoAreaTestFixture, LoopbackPrefixAdvertisement) {
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
  EXPECT_EQ(0, getNextPrefixDb(nodeName, area1_).size());
  EXPECT_EQ(0, getNextPrefixDb(nodeName, area2_).size());

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

  {
    folly::F14FastMap<folly::CIDRNetwork, thrift::PrefixEntry> prefixesArea1,
        prefixesArea2;
    do {
      prefixesArea1 = getNextPrefixDb(nodeName, area1_);
      prefixesArea2 = getNextPrefixDb(nodeName, area2_);
    } while (prefixesArea1.size() != 5 || prefixesArea2.size() != 5);

    // verify prefixes with VALID prefixes has been advertised
    EXPECT_EQ(
        1,
        prefixesArea1.count(folly::IPAddress::createNetwork(loopbackAddrV4)));
    EXPECT_EQ(
        1,
        prefixesArea1.count(
            folly::IPAddress::createNetwork(loopbackAddrV4Subnet)));
    EXPECT_EQ(
        1,
        prefixesArea1.count(folly::IPAddress::createNetwork(loopbackAddrV6_1)));
    EXPECT_EQ(
        1,
        prefixesArea1.count(folly::IPAddress::createNetwork(loopbackAddrV6_2)));
    EXPECT_EQ(
        1,
        prefixesArea1.count(
            folly::IPAddress::createNetwork(loopbackAddrV6Subnet)));

    // Both area should report the same set of prefixes
    EXPECT_EQ(prefixesArea1, prefixesArea2);
  }

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
  {
    folly::F14FastMap<folly::CIDRNetwork, thrift::PrefixEntry> prefixesArea1,
        prefixesArea2;
    do {
      prefixesArea1 = getNextPrefixDb(nodeName, area1_);
      prefixesArea2 = getNextPrefixDb(nodeName, area2_);
    } while (prefixesArea1.size() != 1 || prefixesArea2.size() != 1);

    ASSERT_EQ(
        1,
        prefixesArea1.count(folly::IPAddress::createNetwork(loopbackAddrV6_2)));
    auto& prefixEntry =
        prefixesArea1.at(folly::IPAddress::createNetwork(loopbackAddrV6_2));
    EXPECT_EQ(
        Constants::kDefaultPathPreference,
        prefixEntry.metrics()->path_preference());
    EXPECT_EQ(
        Constants::kDefaultSourcePreference,
        prefixEntry.metrics()->source_preference());
    EXPECT_EQ(
        std::set<std::string>(
            {"INTERFACE_SUBNET", fmt::format("{}:loopback", nodeName)}),
        prefixEntry.tags());

    // Both area should report the same set of prefixes
    EXPECT_EQ(prefixesArea1, prefixesArea2);
  }

  //
  // Send link down event
  //

  nlEventsInjector->sendLinkEvent("loopback", 101, false);
  recvAndReplyIfUpdate();

  // Verify all addresses are withdrawn on link down event
  {
    folly::F14FastMap<folly::CIDRNetwork, thrift::PrefixEntry> prefixesArea1,
        prefixesArea2;
    do {
      prefixesArea1 = getNextPrefixDb(nodeName, area1_);
      prefixesArea2 = getNextPrefixDb(nodeName, area2_);
    } while (prefixesArea1.size() != 0 || prefixesArea2.size() != 0);
  }

  LOG(INFO) << "All prefixes get withdrawn.";
}

TEST_F(LinkMonitorTestFixture, GetAllLinks) {
  // Empty links
  auto links = linkMonitor->semifuture_getAllLinks().get();
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
  links = linkMonitor->semifuture_getAllLinks().get();
  ASSERT_EQ(1, links.size());

  const auto& link = links.at(0);
  EXPECT_EQ(ifName, link.ifName);
  EXPECT_EQ(1, link.ifIndex);
  EXPECT_TRUE(link.isUp);
  ASSERT_EQ(1, link.networks.size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork(v4Addr, -1, false /* apply mask */),
      *link.networks.begin());
}

TEST_F(LinkMonitorTestFixture, InitialLinkDiscoveredTest) {
  OpenrEventBase evb;
  int64_t scheduleAt = 0;

  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // create an interface
        nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
        recvAndReplyIfUpdate();
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * openr::Constants::kLinkThrottleTimeout.count()),
      [&]() noexcept {
        auto linkDiscoveredKey = fmt::format(
            Constants::kInitEventCounterFormat,
            apache::thrift::util::enumNameSafe(
                thrift::InitializationEvent::LINK_DISCOVERED));
        EXPECT_TRUE(facebook::fb303::fbData->hasCounter(linkDiscoveredKey));
        EXPECT_GE(facebook::fb303::fbData->getCounter(linkDiscoveredKey), 0);

        evb.stop();
      });

  // Start the eventbase and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

TEST_F(LinkMonitorTestFixture, InitialLinkDiscoveredNegativeTest) {
  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::seconds(Constants::kMaxDurationLinkDiscovery),
      [&]() noexcept {
        auto linkDiscoveredKey = fmt::format(
            Constants::kInitEventCounterFormat,
            apache::thrift::util::enumNameSafe(
                thrift::InitializationEvent::LINK_DISCOVERED));
        EXPECT_TRUE(facebook::fb303::fbData->hasCounter(linkDiscoveredKey));
        EXPECT_GE(facebook::fb303::fbData->getCounter(linkDiscoveredKey), 0);

        evb.stop();
      });

  // Start the eventbase and wait until it is finished execution.
  evb.run();
  evb.waitUntilStopped();
}

TEST_F(LinkMonitorTestFixture, AdjacencyUpWithGracefulRestartTest) {
  // NOTE: explicitly override thrift::SparkNeighbor to mimick Spark => LM
  // with `adjOnlyUsedByOtherNode` set to true.
  auto neighborEvent = nb2_up_event;
  neighborEvent.eventType = NeighborEventType::NEIGHBOR_RESTARTED;
  neighborUpdatesQueue.push(
      NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

  // NOTE: adjacency db will contain adj_2_1 with
  // `adjOnlyUsedByOtherNode=false` as special flag is not set when neighboring
  // node does WARM_BOOT(GR).
  auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
  expectedAdjDbs.push(std::move(adjDb));
  checkNextAdjPub("adj:node-1");
}

TEST_F(LinkMonitorTestFixture, AdjacencyUpTest) {
  {
    // NOTE: explicitly override thrift::SparkNeighbor to mimick Spark => LM
    // with `adjOnlyUsedByOtherNode` set to true.
    auto neighborEvent = nb2_up_event;
    neighborEvent.adjOnlyUsedByOtherNode = true;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    // NOTE: adjacency db will contain adj_2_1 with
    // `adjOnlyUsedByOtherNode=true` as the neighbor is coming up for the first
    // time i.e cold booting.
    auto adj_2_1Copy = folly::copy(adj_2_1);
    adj_2_1Copy.adjOnlyUsedByOtherNode() = true;
    auto adjDb = createAdjDb("node-1", {adj_2_1Copy}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    checkNextAdjPub("adj:node-1");
  }

  {
    // push NB_UP_ADJ_SYNCED event to indicate adj is unblcoked
    auto neighborEvent = nb2_up_event;
    neighborEvent.eventType = NeighborEventType::NEIGHBOR_ADJ_SYNCED;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    neighborUpdatesQueue.push(
        NeighborInitEvent(thrift::InitializationEvent::NEIGHBOR_DISCOVERED));

    // check adjacency will be published as `adjOnlyUsedByOtherNode` is reset
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    checkNextAdjPub("adj:node-1");

    // check kvstore received peer event
    checkPeerDump(*adj_2_1.otherNodeName(), peerSpec_2_1);
  }
}

class DrainStatusTestFixture : public LinkMonitorTestFixture {
 public:
  thrift::OpenrConfig
  createConfig() override {
    // enable softdrain
    auto tConfig = LinkMonitorTestFixture::createConfig();
    tConfig.enable_soft_drain() = true;
    tConfig.assume_drained() = true;

    return tConfig;
  }
};

TEST_F(DrainStatusTestFixture, SoftDrainStatusUponStart) {
  {
    // Create an interface.
    nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
    recvAndReplyIfUpdate();
  }

  {
    // Send neighbor up.
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    // Create expected adjacency.
    // ATTN: expect node metric increment to be applied.
    auto adj_2_1_modified = adj_2_1;
    adj_2_1_modified.metric() =
        *adj_2_1.metric() + config->getNodeMetricIncrement();

    auto adjDb = createAdjDb(
        "node-1",
        {adj_2_1_modified},
        kNodeLabel,
        false /* nodeOverloaded */,
        kTestingAreaName,
        config->getNodeMetricIncrement() /* nodeMetricIncrement */);
    expectedAdjDbs.push(std::move(adjDb));
  }

  // verify adjDb
  checkNextAdjPub("adj:node-1");
}

class UndrainStatusTestFixture : public LinkMonitorTestFixture {
 public:
  thrift::OpenrConfig
  createConfig() override {
    // enable softdrain
    auto tConfig = LinkMonitorTestFixture::createConfig();
    tConfig.enable_soft_drain() = true;
    tConfig.assume_drained() = false;

    return tConfig;
  }
};

TEST_F(UndrainStatusTestFixture, SoftDrainStatusUponStartWithUndrain) {
  {
    // Create an interface.
    nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
    recvAndReplyIfUpdate();
  }

  {
    // Send neighbor up.
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    auto adjDb = createAdjDb(
        "node-1",
        {adj_2_1},
        kNodeLabel,
        false /* nodeOverloaded */,
        kTestingAreaName);
    expectedAdjDbs.push(std::move(adjDb));
  }

  // verify adjDb
  checkNextAdjPub("adj:node-1");
}
class MultiAreaTestFixture : public LinkMonitorTestFixture {
 public:
  std::vector<thrift::AreaConfig>
  createAreaConfigs() override {
    return populateAreaConfigs({kTestingAreaName.t, podArea_, planeArea_});
  }

 protected:
  const std::string podArea_{"pod"};
  const std::string planeArea_{"plane"};
};

/*
 * TODO: T101565435 to track this flaky test under OSS env and re-enable it
 */
TEST_F(MultiAreaTestFixture, DISABLED_AreaTest) {
  // Verify that we receive empty adjacency database in all 3 areas
  expectedAdjDbs.push(createAdjDb("node-1", {}, kNodeLabel));
  expectedAdjDbs.push(createAdjDb("node-1", {}, kNodeLabel, false, planeArea_));
  expectedAdjDbs.push(createAdjDb("node-1", {}, kNodeLabel, false, podArea_));

  checkNextAdjPub("adj:node-1", kTestingAreaName);
  checkNextAdjPub("adj:node-1", planeArea_);
  checkNextAdjPub("adj:node-1", podArea_);

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
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    {
      auto neighborEvent = nb2_up_event;
      neighborUpdatesQueue.push(
          NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
      LOG(INFO) << "Testing neighbor UP event in default area!";

      checkNextAdjPub("adj:node-1", kTestingAreaName);
    }

    // bring up iface3_1, neighbor up event in plane area. Adj db in "plane"
    // area should contain only 'adj_3_1'
    nlEventsInjector->sendLinkEvent("iface_3_1", 100, true);
    recvAndReplyIfUpdate();
    adjDb = createAdjDb("node-1", {adj_3_1}, kNodeLabel, false, planeArea_);
    expectedAdjDbs.push(std::move(adjDb));
    {
      auto neighborEvent = nb3_up_event;
      // cp.area() = planeArea_;
      neighborUpdatesQueue.push(
          NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
      LOG(INFO) << "Testing neighbor UP event in plane area!";

      checkNextAdjPub("adj:node-1", planeArea_);
    }
  }
  // neighbor up on default area
  {
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1", kTestingAreaName);
    checkPeerDump(*adj_2_1.otherNodeName(), peerSpec_2_1);
  }
  // neighbor up on "plane" area
  {
    auto adjDb =
        createAdjDb("node-1", {adj_3_1}, kNodeLabel, false, AreaId{planeArea_});
    expectedAdjDbs.push(std::move(adjDb));

    auto neighborEvent = nb3_up_event;
    // cp.area() = planeArea_;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1", planeArea_);
    checkPeerDump(*adj_3_1.otherNodeName(), peerSpec_3_1, AreaId{planeArea_});
  }
  // verify neighbor down in "plane" area
  {
    auto adjDb = createAdjDb("node-1", {}, kNodeLabel, false, planeArea_);
    expectedAdjDbs.push(std::move(adjDb));

    auto neighborEvent = nb3_down_event;
    // cp.area() = planeArea_;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1", planeArea_);
  }
}

/**
 * Verify that LinkMonitor advertises adjacencies to KvStore after
 *  expiry of adj_hold_time.
 *  This is when KVSTORE_SYNCED signal is not received and thus
 *  hitting hold timer expiry
 */
TEST_F(LinkMonitorTestFixture, AdjHoldTimerExpireTestWithoutFlag) {
  // Note start time for the test.
  std::chrono::steady_clock::time_point testStartTime =
      std::chrono::steady_clock::now();
  // Create an interface.
  nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
  recvAndReplyIfUpdate();

  {
    // Send neighbor up.
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    // Create expected adjacency.
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
  }
  // Find the configured time after which adjacency will be advertised
  // to KvStore. This will be default value of 4 sec.
  const std::chrono::seconds initialAdjHoldTime{
      *config->getConfig().adj_hold_time_s()};
  {
    // Wait for expected adjacency to advertise.
    checkNextAdjPub("adj:node-1");
    // Note the time we successfully received the adj publication.
    std::chrono::steady_clock::time_point kvStorePubTime =
        std::chrono::steady_clock::now();
    // Find the time elapsed since we started.
    auto elapsedTime = kvStorePubTime - testStartTime;
    LOG(INFO)
        << "Elapsed time in seconds: "
        << std::chrono::duration_cast<std::chrono::seconds>(elapsedTime).count()
        << " sec";
    // Publication should have been received after adj_hold_time expiry.
    ASSERT_TRUE(elapsedTime >= std::chrono::seconds(initialAdjHoldTime));
  }
}

class LinkMonitorTestFlagFixture : public LinkMonitorTestFixture {
 public:
  thrift::OpenrConfig
  createConfig() override {
    auto tConfig = LinkMonitorTestFixture::createConfig();

    // override LM config
    tConfig.enable_init_optimization() = false;

    return tConfig;
  }
};

/**
 * Verify that LinkMonitor advertises adjacencies to KvStore after
 *  expiry of adj_hold_time.
 *  This is when openr is configured to ignore KVSTORE_SYNCED flag
 */
TEST_F(LinkMonitorTestFlagFixture, AdjHoldTimerExpireTestWithFlag) {
  // Note start time for the test.
  std::chrono::steady_clock::time_point testStartTime =
      std::chrono::steady_clock::now();
  // Create an interface.
  nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
  recvAndReplyIfUpdate();

  // Trigger KVSTORE_SYNCED initialization event
  // but it should be ignored because of flag setting
  triggerInitializationEventKvStoreSynced(
      kvStoreWrapper->getKvStoreUpdatesQueueWriter());

  {
    // Send neighbor up.
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    // Create expected adjacency.
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
  }
  // Find the configured time after which adjacency will be advertised
  // to KvStore. This will be default value of 4 sec.
  const std::chrono::seconds initialAdjHoldTime{
      *config->getConfig().adj_hold_time_s()};
  {
    // Wait for expected adjacency to advertise.
    checkNextAdjPub("adj:node-1");
    // Note the time we successfully received the adj publication.
    std::chrono::steady_clock::time_point kvStorePubTime =
        std::chrono::steady_clock::now();
    // Find the time elapsed since we started.
    auto elapsedTime = kvStorePubTime - testStartTime;
    LOG(INFO)
        << "Elapsed time in seconds: "
        << std::chrono::duration_cast<std::chrono::seconds>(elapsedTime).count()
        << " sec";
    // Publication should have been received after adj_hold_time expiry.
    ASSERT_TRUE(elapsedTime >= std::chrono::seconds(initialAdjHoldTime));
  }
}

/**
 * Verify that LinkMonitor immediately advertises adjacencies to KvStore
 * after receiving Initialization event -  NEIGHBOR_DISCOVERED.
 */
TEST_F(LinkMonitorTestFixture, EventBasedInitializationTest) {
  // Note start time for the test.
  std::chrono::steady_clock::time_point testStartTime =
      std::chrono::steady_clock::now();
  // Create an interface.
  nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
  recvAndReplyIfUpdate();

  {
    // Send neighbor up.
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    // Signal Intialization event.
    neighborUpdatesQueue.push(thrift::InitializationEvent::NEIGHBOR_DISCOVERED);
  }
  {
    // Create expected adjacency.
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
  }
  // Find the configured time after which adjacency will be advertised
  // to KvStore. This will be default value of 4 sec.
  const std::chrono::seconds initialAdjHoldTime{
      *config->getConfig().adj_hold_time_s()};

  {
    // Wait for expected adjacency to advertise.
    checkNextAdjPub("adj:node-1");
    // Note the time we successfully received the adj publication.
    std::chrono::steady_clock::time_point kvStorePubTime =
        std::chrono::steady_clock::now();
    // Find the time elapsed since we started.
    auto elapsedTime = kvStorePubTime - testStartTime;
    LOG(INFO)
        << "Elapsed time in seconds: "
        << std::chrono::duration_cast<std::chrono::seconds>(elapsedTime).count()
        << " sec";

    // TODO(agrewal): Enable the below assertion after turning on event
    // based initialization.
    // Publication should have been received prior to adj_hold_time expiry.
    // ASSERT_TRUE(elapsedTime < std::chrono::seconds(initialAdjHoldTime));
  }
}

/**
 * Verify that LinkMonitor immediately advertises adjacencies to KvStore
 * after receiving Initialization event -  NEIGHBOR_DISCOVERED.
 */
TEST_F(LinkMonitorTestFixture, NotAllNeighborsUpInitializationTest) {
  // Note start time for the test.
  std::chrono::steady_clock::time_point testStartTime =
      std::chrono::steady_clock::now();
  // Create an interface.
  nlEventsInjector->sendLinkEvent("iface_2_1", 100, true);
  nlEventsInjector->sendLinkEvent("iface_3_1", 100, true);
  recvAndReplyIfUpdate();

  {
    // Send neighbor up for 1 and not the other to simulate the 2nd neighbor
    // not coming up before Spark dumps initial set of discovered neighbors.
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    // Signal Intialization event.
    neighborUpdatesQueue.push(thrift::InitializationEvent::NEIGHBOR_DISCOVERED);
  }
  {
    // Create expected adjacency.
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
  }
  // Find the configured time after which adjacency will be advertised
  // to KvStore. This will be default value of 4 sec.
  const std::chrono::seconds initialAdjHoldTime{
      *config->getConfig().adj_hold_time_s()};

  {
    // Wait for expected adjacency to advertise.
    checkNextAdjPub("adj:node-1");
    // Note the time we successfully received the adj publication.
    std::chrono::steady_clock::time_point kvStorePubTime =
        std::chrono::steady_clock::now();
    // Find the time elapsed since we started.
    auto elapsedTime = kvStorePubTime - testStartTime;
    LOG(INFO)
        << "Elapsed time in seconds: "
        << std::chrono::duration_cast<std::chrono::seconds>(elapsedTime).count()
        << " sec";

    // TODO(agrewal): Enable the below assertion after turning on event
    // based initialization.
    // Publication should have been received prior to adj_hold_time expiry.
    // ASSERT_TRUE(elapsedTime < std::chrono::seconds(initialAdjHoldTime));
  }
}

class LinkStatusTestFixture : public LinkMonitorTestFixture {
 public:
  thrift::OpenrConfig
  createConfig() override {
    auto tConfig = LinkMonitorTestFixture::createConfig();

    // enable feature that keeps track of link status changes
    tConfig.link_monitor_config()->enable_link_status_measurement() = true;

    return tConfig;
  }
};

// Test that link status record is updated with link status change.
TEST_F(LinkStatusTestFixture, LinkStatusRecords) {
  const std::string ifName = "iface_2_1";
  std::string key = "adj:node-1";
  std::string area = kTestingAreaName;
  int64_t ifTs = INT64_MAX;

  // Create an UP interface with UP neighbor.
  // KvStore should have an entry in LinkStatusRecords with status UP.
  {
    nlEventsInjector->sendLinkEvent(ifName, 100, true /* up */);
    recvAndReplyIfUpdate();
    auto neighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    std::optional<thrift::Value> value = getPublicationValueForKey(key);
    auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
        value->value().value(), serializer);
    printAdjDb(adjDb);
    ASSERT_TRUE(adjDb.linkStatusRecords().has_value());
    auto linkStatusMap = adjDb.linkStatusRecords()->linkStatusMap();
    EXPECT_EQ(linkStatusMap->size(), 1);
    EXPECT_EQ(linkStatusMap->count(ifName), 1);
    EXPECT_EQ(*linkStatusMap->at(ifName).status(), thrift::LinkStatusEnum::UP);
    ifTs = *linkStatusMap->at(ifName).unixTs();
  }

  // Flip the interface DOWN and so neighbor is DOWN.
  // KvStore should still have one entry in LinkStatusRecords, but status is
  // now DOWN and timestamp is larger.
  {
    nlEventsInjector->sendLinkEvent(ifName, 100, false /* down */);
    recvAndReplyIfUpdate();
    auto neighborEvent = nb2_down_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(neighborEvent)})));

    std::optional<thrift::Value> value = getPublicationValueForKey(key);
    auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
        value->value().value(), serializer);
    printAdjDb(adjDb);
    ASSERT_TRUE(adjDb.linkStatusRecords().has_value());
    auto linkStatusMap = adjDb.linkStatusRecords()->linkStatusMap();
    EXPECT_EQ(linkStatusMap->size(), 1);
    EXPECT_EQ(linkStatusMap->count(ifName), 1);
    EXPECT_EQ(
        *linkStatusMap->at(ifName).status(), thrift::LinkStatusEnum::DOWN);
    EXPECT_GE(*linkStatusMap->at(ifName).unixTs(), ifTs);
    ifTs = *linkStatusMap->at(ifName).unixTs();
  }

  // Flip the interface back to UP with UP neighbor. And then, push
  // neighbor DOWN but link to it is still UP (i.e., neighbor crashes).
  // KvStore should still have one entry in LinkStatusRecords, but status is
  // now DOWN (last status) and timestamp is the largest.
  {
    nlEventsInjector->sendLinkEvent(ifName, 100, true /* up */);
    recvAndReplyIfUpdate();
    auto upNeighborEvent = nb2_up_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(upNeighborEvent)})));

    auto downNeighborEvent = nb2_down_event;
    neighborUpdatesQueue.push(
        NeighborInitEvent(NeighborEvents({std::move(downNeighborEvent)})));

    std::optional<thrift::Value> value = getPublicationValueForKey(key);
    auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
        value->value().value(), serializer);
    printAdjDb(adjDb);
    ASSERT_TRUE(adjDb.linkStatusRecords().has_value());
    auto linkStatusMap = adjDb.linkStatusRecords()->linkStatusMap();
    EXPECT_EQ(linkStatusMap->size(), 1);
    EXPECT_EQ(linkStatusMap->count(ifName), 1);
    EXPECT_EQ(
        *linkStatusMap->at(ifName).status(), thrift::LinkStatusEnum::DOWN);
    EXPECT_GE(*linkStatusMap->at(ifName).unixTs(), ifTs);
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  testing::InitGoogleMock(&argc, argv);
  const folly::Init init(&argc, &argv);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // Run the tests
  auto rc = RUN_ALL_TESTS();
  return rc;
}
