/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Format.h>
#include <folly/Subprocess.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/system/Shell.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <fbzmq/zmq/Zmq.h>
#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/mocks/NetlinkEventsInjector.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;
using namespace folly::literals::shell_literals;

using ::testing::InSequence;

// node-1 connects node-2 via interface iface_2_1 and iface_2_2, node-3 via
// interface iface_3_1
namespace {

using Area2SRNodeLabel =
    std::unordered_map<std::string, thrift::SegmentRoutingNodeLabel>;

const auto nb2_v4_addr = "192.168.0.2";
const auto nb3_v4_addr = "192.168.0.3";
const auto nb2_v6_addr = "fe80::2";
const auto nb3_v6_addr = "fe80::3";
const auto if_2_1 = "iface_2_1";
const auto if_2_2 = "iface_2_2";
const auto if_3_1 = "iface_3_1";
const auto if_3_2 = "iface_3_2";

const auto peerSpec_2_1 = createPeerSpec(
    fmt::format(
        "tcp://[{}%{}]:{}", nb2_v6_addr, if_2_1, Constants::kKvStoreRepPort),
    fmt::format("{}%{}", nb2_v6_addr, if_2_1),
    1);

const auto peerSpec_2_2 = createPeerSpec(
    fmt::format(
        "tcp://[{}%{}]:{}", nb2_v6_addr, if_2_2, Constants::kKvStoreRepPort),
    fmt::format("{}%{}", nb2_v6_addr, if_2_2),
    1);

const auto peerSpec_3_1 = createPeerSpec(
    fmt::format(
        "tcp://[{}%{}]:{}", nb3_v6_addr, if_3_1, Constants::kKvStoreRepPort),
    fmt::format("{}%{}", nb3_v6_addr, if_3_1),
    2);

const auto nb2 = createSparkNeighbor(
    "node-2",
    toBinaryAddress(folly::IPAddress(nb2_v4_addr)),
    toBinaryAddress(folly::IPAddress(nb2_v6_addr)),
    Constants::kKvStoreRepPort, /* ZMQ kvstore port */
    1, /* openrCtrlThriftPort */
    1, /* label */
    100, /* rtt */
    "", /* remote interface name */
    if_2_1 /* local interface name */);

const auto nb3 = createSparkNeighbor(
    "node-3",
    toBinaryAddress(folly::IPAddress(nb3_v4_addr)),
    toBinaryAddress(folly::IPAddress(nb3_v6_addr)),
    Constants::kKvStoreRepPort, /* ZMQ kvstore port */
    2, /* openrCtrlThriftPort */
    1, /* label */
    100, /* rtt */
    "", /* remote interface name */
    if_3_1 /* local interface name */);

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

const auto adj_3_2 = createThriftAdjacency(
    "node-3",
    if_3_2,
    nb3_v6_addr,
    nb3_v4_addr,
    1 /* metric */,
    2 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

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

class LinkMonitorTestFixture : public testing::Test {
 public:
  void
  SetUp() override {
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
    createKvStore(config);

    // create prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        staticRouteUpdatesQueue,
        kvRequestQueue,
        prefixMgrRouteUpdatesQueue,
        kvStoreWrapper->getReader(),
        prefixUpdatesQueue.getReader(),
        fibRouteUpdatesQueue.getReader(),
        config,
        kvStoreWrapper->getKvStore());
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
    kvRequestQueue.close();
    neighborUpdatesQueue.close();
    kvStoreSyncEventsQueue.close();
    prefixUpdatesQueue.close();
    fibRouteUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
    prefixMgrRouteUpdatesQueue.close();
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
    auto tConfig =
        getBasicOpenrConfig("node-1", "domain", areaCfgs, true, true);
    tConfig.persistent_config_store_path_ref() = kConfigStorePath;

    // override LM config
    tConfig.link_monitor_config_ref()->linkflap_initial_backoff_ms_ref() = 1;
    tConfig.link_monitor_config_ref()->linkflap_max_backoff_ms_ref() = 8;
    tConfig.link_monitor_config_ref()->use_rtt_metric_ref() = false;

    return tConfig;
  }

  virtual std::vector<thrift::AreaConfig>
  createAreaConfigs() {
    return populateAreaConfigs({}, {});
  }

  /*
   * [Util methods]
   *
   * Methods serves for util purpose to do:
   *  1) thread/module creation/destruction;
   *  2) validation method for unit-test usage;
   *  3) etc.
   */
  thrift::SegmentRoutingNodeLabel
  populateSegmentRoutingNodeLabel(
      thrift::SegmentRoutingNodeLabelType nodeLabelType, int32_t nodeLabel) {
    thrift::SegmentRoutingNodeLabel srNodeLabel;
    srNodeLabel.sr_node_label_type_ref() = nodeLabelType;
    srNodeLabel.node_segment_label_ref() = nodeLabel;
    return srNodeLabel;
  }

  std::vector<thrift::AreaConfig>
  populateAreaConfigs(
      std::unordered_set<std::string> areas, Area2SRNodeLabel areaMap) {
    // Use kTestingAreaName by default
    if (areas.empty()) {
      areas.insert(kTestingAreaName);
    }

    std::vector<openr::thrift::AreaConfig> areaConfig;
    int32_t nodeLabelBase = 101;

    // iterate and construct config
    for (auto areaId : areas) {
      thrift::AreaConfig cfg;
      cfg.area_id_ref() = areaId;
      cfg.include_interface_regexes_ref() = {
          kTestVethNamePrefix + ".*", "iface.*"};
      cfg.redistribute_interface_regexes_ref() = {"loopback"};

      auto it = areaMap.find(areaId);
      if (it == areaMap.end()) {
        cfg.area_sr_node_label_ref() = populateSegmentRoutingNodeLabel(
            thrift::SegmentRoutingNodeLabelType::STATIC, nodeLabelBase++);
      } else {
        thrift::SegmentRoutingNodeLabel srNodeLabel;
        openr::thrift::LabelRange labelRange;
        srNodeLabel.sr_node_label_type_ref() =
            *it->second.sr_node_label_type_ref();

        if (it->second.node_segment_label_range_ref().has_value()) {
          labelRange.start_label_ref() =
              *it->second.get_node_segment_label_range()->start_label_ref();
          labelRange.end_label_ref() =
              *it->second.get_node_segment_label_range()->end_label_ref();
          srNodeLabel.node_segment_label_range_ref() = labelRange;
        }

        if (it->second.node_segment_label_ref().has_value()) {
          srNodeLabel.node_segment_label_ref() =
              *it->second.get_node_segment_label();
        }
        cfg.area_sr_node_label_ref() = srNodeLabel;
      }
      areaConfig.emplace_back(std::move(cfg));
    }
    return areaConfig;
  }

  void
  createKvStore(std::shared_ptr<Config> config) {
    kvStoreWrapper = std::make_unique<KvStoreWrapper>(
        context,
        config,
        peerUpdatesQueue.getReader(),
        kvRequestQueue.getReader());
    kvStoreWrapper->run();
  }

  void
  createLinkMonitor(
      std::shared_ptr<Config> config, bool overrideDrainState = false) {
    linkMonitor = std::make_unique<LinkMonitor>(
        config,
        nlSock.get(),
        kvStoreWrapper->getKvStore(),
        configStore.get(),
        interfaceUpdatesQueue,
        prefixUpdatesQueue,
        peerUpdatesQueue,
        logSampleQueue,
        kvRequestQueue,
        neighborUpdatesQueue.getReader(),
        kvStoreSyncEventsQueue.getReader(),
        nlSock->getReader(),
        overrideDrainState);

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
    auto update = interfaceUpdatesReader.get().value();

    // ATTN: update class variable `sparkIfDb` for later verification
    if (auto* db = std::get_if<InterfaceDatabase>(&update)) {
      sparkIfDb = *db;
      LOG(INFO) << "----------- Interface Updates ----------";
      for (const auto& info : sparkIfDb) {
        LOG(INFO) << "  Name=" << info.ifName << ", Status=" << info.isUp
                  << ", IfIndex=" << info.ifIndex
                  << ", networks=" << info.networks.size();
      }
    } else if (
        auto* isAdjDbSynced =
            std::get_if<thrift::InitializationEvent>(&update)) {
      ASSERT_TRUE(0);
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
  using CollatedIfData = struct {
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
        } else if (network.first.isV6() and network.first.isLinkLocal()) {
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
      std::string const& key, std::string const& area = kTestingAreaName) {
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

      auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
          value->value_ref().value(), serializer);
      // we can not know what the nodeLabel will be
      adjDb.nodeLabel_ref() = kNodeLabel;
      // nor the timestamp, so we override with our predefinded const values
      for (auto& adj : *adjDb.adjacencies_ref()) {
        adj.timestamp_ref() = kTimestamp;
      }

      LOG(INFO) << "[RECEIVED ADJ]";
      printAdjDb(adjDb);
      auto adjDbstr = writeThriftObjStr(adjDb, serializer);
      auto expectedAdjDbstr =
          writeThriftObjStr(expectedAdjDbs.front(), serializer);
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
      AreaId const& area = kTestingAreaName) {
    auto const peers = kvStoreWrapper->getPeers(area);
    EXPECT_EQ(peers.count(nodeName), 1);
    if (!peers.count(nodeName)) {
      return;
    }
    EXPECT_EQ(*peers.at(nodeName).cmdUrl_ref(), *peerSpec.cmdUrl_ref());
    EXPECT_EQ(*peers.at(nodeName).peerAddr_ref(), *peerSpec.peerAddr_ref());
    EXPECT_EQ(*peers.at(nodeName).ctrlPort_ref(), *peerSpec.ctrlPort_ref());
  }

  std::unordered_map<folly::CIDRNetwork, thrift::PrefixEntry>
  getNextPrefixDb(
      std::string const& originatorId, AreaId const& area = kTestingAreaName) {
    std::unordered_map<folly::CIDRNetwork, thrift::PrefixEntry> prefixes;

    // Leverage KvStoreFilter to get `prefix:*` change
    std::optional<KvStoreFilters> kvFilters{KvStoreFilters(
        {Constants::kPrefixDbMarker.toString()}, {originatorId})};
    auto kvs = kvStoreWrapper->dumpAll(area, std::move(kvFilters));
    for (const auto& [key, val] : kvs) {
      if (auto value = val.value_ref()) {
        auto prefixDb =
            readThriftObjStr<thrift::PrefixDatabase>(*value, serializer);
        // skip prefix entries marked as deleted
        if (*prefixDb.deletePrefix_ref()) {
          continue;
        }
        for (auto const& prefixEntry : *prefixDb.prefixEntries_ref()) {
          prefixes.emplace(toIPNetwork(*prefixEntry.prefix_ref()), prefixEntry);
        }
      }
    }
    return prefixes;
  }

  void
  stopLinkMonitor() {
    // close queue first
    neighborUpdatesQueue.close();
    kvStoreSyncEventsQueue.close();
    nlSock->closeQueue();
    kvStoreWrapper->closeQueue();

    linkMonitor->stop();
    linkMonitorThread->join();
    linkMonitor.reset();
  }

  fbzmq::Context context{};
  folly::EventBase nlEvb_;
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock{nullptr};

  messaging::ReplicateQueue<InterfaceEvent> interfaceUpdatesQueue;
  messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue;
  messaging::ReplicateQueue<NeighborEvents> neighborUpdatesQueue;
  messaging::ReplicateQueue<KvStoreSyncEvent> kvStoreSyncEventsQueue;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> prefixMgrRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  messaging::RQueue<InterfaceEvent> interfaceUpdatesReader{
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
  EXPECT_EQ(maybeValue.value().get_version(), 1);
  EXPECT_EQ(*maybeValue.value().value_ref(), adjacencies);
}

// Start LinkMonitor and ensure empty adjacency database and prefixes are
// received upon initial hold-timeout expiry
TEST_F(LinkMonitorTestFixture, NoNeighborEvent) {
  // Verify that we receive empty adjacency database
  expectedAdjDbs.push(createAdjDb("node-1", {}, kNodeLabel));
  checkNextAdjPub("adj:node-1");
}

// Start LinkMonitor and ensure drain state are set correctly according to
// parameters
TEST_F(LinkMonitorTestFixture, DrainState) {
  // 1. default setup:
  // persistent store == null, assume_drain = false, override_drain_state =
  // isOverloaded should be read from assume_drain, = false
  auto res = linkMonitor->semifuture_getInterfaces().get();
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
  kvStoreSyncEventsQueue.open();
  nlSock->openQueue();
  kvStoreWrapper->openQueue();
  createLinkMonitor(config, false /*overrideDrainState*/);
  // checkNextAdjPub("adj:node-1");

  res = linkMonitor->semifuture_getInterfaces().get();
  ASSERT_NE(nullptr, res);
  EXPECT_TRUE(*res->isOverloaded_ref());

  // 3. restart with override_drain_state = true
  // override_drain_state = true, assume_drain = false, persistent store has
  // overload = true isOverloaded should be read from assume_drain store, =
  // false
  stopLinkMonitor();

  // Create new neighbor update queue. Previous one is closed
  neighborUpdatesQueue.open();
  kvStoreSyncEventsQueue.open();
  nlSock->openQueue();
  kvStoreWrapper->openQueue();
  createLinkMonitor(config, true /*overrideDrainState*/);

  res = linkMonitor->semifuture_getInterfaces().get();
  ASSERT_NE(nullptr, res);
  EXPECT_FALSE(*res->isOverloaded_ref());
}

// receive neighbor up/down events from "spark"
// form peer connections and inform KvStore of adjacencies
TEST_F(LinkMonitorTestFixture, BasicOperation) {
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
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit set
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;
      adj_2_1_modified.isOverloaded_ref() = true;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;
      adj_2_1_modified.isOverloaded_ref() = true;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value unset
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // set adjacency metric, this should override the link metric
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = adjMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // unset adjacency metric, this will remove the override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDb("node-1", {}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // link-monitor and config-store is restarted but state will be
      // retained. expect neighbor up with overrides
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric_ref() = linkMetric;

      auto adjDb = createAdjDb("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDb("node-1", {}, kNodeLabel);
      adjDb.isOverloaded_ref() = true;
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    LOG(INFO) << "Sent neighbor UP event.";

    // Makes sure peerUpdatesQueue gets PeerEvent.
    auto peerEvent = peerUpdatesQueue.getReader().get();
    ASSERT_TRUE(peerEvent.hasValue());
    EXPECT_EQ(peerEvent.value().size(), 1);

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }

  // kvstore peer initial sync
  {
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));
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
    auto ret = linkMonitor->semifuture_setNodeOverload(true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    auto res = linkMonitor->semifuture_getInterfaces().get();
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
    ret =
        linkMonitor->semifuture_setLinkMetric(interfaceName, linkMetric).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set link overload command!";
    ret =
        linkMonitor->semifuture_setInterfaceOverload(interfaceName, true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = linkMonitor->semifuture_getInterfaces().get();
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
    ret = linkMonitor->semifuture_setNodeOverload(false).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = linkMonitor->semifuture_getInterfaces().get();
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
    ret = linkMonitor->semifuture_setInterfaceOverload(interfaceName, false)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing unset link metric command!";
    ret = linkMonitor->semifuture_setLinkMetric(interfaceName, std::nullopt)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    res = linkMonitor->semifuture_getInterfaces().get();
    EXPECT_FALSE(*res->isOverloaded_ref());
    EXPECT_FALSE(
        *res->interfaceDetails_ref()->at(interfaceName).isOverloaded_ref());
    EXPECT_FALSE(res->interfaceDetails_ref()
                     ->at(interfaceName)
                     .metricOverride_ref()
                     .has_value());

    LOG(INFO) << "Testing set node overload command( AGAIN )!";
    ret = linkMonitor->semifuture_setNodeOverload(true).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set link metric command( AGAIN )!";
    ret =
        linkMonitor->semifuture_setLinkMetric(interfaceName, linkMetric).get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set adj metric command!";
    ret =
        linkMonitor
            ->semifuture_setAdjacencyMetric(interfaceName, nodeName, adjMetric)
            .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing unset adj metric command!";
    ret = linkMonitor
              ->semifuture_setAdjacencyMetric(
                  interfaceName, nodeName, std::nullopt)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
  }

  // 11. neighbor down
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_DOWN, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    LOG(INFO) << "Testing neighbor down event!";
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
  neighborUpdatesQueue = messaging::ReplicateQueue<NeighborEvents>();
  kvStoreSyncEventsQueue = messaging::ReplicateQueue<KvStoreSyncEvent>();
  peerUpdatesQueue = messaging::ReplicateQueue<PeerEvent>();
  kvRequestQueue = messaging::ReplicateQueue<KeyValueRequest>();
  nlSock->openQueue();

  // Recreate KvStore as previous kvStoreUpdatesQueue is closed
  createKvStore(config);

  // mock "restarting" link monitor with existing config store
  createLinkMonitor(config);

  // 12. neighbor up
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));
    LOG(INFO) << "Testing adj up event!";
    checkNextAdjPub("adj:node-1");
  }

  // 13. neighbor down with empty address
  {
    thrift::BinaryAddress empty;
    auto cp = nb2;
    *cp.transportAddressV4_ref() = empty;
    *cp.transportAddressV6_ref() = empty;
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_DOWN, cp);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    LOG(INFO) << "Testing neighbor down event with empty address!";
    checkNextAdjPub("adj:node-1");
  }
}

// Test linkMonitor restarts to honor `enableSegmentRouting` flag
TEST_F(LinkMonitorTestFixture, NodeLabelRemoval) {
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
    kvStoreSyncEventsQueue.open();
    nlSock->openQueue();
    kvStoreWrapper->openQueue();

    // ATTN: intentionally set `enableSegmentRouting = false` to test the
    //       config_ load scenario.
    auto tConfigCopy = createConfig();
    tConfigCopy.enable_segment_routing_ref() = false;
    auto configSegmentRoutingDisabled = std::make_shared<Config>(tConfigCopy);
    createLinkMonitor(configSegmentRoutingDisabled);

    // nodeLabel is non-zero value read from config_, override to 0 to
    // honor flag.
    auto thriftAdjDbs = linkMonitor->semifuture_getAdjacencies().get();
    EXPECT_EQ(1, thriftAdjDbs->size());
    EXPECT_EQ(0, thriftAdjDbs->at(0).get_nodeLabel());
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
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb3);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }

  // initial sync event on nb2, kick advertiseAdjacenciesThrottled_
  kvStoreSyncEventsQueue.push(
      KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));
  // another initial sync event from nb3
  kvStoreSyncEventsQueue.push(
      KvStoreSyncEvent(*nb3.nodeName_ref(), kTestingAreaName));

  // before throttled function kicks in

  // neighbor 3 down immediately
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_DOWN, nb3);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
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

  auto nb2_if2 = nb2;
  nb2_if2.localIfName_ref() = if_2_2;
  nb2_if2.label_ref() = 2;
  // neighbor 2 up on iface_2_2
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    LOG(INFO) << "Sent neighbor UP event.";

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));
    checkNextAdjPub("adj:node-1");

    // check kvstore peer events: peerSpec_2_2
    checkPeerDump(*adj_2_2.otherNodeName_ref(), peerSpec_2_2);
  }

  // neighbor 2 up on iface_2_1
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // KvStore Peer has reached to the initial sync state,
    // publish Adj UP immediately
    checkNextAdjPub("adj:node-1");

    // kvstore still have peerSpec_2_2
    checkPeerDump(*adj_2_2.otherNodeName_ref(), peerSpec_2_2);
  }

  // neighbor 2 down on iface_2_2
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_DOWN, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // only iface_2_1 in adj pub
    checkNextAdjPub("adj:node-1");

    // check kvstore peer spec change to peerSpec_2_1
    checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);
  }

  // neighbor 2 down on iface_2_1
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_DOWN, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // check node-1 adj is empty
    checkNextAdjPub("adj:node-1");

    // check kvstore peer should be gone
    EXPECT_TRUE(kvStoreWrapper->getPeers(kTestingAreaName).empty());
  }

  // neighbor 2 up on iface_2_2
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    LOG(INFO) << "Sent neighbor UP event.";

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));
    checkNextAdjPub("adj:node-1");

    // check kvstore peer events: peerSpec_2_2
    checkPeerDump(*adj_2_2.otherNodeName_ref(), peerSpec_2_2);
  }

  // neighbor 3: both adj up
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb3);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    auto cp = nb3;
    cp.localIfName_ref() = if_3_2;
    cp.label_ref() = 2;
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, cp);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb3.nodeName_ref(), kTestingAreaName));
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
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));

    // check for adj update: initial adjacency db includes adj_2_1
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");

    // check kvstore received peer event
    checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);
  }

  // neighbor 2 up on adj_2_2
  auto nb2_if2 = nb2;
  nb2_if2.localIfName_ref() = if_2_2;
  nb2_if2.label_ref() = 2;
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // check adj_2_2 is published
    auto adjDb = createAdjDb("node-1", {adj_2_2, adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");

    // check kvstore peer spec still have peerSpec_2_1
    checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);
  }

  // neighbor restarting on iface_2_1 (GR)
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_RESTARTING, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  // neighbor restarting iface_2_2 (GR)
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_RESTARTING, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    // wait for this peer change to propogate
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // neighbor 2 kvstore peer2 should be gone
    EXPECT_TRUE(kvStoreWrapper->getPeers(kTestingAreaName).empty());

    // neighbor started GR, no adj update should happen
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }

  // neighbor restarted on iface_2_1 (GR Success)
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_RESTARTED, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  // neighbor restarted on iface_2_2 (GR Success)
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_RESTARTED, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    // wait for this peer change to propogate
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));
    checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);

    // neighbor started GR, no adj update should happen
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }

  // before neighbor 2 finish initial sync, make sure additional events will
  // not accidentally trigger adj withdrawn
  // send neighbor 3 up
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb3);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb3.nodeName_ref(), kTestingAreaName));

    // adj_2_1 still in adj publication
    auto adjDb = createAdjDb("node-1", {adj_3_1, adj_2_1, adj_2_2}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");
  }

  // send adj_2_1 rtt event
  // make sure adj_2_1 is still getting advertised
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_RTT_CHANGE, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // wait for this rtt change to propogate
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // no adj events
    CHECK_EQ(0, kvStoreWrapper->getReader().size());
  }

  // neighbor 2 kvstore initial sync adj_2_1, adj_2_2 exit GR mode
  {
    // kvstore peer initial sync
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));

    // wait for this peer change to propogate
    /* sleep override */
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
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));

    // check for adj update: initial adjacency db includes adj_2_1
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");

    // check kvstore received peer event
    checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);
  }

  // neighbor 2 up on adj_2_2
  auto nb2_if2 = nb2;
  nb2_if2.localIfName_ref() = if_2_2;
  nb2_if2.label_ref() = 2;
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // check adj_2_2 is published
    auto adjDb = createAdjDb("node-1", {adj_2_2, adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");

    // check kvstore peer spec still have peerSpec_2_1
    checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);
  }

  // neighbor restarting on iface_2_1 (GR)
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_RESTARTING, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  // neighbor restarting iface_2_2 (GR)
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_RESTARTING, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
  }
  {
    // wait for this peer change to propogate
    /* sleep override */
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
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb3);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    // no adj up before KvStore Peer finish initial sync
    CHECK_EQ(0, kvStoreWrapper->getReader().size());

    // kvstore peer initial sync
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb3.nodeName_ref(), kTestingAreaName));

    // adj_2_1 still in adj publication
    auto adjDb = createAdjDb("node-1", {adj_3_1, adj_2_1, adj_2_2}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");
  }

  // neighbor down on iface_2_1 (GR Failure)
  {
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_DOWN, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

    // adj_2_1 removed
    auto adjDb = createAdjDb("node-1", {adj_3_1, adj_2_2}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));

    checkNextAdjPub("adj:node-1");
  }

  // neighbor down on iface_2_2 (GR Failure)
  {
    auto neighborEvent =
        NeighborEvent(NeighborEventType::NEIGHBOR_DOWN, nb2_if2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));

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
    tConfig.link_monitor_config_ref()->linkflap_initial_backoff_ms_ref() = 2000;
    tConfig.link_monitor_config_ref()->linkflap_max_backoff_ms_ref() = 4000;

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
  auto links = linkMonitor->semifuture_getInterfaces().get();
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
    auto links1 = linkMonitor->semifuture_getInterfaces().get();
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
    auto links2 = linkMonitor->semifuture_getInterfaces().get();

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
    auto links3 = linkMonitor->semifuture_getInterfaces().get();

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

class StaticNodeLabelTestFixture : public LinkMonitorTestFixture {
 public:
  std::vector<thrift::AreaConfig>
  createAreaConfigs() override {
    Area2SRNodeLabel areaMap;
    areaMap.emplace(
        kTestingAreaName,
        populateSegmentRoutingNodeLabel(
            thrift::SegmentRoutingNodeLabelType::STATIC, 101));
    areaMap.emplace(
        kTestingSpineAreaName,
        populateSegmentRoutingNodeLabel(
            thrift::SegmentRoutingNodeLabelType::STATIC, 201));

    // create list of thrift::AreaConfig
    return populateAreaConfigs(
        {kTestingAreaName, kTestingSpineAreaName}, areaMap);
  }

 protected:
  const openr::AreaId kTestingSpineAreaName{"test_spine_area_name"};
};

/**
 * Test allocation of static node segment label in mesh area and spine area
 */
TEST_F(StaticNodeLabelTestFixture, StaticNodeLabelAlloc) {
  // spin up kNumNodesToTest - 1 new link monitors. 1 is spun up in setup()
  size_t kNumNodesToTest = 10;
  std::vector<std::unique_ptr<LinkMonitor>> linkMonitors;
  std::vector<std::unique_ptr<std::thread>> linkMonitorThreads;

  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    Area2SRNodeLabel mp;
    mp.emplace(
        kTestingAreaName,
        populateSegmentRoutingNodeLabel(
            thrift::SegmentRoutingNodeLabelType::STATIC, 102 + i));
    mp.emplace(
        kTestingSpineAreaName,
        populateSegmentRoutingNodeLabel(
            thrift::SegmentRoutingNodeLabelType::STATIC, 202 + i));

    // create thrift::AreaConfig
    auto areaCfgs =
        populateAreaConfigs({kTestingAreaName, kTestingSpineAreaName}, mp);

    // create thrift::OpenrConfig
    auto tConfig =
        getBasicOpenrConfig("node-1", "domain", areaCfgs, true, true);

    // override LM config
    tConfig.node_name_ref() = fmt::format("lm{}", i + 1);
    tConfig.persistent_config_store_path_ref() = kConfigStorePath;
    tConfig.link_monitor_config_ref()->linkflap_initial_backoff_ms_ref() = 1;
    tConfig.link_monitor_config_ref()->linkflap_max_backoff_ms_ref() = 8;
    tConfig.link_monitor_config_ref()->use_rtt_metric_ref() = false;
    auto currConfig = std::make_shared<Config>(tConfig);

    // create lm instances
    auto lm = std::make_unique<LinkMonitor>(
        currConfig,
        nlSock.get(),
        kvStoreWrapper->getKvStore(),
        configStore.get(),
        interfaceUpdatesQueue,
        prefixUpdatesQueue,
        peerUpdatesQueue,
        logSampleQueue,
        kvRequestQueue,
        neighborUpdatesQueue.getReader(),
        kvStoreSyncEventsQueue.getReader(),
        nlSock->getReader(),
        false /* overrideDrainState */);
    linkMonitors.emplace_back(std::move(lm));

    auto lmThread =
        std::make_unique<std::thread>([&]() { linkMonitors.back()->run(); });
    linkMonitorThreads.emplace_back(std::move(lmThread));
    linkMonitors.back()->waitUntilRunning();
  }

  // label to node mapping to make sure unique label is assigned
  std::unordered_map<int32_t, std::string> label2NodeMap1;
  std::unordered_map<int32_t, std::string> label2NodeMap2;
  auto unknownAreaPublications = 0;

  // recv kv store publications until we have valid labels from each node
  while (label2NodeMap1.size() < kNumNodesToTest or
         label2NodeMap2.size() < kNumNodesToTest) {
    auto pub = kvStoreWrapper->recvPublication();
    for (auto const& [key, val] : *pub.keyVals_ref()) {
      // skip non-adj key update or ttl update
      if (key.find("adj:") != 0 or (not val.value_ref())) {
        continue;
      }

      auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
          val.value_ref().value(), serializer);
      if (pub.get_area() == static_cast<std::string>(kTestingAreaName)) {
        // kTestingAreaName node segment label
        label2NodeMap1[adjDb.get_nodeLabel()] = adjDb.get_thisNodeName();
      } else if (
          pub.get_area() == static_cast<std::string>(kTestingSpineAreaName)) {
        // kTestingSpineAreaName node segment label
        label2NodeMap2[adjDb.get_nodeLabel()] = adjDb.get_thisNodeName();
      } else {
        unknownAreaPublications++;
      }
    }
  }
  EXPECT_EQ(unknownAreaPublications, 0);

  // cleanup
  nlSock->closeQueue();
  neighborUpdatesQueue.close();
  kvStoreSyncEventsQueue.close();
  kvStoreWrapper->closeQueue();
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    linkMonitors[i]->stop();
    linkMonitorThreads[i]->join();
  }
}

class TwoAreaTestFixture : public LinkMonitorTestFixture {
 public:
  std::vector<thrift::AreaConfig>
  createAreaConfigs() override {
    return populateAreaConfigs({area1_, area2_}, {});
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
    std::unordered_map<folly::CIDRNetwork, thrift::PrefixEntry> prefixesArea1,
        prefixesArea2;
    do {
      prefixesArea1 = getNextPrefixDb(nodeName, area1_);
      prefixesArea2 = getNextPrefixDb(nodeName, area2_);
    } while (prefixesArea1.size() != 5 or prefixesArea2.size() != 5);

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
    std::unordered_map<folly::CIDRNetwork, thrift::PrefixEntry> prefixesArea1,
        prefixesArea2;
    do {
      prefixesArea1 = getNextPrefixDb(nodeName, area1_);
      prefixesArea2 = getNextPrefixDb(nodeName, area2_);
    } while (prefixesArea1.size() != 1 or prefixesArea2.size() != 1);

    ASSERT_EQ(
        1,
        prefixesArea1.count(folly::IPAddress::createNetwork(loopbackAddrV6_2)));
    auto& prefixEntry =
        prefixesArea1.at(folly::IPAddress::createNetwork(loopbackAddrV6_2));
    EXPECT_EQ(
        Constants::kDefaultPathPreference,
        prefixEntry.metrics_ref()->path_preference_ref());
    EXPECT_EQ(
        Constants::kDefaultSourcePreference,
        prefixEntry.metrics_ref()->source_preference_ref());
    EXPECT_EQ(
        std::set<std::string>(
            {"INTERFACE_SUBNET", fmt::format("{}:loopback", nodeName)}),
        prefixEntry.tags_ref());

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
    std::unordered_map<folly::CIDRNetwork, thrift::PrefixEntry> prefixesArea1,
        prefixesArea2;
    do {
      prefixesArea1 = getNextPrefixDb(nodeName, area1_);
      prefixesArea2 = getNextPrefixDb(nodeName, area2_);
    } while (prefixesArea1.size() != 0 or prefixesArea2.size() != 0);
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

class MultiAreaTestFixture : public LinkMonitorTestFixture {
 public:
  std::vector<thrift::AreaConfig>
  createAreaConfigs() override {
    return populateAreaConfigs({kTestingAreaName, podArea_, planeArea_}, {});
  }

 protected:
  const std::string podArea_{"pod"};
  const std::string planeArea_{"plane"};
};

/*
 * TODO: T101565435 to track this flaky test under OSS env and re-enable it
 *
TEST_F(MultiAreaTestFixture, AreaTest) {
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
      auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2);
      neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
      kvStoreSyncEventsQueue.push(
          KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));
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
      auto cp = nb3;
      cp.area_ref() = planeArea_;
      auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, cp);
      neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
      kvStoreSyncEventsQueue.push(
          KvStoreSyncEvent(*cp.nodeName_ref(), *cp.area_ref()));
      LOG(INFO) << "Testing neighbor UP event in plane area!";

      checkNextAdjPub("adj:node-1", planeArea_);
    }
  }
  // neighbor up on default area
  {
    auto adjDb = createAdjDb("node-1", {adj_2_1}, kNodeLabel);
    expectedAdjDbs.push(std::move(adjDb));
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, nb2);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*nb2.nodeName_ref(), kTestingAreaName));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1", kTestingAreaName);
    checkPeerDump(*adj_2_1.otherNodeName_ref(), peerSpec_2_1);
  }
  // neighbor up on "plane" area
  {
    auto adjDb =
        createAdjDb("node-1", {adj_3_1}, kNodeLabel, false, AreaId{planeArea_});
    expectedAdjDbs.push(std::move(adjDb));

    auto cp = nb3;
    cp.area_ref() = planeArea_;
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_UP, cp);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    kvStoreSyncEventsQueue.push(
        KvStoreSyncEvent(*cp.nodeName_ref(), *cp.area_ref()));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1", planeArea_);
    checkPeerDump(
        *adj_3_1.otherNodeName_ref(), peerSpec_3_1, AreaId{planeArea_});
  }
  // verify neighbor down in "plane" area
  {
    auto adjDb = createAdjDb("node-1", {}, kNodeLabel, false, planeArea_);
    expectedAdjDbs.push(std::move(adjDb));

    auto cp = nb3;
    cp.area_ref() = planeArea_;
    auto neighborEvent = NeighborEvent(NeighborEventType::NEIGHBOR_DOWN, cp);
    neighborUpdatesQueue.push(NeighborEvents({std::move(neighborEvent)}));
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1", planeArea_);
  }
}
*/

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
