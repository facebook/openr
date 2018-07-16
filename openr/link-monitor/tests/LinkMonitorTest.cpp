/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockNetlinkSystemHandler.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/BinaryProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/AddressUtil.h>
#include <openr/if/gen-cpp2/IpPrefix_types.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/platform/PlatformPublisher.h>

using namespace std;
using namespace openr;

using ::testing::InSequence;
using ::testing::InvokeWithoutArgs;
using ::testing::_;
using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;
using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;

// node-1 connects node-2 via interface iface_2_1 and iface_2_2, node-3 via
// interface iface_3_1
namespace {

re2::RE2::Options regexOpts;

const auto peerSpec_2_1 = thrift::PeerSpec(
    FRAGILE,
    "tcp://[fe80::2%iface_2_1]:10001",
    "tcp://[fe80::2%iface_2_1]:10002");

const auto peerSpec_2_2 = thrift::PeerSpec(
    FRAGILE,
    "tcp://[fe80::2%iface_2_2]:10001",
    "tcp://[fe80::2%iface_2_2]:10002");

const auto nb2 = thrift::SparkNeighbor(
    FRAGILE,
    "domain",
    "node-2",
    0, /* hold time */
    "", /* DEPRECATED - public key */
    toBinaryAddress(folly::IPAddress("fe80::2")),
    toBinaryAddress(folly::IPAddress("192.168.0.2")),
    10001,
    10002,
    "" /* ifName */);

const auto nb3 = thrift::SparkNeighbor(
    FRAGILE,
    "domain",
    "node-3",
    0, /* hold time */
    "", /* DEPRECATED - public key */
    toBinaryAddress(folly::IPAddress("fe80::3")),
    toBinaryAddress(folly::IPAddress("192.168.0.3")),
    10001,
    10002,
    "" /* ifName */);

const uint64_t kTimestamp{1000000};
const uint64_t kNodeLabel{0};

const auto adj_2_1 = thrift::Adjacency(
    FRAGILE,
    "node-2",
    "iface_2_1",
    toBinaryAddress(folly::IPAddress("fe80::2")),
    toBinaryAddress(folly::IPAddress("192.168.0.2")),
    1 /* metric */,
    1 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

const auto adj_2_2 = thrift::Adjacency(
    FRAGILE,
    "node-2",
    "iface_2_2",
    toBinaryAddress(folly::IPAddress("fe80::2")),
    toBinaryAddress(folly::IPAddress("192.168.0.2")),
    1 /* metric */,
    2 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */);

const auto staticPrefix1 = toIpPrefix("fc00:face:b00c::/64");
const auto staticPrefix2 = toIpPrefix("fc00:cafe:babe::/64");

thrift::SparkNeighborEvent
createNeighborEvent(
    thrift::SparkNeighborEventType eventType,
    const std::string& ifName,
    const thrift::SparkNeighbor& neighbor,
    int64_t rttUs,
    int32_t label) {
  return thrift::SparkNeighborEvent(
      FRAGILE, eventType, ifName, neighbor, rttUs, label);
}

thrift::AdjacencyDatabase
createAdjDatabase(
    const std::string& thisNodeName,
    const std::vector<thrift::Adjacency>& adjacencies,
    int32_t nodeLabel) {
  return createAdjDb(thisNodeName, adjacencies, nodeLabel);
}

void
printAdjDb(const thrift::AdjacencyDatabase& adjDb) {
  LOG(INFO) << "Node: " << adjDb.thisNodeName
            << ", Overloaded: " << adjDb.isOverloaded
            << ", Label: " << adjDb.nodeLabel;
  for (auto const& adj : adjDb.adjacencies) {
    LOG(INFO) << "  " << adj.otherNodeName << "@" << adj.ifName
              << ", metric: " << adj.metric << ", label: " << adj.adjLabel
              << ", overloaded: " << adj.isOverloaded << ", rtt: " << adj.rtt
              << ", ts: " << adj.timestamp << ", " << toString(adj.nextHopV4)
              << ", " << toString(adj.nextHopV6);
  }
}

void
printIntfDb(const thrift::InterfaceDatabase& intfDb) {
  LOG(INFO) << "Node: " << intfDb.thisNodeName;
  for (auto const& kv : intfDb.interfaces) {
    std::vector<std::string> addrs;
    for (auto const& addr : kv.second.networks) {
      addrs.emplace_back(toString(addr.prefixAddress));
    }
    LOG(INFO) << "  Interface => name: " << kv.first
              << ", Status: " << kv.second.isUp
              << ", Index: " << kv.second.ifIndex
              << ", Addrs: " << folly::join(",", addrs);
  }
}

const std::string kTestVethNamePrefix = "vethLMTest";
const std::vector<uint64_t> kTestVethIfIndex = {1240, 1241, 1242};
const std::string kConfigStorePath = "/tmp/lm_ut_config_store.bin";
const std::string kConfigStoreUrl = "inproc://lm_ut_config_store";
} // namespace

class LinkMonitorTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // Cleanup any existing config file on disk
    system(folly::sformat("rm -rf {}", kConfigStorePath).c_str());

    mockNlHandler = std::make_shared<MockNetlinkSystemHandler>(
        context, "inproc://platform-pub-url");

    server = make_shared<ThriftServer>();
    server->setPort(0);
    server->setInterface(mockNlHandler);

    systemThriftThread.start(server);
    port = systemThriftThread.getAddress()->getPort();

    // spin up a config store
    configStore = std::make_unique<PersistentStore>(
        kConfigStorePath, PersistentStoreUrl{kConfigStoreUrl}, context);

    configStoreThread = std::make_unique<std::thread>([this]() noexcept {
      LOG(INFO) << "ConfigStore thread starting";
      configStore->run();
      LOG(INFO) << "ConfigStore thread finishing";
    });
    configStore->waitUntilRunning();

    // spin up a kvstore
    kvStoreWrapper = std::make_shared<KvStoreWrapper>(
        context,
        "test_store1",
        std::chrono::seconds(1) /* db sync interval */,
        std::chrono::seconds(600) /* counter submit interval */,
        std::unordered_map<std::string, thrift::PeerSpec>{});
    kvStoreWrapper->run();
    LOG(INFO) << "The test KV store is running";

    // create prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        "node-1",
        PrefixManagerGlobalCmdUrl{"inproc://prefix-manager-global-url"},
        PrefixManagerLocalCmdUrl{"inproc://prefix-manager-local-url"},
        PersistentStoreUrl{kConfigStoreUrl},
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
        false,
        MonitorSubmitUrl{"inproc://monitor_submit"},
        context);
    prefixManagerThread = std::make_unique<std::thread>([this] {
      LOG(INFO) << "prefix manager starting";
      prefixManager->run();
      LOG(INFO) << "prefix manager stopped";
    });

    // spark reports neighbor up
    EXPECT_NO_THROW(
        sparkReport.bind(fbzmq::SocketUrl{"inproc://spark-report"}).value());

    // spark responses to if events
    EXPECT_NO_THROW(
        sparkIfDbResp.bind(fbzmq::SocketUrl{"inproc://spark-req"}).value());

    regexOpts.set_case_sensitive(false);
    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
    includeRegexList->Add("iface.*", &regexErr);
    includeRegexList->Compile();

    std::unique_ptr<re2::RE2::Set> excludeRegexList;

    auto redistRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    redistRegexList->Add("loopback", &regexErr);
    redistRegexList->Compile();

    // start a link monitor
    linkMonitor = make_shared<LinkMonitor>(
        context,
        "node-1",
        port, /* thrift service port */
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        std::move(excludeRegexList),
        std::move(redistRegexList), // redistribute interface name
        std::vector<thrift::IpPrefix>{staticPrefix1, staticPrefix2},
        false /* useRttMetric */,
        false /* enable full mesh reduction */,
        false /* enable perf measurement */,
        true /* enable v4 */,
        true /* advertise interface db */,
        true /* enable segment routing */,
        AdjacencyDbMarker{"adj:"},
        InterfaceDbMarker{"intf:"},
        SparkCmdUrl{"inproc://spark-req"},
        SparkReportUrl{"inproc://spark-report"},
        MonitorSubmitUrl{"inproc://monitor-rep"},
        PersistentStoreUrl{kConfigStoreUrl},
        false,
        PrefixManagerLocalCmdUrl{"inproc://prefix-manager-local-url"},
        PlatformPublisherUrl{"inproc://platform-pub-url"},
        LinkMonitorGlobalPubUrl{"inproc://link-monitor-pub-url"},
        LinkMonitorGlobalCmdUrl{"inproc://link-monitor-cmd-url"},
        std::chrono::seconds(1),
        // link flap backoffs, set low to keep UT runtime low
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8));

    linkMonitorThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitor->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitor->waitUntilRunning();

    EXPECT_NO_THROW(
        lmCmdSocket.connect(fbzmq::SocketUrl{"inproc://link-monitor-cmd-url"})
            .value());
  }

  void
  TearDown() override {
    LOG(INFO) << "LinkMonitor test/basic operations is done";

    LOG(INFO) << "Stopping prefix manager thread";
    prefixManager->stop();
    prefixManagerThread->join();

    // this will be invoked before linkMonitorThread's d-tor
    LOG(INFO) << "Stopping the linkMonitor thread";
    linkMonitor->stop();
    linkMonitorThread->join();
    sparkReport.close();
    sparkIfDbResp.close();

    // Erase data from config store
    PersistentStoreClient configStoreClient{PersistentStoreUrl{kConfigStoreUrl},
                                            context};
    configStoreClient.erase("link-monitor-config");

    // stop config store
    configStore->stop();
    configStoreThread->join();

    // stop the kvStore
    kvStoreWrapper->stop();
    LOG(INFO) << "The test KV store is stopped";

    // stop mocked nl platform
    mockNlHandler->stop();
    systemThriftThread.stop();
    LOG(INFO) << "Mock nl platform is stopped";
  }

  thrift::DumpLinksReply
  dumpLinks() {
    const auto dumpLinksRequest = thrift::LinkMonitorRequest(
        FRAGILE,
        thrift::LinkMonitorCommand::DUMP_LINKS,
        "" /* interface-name */,
        0 /* interface-metric */,
        "" /* adjacency-node */);
    lmCmdSocket.sendThriftObj(dumpLinksRequest, serializer).value();

    auto reply = lmCmdSocket.recvThriftObj<thrift::DumpLinksReply>(serializer);
    EXPECT_FALSE(reply.hasError());
    return reply.value();
  }

  // emulate spark keeping receiving InterfaceDb until no more udpates
  // and update sparkIfDb for every update received
  // return number of updates received
  int
  recvAndReplyIfUpdate(std::chrono::seconds timeout = std::chrono::seconds(2)) {
    int numUpdateRecv = 0;
    while (true) {
      auto ifDb = sparkIfDbResp.recvThriftObj<thrift::InterfaceDatabase>(
          serializer, std::chrono::milliseconds{timeout});
      if (ifDb.hasError()) {
        return numUpdateRecv;
      }
      sendIfDbResp(true);
      sparkIfDb = std::move(ifDb.value().interfaces);
      ++numUpdateRecv;
    }
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
      for (const auto& network : kv.second.networks) {
        const auto& ipNetwork = toIPNetwork(network);
        if (ipNetwork.first.isV4()) {
          v4AddrsCount++;
        }
        else if (ipNetwork.first.isV6() && ipNetwork.first.isLinkLocal()) {
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

  // emulate spark sending a response for IfDb
  void
  sendIfDbResp(bool isSuccess) {
    thrift::SparkIfDbUpdateResult result;
    result.isSuccess = isSuccess;
    sparkIfDbResp.sendThriftObj(result, serializer);
  }

  folly::Optional<thrift::Value>
  getPublicationValueForKey(
      std::string const& key,
      std::chrono::seconds timeout = std::chrono::seconds(10)) {
    VLOG(1) << "Waiting to receive publication for key " << key;
    auto pub = kvStoreWrapper->recvPublication(timeout);

    VLOG(1) << "Received publication with keys";
    for (auto const& kv : pub.keyVals) {
      VLOG(1) << "  " << kv.first;
    }

    auto kv = pub.keyVals.find(key);
    if (kv == pub.keyVals.end() or !kv->second.value) {
      return folly::none;
    }

    return kv->second;
  }

  // recv publicatons from kv store until we get what we were
  // expecting for a given key
  void
  checkNextAdjPub(std::string const& key) {
    CHECK(!expectedAdjDbs.empty());

    printAdjDb(expectedAdjDbs.front());

    while (true) {
      folly::Optional<thrift::Value> value;
      try {
        value = getPublicationValueForKey(key);
        if (not value.hasValue()) {
          continue;
        }
      } catch (std::exception const& e) {
        LOG(ERROR) << "Exception: " << folly::exceptionStr(e);
        EXPECT_TRUE(false);
        return;
      }

      auto adjDb = fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
          value->value.value(), serializer);
      printAdjDb(adjDb);

      // we can not know what the nodeLabel will be
      adjDb.nodeLabel = kNodeLabel;
      // nor the timestamp, so we override with our predefinded const values
      for (auto& adj : adjDb.adjacencies) {
        adj.timestamp = kTimestamp;
      }
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
  void checkPeerDump(std::string const& nodeName, thrift::PeerSpec peerSpec) {
    auto const peers = kvStoreWrapper->getPeers();
    EXPECT_EQ(peers.count(nodeName), 1);
    if (!peers.count(nodeName)) {
      return;
    }
    EXPECT_EQ(peers.at(nodeName).pubUrl, peerSpec.pubUrl);
    EXPECT_EQ(peers.at(nodeName).cmdUrl, peerSpec.cmdUrl);
  }

  void
  checkNextIntfPub(
      std::string const& key, thrift::InterfaceDatabase const& expectedIntfDb) {
    printIntfDb(expectedIntfDb);

    while (true) {
      thrift::Publication pub;

      folly::Optional<thrift::Value> value;
      try {
        value = getPublicationValueForKey(key);
        if (not value.hasValue()) {
          continue;
        }
      } catch (std::exception const& e) {
        LOG(ERROR) << "Exception: " << folly::exceptionStr(e);
        EXPECT_TRUE(false);
        return;
      }

      auto intfDb = fbzmq::util::readThriftObjStr<thrift::InterfaceDatabase>(
          value->value.value(), serializer);
      printIntfDb(intfDb);

      if (intfDb == expectedIntfDb) {
        EXPECT_TRUE(true);
        return;
      }
    }
  }

  std::unordered_set<thrift::IpPrefix>
  getNextPrefixDb(std::string const& key) {
    while (true) {
      auto value = getPublicationValueForKey(key, std::chrono::seconds(3));
      if (not value.hasValue()) {
        continue;
      }

      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          value->value.value(), serializer);
      std::unordered_set<thrift::IpPrefix> prefixes;
      for (auto const& prefixEntry : prefixDb.prefixEntries) {
        prefixes.insert(prefixEntry.prefix);
      }
      return prefixes;
    }
  }

  int port{0};
  std::shared_ptr<ThriftServer> server;
  ScopedServerThread systemThriftThread;

  fbzmq::Context context{};
  fbzmq::Socket<ZMQ_PAIR, fbzmq::ZMQ_SERVER> sparkReport{context};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> sparkIfDbResp{context};
  fbzmq::Socket<ZMQ_DEALER, fbzmq::ZMQ_CLIENT> lmCmdSocket{context};

  unique_ptr<PersistentStore> configStore;
  unique_ptr<std::thread> configStoreThread;

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<LinkMonitor> linkMonitor;
  std::unique_ptr<std::thread> linkMonitorThread;

  std::unique_ptr<PrefixManager> prefixManager;
  std::unique_ptr<std::thread> prefixManagerThread;

  std::shared_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<MockNetlinkSystemHandler> mockNlHandler;

  std::queue<thrift::AdjacencyDatabase> expectedAdjDbs;
  std::map<std::string, thrift::InterfaceInfo> sparkIfDb;
};

//
// validate getPeerDifference
//
TEST(LinkMonitorTest, PeerDifferenceTest) {
  std::unordered_map<std::string, thrift::PeerSpec> oldPeers;
  std::unordered_map<std::string, thrift::PeerSpec> newPeers;
  const std::string peerName1{"peer1"};
  const std::string peerName2{"peer2"};
  const std::string peerName3{"peer3"};
  const std::string peerName4{"peer4"};
  const std::string peerName5{"peer5"};
  const thrift::PeerSpec peerSpec1{
      apache::thrift::FRAGILE,
      "inproc://fake_pub_url_1",
      "inproc://fake_cmd_url_1"};
  const thrift::PeerSpec peerSpec2{
      apache::thrift::FRAGILE,
      "inproc://fake_pub_url_2",
      "inproc://fake_cmd_url_2"};
  const thrift::PeerSpec peerSpec3{
      apache::thrift::FRAGILE,
      "inproc://fake_pub_url_3",
      "inproc://fake_cmd_url_3"};
  const thrift::PeerSpec peerSpec4{
      apache::thrift::FRAGILE,
      "inproc://fake_pub_url_4",
      "inproc://fake_cmd_url_4"};
  const thrift::PeerSpec peerSpec5{
      apache::thrift::FRAGILE,
      "inproc://fake_pub_url_5",
      "inproc://fake_cmd_url_5"};

  oldPeers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName1),
      std::forward_as_tuple(peerSpec1));
  oldPeers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName2),
      std::forward_as_tuple(peerSpec2));
  oldPeers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName3),
      std::forward_as_tuple(peerSpec3));

  newPeers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName3),
      std::forward_as_tuple(peerSpec3));
  newPeers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName4),
      std::forward_as_tuple(peerSpec4));
  newPeers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName5),
      std::forward_as_tuple(peerSpec5));

  // expected toDelPeers
  std::vector<std::string> toDelPeersExpected{peerName1, peerName2};
  std::sort(toDelPeersExpected.begin(), toDelPeersExpected.end());
  // expected toAddPeers
  std::unordered_map<std::string, thrift::PeerSpec> toAddPeersExpected;
  toAddPeersExpected.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName4),
      std::forward_as_tuple(peerSpec4));
  toAddPeersExpected.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName5),
      std::forward_as_tuple(peerSpec5));

  std::vector<std::string> toDelPeers;
  std::unordered_map<std::string, thrift::PeerSpec> toAddPeers;
  LinkMonitor::getPeerDifference(oldPeers, newPeers, toDelPeers, toAddPeers);
  std::sort(toDelPeers.begin(), toDelPeers.end());
  EXPECT_EQ(toDelPeers, toDelPeersExpected);
  EXPECT_EQ(toAddPeers, toAddPeersExpected);
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
      mockNlHandler->sendLinkEvent("iface_2_1", 100, true);
      recvAndReplyIfUpdate();
      thrift::InterfaceDatabase intfDb(
          FRAGILE,
          "node-1",
          {
              {
                  "iface_2_1",
                  thrift::InterfaceInfo(
                      FRAGILE,
                      true, // isUp
                      100, // ifIndex
                      {}, // v4Addrs: TO BE DEPRECATED SOON
                      {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
                      {} // networks
                      ),
              },
          },
          thrift::PerfEvents());
      intfDb.perfEvents = folly::none;
      checkNextIntfPub("intf:node-1", intfDb);
    }

    {
      // expect neighbor up first
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit set
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;
      adj_2_1_modified.isOverloaded = true;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;
      adj_2_1_modified.isOverloaded = true;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

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
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // set adjacency metric, this should override the link metric
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = adjMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // unset adjacency metric, this will remove the override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // link-monitor and config-store is restarted but state will be
      // retained. expect neighbor up with overrides
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel);
      adjDb.isOverloaded = true;
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
    EXPECT_NO_THROW(
        sparkReport.sendThriftObj(neighborEvent, serializer).value());
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
  {
    const auto setOverloadRequest = thrift::LinkMonitorRequest(
        FRAGILE,
        thrift::LinkMonitorCommand::SET_OVERLOAD,
        "" /* interface-name */,
        0 /* interface-metric */,
        "" /* adjacency-node */);
    lmCmdSocket.sendThriftObj(setOverloadRequest, serializer);
    LOG(INFO) << "Testing set node overload command!";
    checkNextAdjPub("adj:node-1");

    auto links = dumpLinks();
    EXPECT_TRUE(links.isOverloaded);
    EXPECT_EQ(1, links.interfaceDetails.size());
    EXPECT_FALSE(links.interfaceDetails.at("iface_2_1").isOverloaded);
    EXPECT_FALSE(
        links.interfaceDetails.at("iface_2_1").metricOverride.hasValue());

    const auto setMetricRequest = thrift::LinkMonitorRequest(
        FRAGILE,
        thrift::LinkMonitorCommand::SET_LINK_METRIC,
        "iface_2_1" /* interface-name */,
        linkMetric /* interface-metric */,
        "" /* adjacency-node */);
    lmCmdSocket.sendThriftObj(setMetricRequest, serializer);
    LOG(INFO) << "Testing set link metric command!";
    checkNextAdjPub("adj:node-1");

    const auto setLinkOverloadRequest = thrift::LinkMonitorRequest(
        FRAGILE,
        thrift::LinkMonitorCommand::SET_LINK_OVERLOAD,
        "iface_2_1" /* interface-name */,
        0 /* interface-metric */,
        "" /* adjacency-node */);
    lmCmdSocket.sendThriftObj(setLinkOverloadRequest, serializer);
    LOG(INFO) << "Testing set link overload command!";
    checkNextAdjPub("adj:node-1");

    links = dumpLinks();
    EXPECT_TRUE(links.isOverloaded);
    EXPECT_TRUE(links.interfaceDetails.at("iface_2_1").isOverloaded);
    EXPECT_EQ(
        linkMetric,
        links.interfaceDetails.at("iface_2_1").metricOverride.value());

    const auto unsetOverloadRequest = thrift::LinkMonitorRequest(
        FRAGILE,
        thrift::LinkMonitorCommand::UNSET_OVERLOAD,
        "" /* interface-name */,
        0 /* interface-metric */,
        "" /* adjacency-node */);
    lmCmdSocket.sendThriftObj(unsetOverloadRequest, serializer);
    LOG(INFO) << "Testing unset node overload command!";
    checkNextAdjPub("adj:node-1");

    links = dumpLinks();
    EXPECT_FALSE(links.isOverloaded);
    EXPECT_TRUE(links.interfaceDetails.at("iface_2_1").isOverloaded);
    EXPECT_EQ(
        linkMetric,
        links.interfaceDetails.at("iface_2_1").metricOverride.value());

    const auto unsetLinkOverloadRequest = thrift::LinkMonitorRequest(
        FRAGILE,
        thrift::LinkMonitorCommand::UNSET_LINK_OVERLOAD,
        "iface_2_1" /* interface-name */,
        0 /* interface-metric */,
        "" /* adjacency-node */);
    lmCmdSocket.sendThriftObj(unsetLinkOverloadRequest, serializer);
    LOG(INFO) << "Testing unset link overload command!";
    checkNextAdjPub("adj:node-1");

    const auto unsetMetricRequest = thrift::LinkMonitorRequest(
        FRAGILE,
        thrift::LinkMonitorCommand::UNSET_LINK_METRIC,
        "iface_2_1" /* interface-name */,
        0 /* interface-metric */,
        "" /* adjacency-node */);
    lmCmdSocket.sendThriftObj(unsetMetricRequest, serializer);
    LOG(INFO) << "Testing unset link metric command!";
    checkNextAdjPub("adj:node-1");

    links = dumpLinks();
    EXPECT_FALSE(links.isOverloaded);
    EXPECT_FALSE(links.interfaceDetails.at("iface_2_1").isOverloaded);
    EXPECT_FALSE(
        links.interfaceDetails.at("iface_2_1").metricOverride.hasValue());

    // 8/9. Set overload bit and link metric value
    lmCmdSocket.sendThriftObj(setOverloadRequest, serializer);
    LOG(INFO) << "Testing set node overload command!";
    checkNextAdjPub("adj:node-1");
    lmCmdSocket.sendThriftObj(setMetricRequest, serializer);
    LOG(INFO) << "Testing set link metric command!";
    checkNextAdjPub("adj:node-1");
  }

  // 10. set and unset adjacency metric
  {
    auto setAdjMetricRequest = thrift::LinkMonitorRequest(
    FRAGILE,
    thrift::LinkMonitorCommand::SET_ADJ_METRIC,
    "iface_2_1" /* interface-name */,
    adjMetric /* adjacency-metric */,
    "node-2" /* adjacency-node */);
    lmCmdSocket.sendThriftObj(setAdjMetricRequest, serializer);
    LOG(INFO) << "Testing set adj metric command!";
    checkNextAdjPub("adj:node-1");

    setAdjMetricRequest.cmd = thrift::LinkMonitorCommand::UNSET_ADJ_METRIC;
    lmCmdSocket.sendThriftObj(setAdjMetricRequest, serializer);
    LOG(INFO) << "Testing unset adj metric command!";
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
    sparkReport.sendThriftObj(neighborEvent, serializer);
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1");
  }

  {
    // mock "restarting" link monitor with existing config store
    // use different endpoints for sockets
    // simply close() and bind/connect again do not work

    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
    includeRegexList->Compile();
    std::unique_ptr<re2::RE2::Set> excludeRegexList;
    std::unique_ptr<re2::RE2::Set> redistRegexList;

    auto linkMonitor = make_shared<LinkMonitor>(
        context,
        "node-1",
        port, // platform pub port
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        std::move(excludeRegexList),
        // redistribute interface names
        std::move(redistRegexList),
        std::vector<thrift::IpPrefix>{}, // static prefixes
        false /* useRttMetric */,
        false /* enable full mesh reduction */,
        false /* enable perf measurement */,
        false /* enable v4 */,
        true /* advertise interface db */,
        true /* enable segment routing */,
        AdjacencyDbMarker{"adj:"},
        InterfaceDbMarker{"intf:"},
        SparkCmdUrl{"inproc://spark-req2"},
        SparkReportUrl{"inproc://spark-report2"},
        MonitorSubmitUrl{"inproc://monitor-rep2"},
        PersistentStoreUrl{kConfigStoreUrl}, /* same config store */
        false,
        PrefixManagerLocalCmdUrl{"inproc://prefix-manager-local-url"},
        PlatformPublisherUrl{"inproc://platform-pub-url2"},
        LinkMonitorGlobalPubUrl{"inproc://link-monitor-pub-url2"},
        LinkMonitorGlobalCmdUrl{"inproc://link-monitor-cmd-url2"},
        std::chrono::seconds(1),
        // link flap backoffs, set low to keep UT runtime low
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8));

    auto linkMonitorThread = std::make_unique<std::thread>([linkMonitor]() {
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitor->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitor->waitUntilRunning();
    fbzmq::Socket<ZMQ_PAIR, fbzmq::ZMQ_SERVER> sparkReport{context};
    EXPECT_NO_THROW(
        sparkReport.bind(fbzmq::SocketUrl{"inproc://spark-report2"}).value());

    // 12. neighbor up
    {
      auto neighborEvent = createNeighborEvent(
          thrift::SparkNeighborEventType::NEIGHBOR_UP,
          "iface_2_1",
          nb2,
          100 /* rtt-us */,
          1 /* label */);

      sparkReport.sendThriftObj(neighborEvent, serializer);
      LOG(INFO) << "Testing neighbor up event!";
      checkNextAdjPub("adj:node-1");
    }

    // 13. neighbor down
    {
      auto neighborEvent = createNeighborEvent(
          thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
          "iface_2_1",
          nb2,
          100 /* rtt-us */,
          1 /* label */);
      sparkReport.sendThriftObj(neighborEvent, serializer);
      LOG(INFO) << "Testing neighbor down event!";
      checkNextAdjPub("adj:node-1");
    }

    linkMonitor->stop();
    linkMonitorThread->join();
  }
}

// Test throttling
TEST_F(LinkMonitorTestFixture, Throttle) {
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
    sparkReport.sendThriftObj(neighborEvent, serializer);
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
    sparkReport.sendThriftObj(neighborEvent, serializer);
  }

  // neighbor 3 down immediately
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendThriftObj(neighborEvent, serializer);
  }

  checkNextAdjPub("adj:node-1");
}

// parallel adjacencies between two nodes via different interfaces
TEST_F(LinkMonitorTestFixture, ParallelAdj) {
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
    sparkReport.sendThriftObj(neighborEvent, serializer);
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor up on another interface
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        2 /* label */);
    sparkReport.sendThriftObj(neighborEvent, serializer);
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendThriftObj(neighborEvent, serializer);
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_2.otherNodeName, peerSpec_2_2);
}

TEST_F(LinkMonitorTestFixture, DampenLinkFlaps) {
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  // we want much higher backoffs for this test, so lets spin up a different LM

  linkMonitor->stop();
  linkMonitorThread->join();
  linkMonitor.reset();

  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
  includeRegexList->Add("iface.*", &regexErr);
  includeRegexList->Compile();

  std::unique_ptr<re2::RE2::Set> excludeRegexList;

  auto redistRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  redistRegexList->Add("loopback", &regexErr);
  redistRegexList->Compile();

  linkMonitor = make_shared<LinkMonitor>(
      context,
      "node-1",
      port, // platform pub port
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      std::move(includeRegexList),
      std::move(excludeRegexList),
      std::move(redistRegexList), // redistribute interface name
      std::vector<thrift::IpPrefix>{staticPrefix1, staticPrefix2},
      false /* useRttMetric */,
      false /* enable full mesh reduction */,
      false /* enable perf measurement */,
      true /* enable v4 */,
      true /* advertise interface db */,
      true /* enable segment routing */,
      AdjacencyDbMarker{"adj:"},
      InterfaceDbMarker{"intf:"},
      SparkCmdUrl{"inproc://spark-req"},
      SparkReportUrl{"inproc://spark-report"},
      MonitorSubmitUrl{"inproc://monitor-rep"},
      PersistentStoreUrl{kConfigStoreUrl},
      false,
      PrefixManagerLocalCmdUrl{"inproc://prefix-manager-local-url"},
      PlatformPublisherUrl{"inproc://platform-pub-url"},
      LinkMonitorGlobalPubUrl{"inproc://link-monitor-pub-url2"},
      LinkMonitorGlobalCmdUrl{"inproc://link-monitor-cmd-url2"},
      std::chrono::seconds(1),
      // link flap backoffs, set high backoffs for this test
      std::chrono::milliseconds(1500),
      std::chrono::milliseconds(20000));

    linkMonitorThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitor->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitor->waitUntilRunning();

    // connect cmd socket to updated url
    EXPECT_NO_THROW(
        lmCmdSocket.connect(fbzmq::SocketUrl{"inproc://link-monitor-cmd-url2"})
            .value());


  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);

  {
    // Both interfaces report as down on creation
    // expect sparkIfDb to have two interfaces DOWN
    recvAndReplyIfUpdate();
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
  const std::chrono::milliseconds flapInterval{10};

  // Bringing up the interface
  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  // at this point, both interface should have backoff=1.5sec
  auto links = dumpLinks();
  EXPECT_EQ(2, links.interfaceDetails.size());
  for (const auto& ifName : ifNames) {
    EXPECT_LE(
        links.interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 1500);
  }

  /* sleep override */
  std::this_thread::sleep_for(flapInterval);

  VLOG(2) << "*** bring down 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);

  /* sleep override */
  std::this_thread::sleep_for(flapInterval);

  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  // at this point, both interface should have backoff=6sec

  {
    // we expect all interfaces are down at this point because backoff hasn't
    // been cleared up yet
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);
    auto links1 = dumpLinks();
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links1.interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_GE(
          links1.interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 3000);
      EXPECT_LE(
          links1.interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 6000);
    }
  }

  // expect spark to receive updates in 6 seconds since first flap, at
  // this point, we consumed 2 seconds (timeout from previous read),
  // give another 4 seconds to recv commands on spark should be enough
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(4));
  {
    recvAndReplyIfUpdate();
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
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  // at this point, both interface should have backoff=12sec

  {
    // expect sparkIfDb to have two interfaces DOWN
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);
    auto links2 = dumpLinks();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links2.interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_GE(
          links2.interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 6000);
      EXPECT_LE(
          links2.interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 12000);
    }
  }

  /* sleep override */
  std::this_thread::sleep_for(flapInterval);

  // Bringing up the interfaces
  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  // at this point, both interface should have backoff back to init value

  // make sure to pause long enough to clear out back off timers
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(30));

  {
    // expect sparkIfDb to have two interfaces UP
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);
    auto links3 = dumpLinks();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links3.interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_GE(
          links3.interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 0);
      EXPECT_LE(
          links3.interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 1500);
    }
  }
}

// Test Interface events to Spark
TEST_F(LinkMonitorTestFixture, verifyLinkEventSubscription) {
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);

  // Both interfaces report as down on creation
  // We receive 2 IfUpUpdates in spark for each interface
  // Both with status as false (DOWN)
  // We let spark return success for each
  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
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
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
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

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);

  // Both interfaces report as down on creation
  // We receive 2 IfUpUpdates in spark for each interface
  // Both with status as false (DOWN)
  // We let spark return success for each
  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
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

  // Emulate add address event: v4 while interfaces are down. No addr events
  // should be reported.
  mockNlHandler->sendAddrEvent(linkX, "10.0.0.1/31", true /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "10.0.0.2/31", true /* is valid */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
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

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);

  // Emulate add address event: v6 while interfaces are in UP state. Both
  // v4 and v6 addresses should be reported.
  mockNlHandler->sendAddrEvent(linkX, "fe80::1/128", true /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "fe80::2/128", true /* is valid */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
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
  mockNlHandler->sendAddrEvent(linkX, "10.0.0.1/31", false /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "10.0.0.2/31", false /* is valid */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
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
  mockNlHandler->sendAddrEvent(linkX, "fe80::1/128", false /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "fe80::2/128", false /* is valid */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
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
  const std::string linkZ = kTestVethNamePrefix + "Z";

  // Addr event comes in first
  mockNlHandler->sendAddrEvent(linkZ, "fe80::3/128", true /* is valid */);
  // Link event comes in later
  mockNlHandler->sendLinkEvent(
      linkZ /* link name */,
      kTestVethIfIndex[2] /* ifIndex */,
      true /* is up */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
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
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkZ /* link name */,
      kTestVethIfIndex[2] /* ifIndex */,
      false /* is up */);
  {
    // Both interfaces report as down on creation
    // expect sparkIfDb to have two interfaces DOWN
    recvAndReplyIfUpdate();
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
  size_t kNumNodesToTest = 10;

  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
  includeRegexList->Compile();
  std::unique_ptr<re2::RE2::Set> excludeRegexList;
  std::unique_ptr<re2::RE2::Set> redistRegexList;

  // spin up kNumNodesToTest - 1 new link monitors. 1 is spun up in setup()
  std::vector<std::unique_ptr<LinkMonitor>> linkMonitors;
  std::vector<std::unique_ptr<std::thread>> linkMonitorThreads;
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    auto lm = std::make_unique<LinkMonitor>(
        context,
        folly::sformat("lm{}", i + 1),
        0, // platform pub port
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        std::move(excludeRegexList),
        std::move(redistRegexList),
        std::vector<thrift::IpPrefix>(),
        false /* useRttMetric */,
        false /* enable full mesh reduction */,
        false /* enable perf measurement */,
        false /* enable v4 */,
        true /* advertise interface db */,
        true /* enable segment routing */,
        AdjacencyDbMarker{"adj:"},
        InterfaceDbMarker{"intf:"},
        SparkCmdUrl{"inproc://spark-req"},
        SparkReportUrl{"inproc://spark-report"},
        MonitorSubmitUrl{"inproc://monitor-rep"},
        PersistentStoreUrl{kConfigStoreUrl},
        false,
        PrefixManagerLocalCmdUrl{"inproc://prefix-manager-local-url"},
        PlatformPublisherUrl{"inproc://platform-pub-url"},
        LinkMonitorGlobalPubUrl{
            folly::sformat("inproc://link-monitor-pub-url{}", i + 1)},
        LinkMonitorGlobalCmdUrl{
            folly::sformat("inproc://link-monitor-cmd-url{}", i + 1)},
        std::chrono::seconds(1),
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8));
    linkMonitors.emplace_back(std::move(lm));

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
    auto pub = kvStoreWrapper->recvPublication(std::chrono::seconds(10));
    for (auto const& kv : pub.keyVals) {
      if (kv.first.find("adj:") == 0 and kv.second.value) {
        auto adjDb = fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
            kv.second.value.value(), serializer);
        nodeLabels[adjDb.thisNodeName] = adjDb.nodeLabel;
        if (adjDb.nodeLabel == 0) {
          nodeLabels.erase(adjDb.thisNodeName);
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
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    linkMonitors[i]->stop();
    linkMonitorThreads[i]->join();
  }
}

/**
 * Unit-test to test advertisement of static and loopback prefixes
 * - verify initial prefix-db is set to static prefixes
 * - add addresses via addrEvent and verify from KvStore prefix-db
 * - remove address via addrEvent and verify from KvStore prefix-db
 * - announce network instead of address via addrEvent and verify it doesn't
 *   change anything
 * - set link to down state and verify that it removes all associated addresses
 */
TEST_F(LinkMonitorTestFixture, StaticLoopbackPrefixAdvertisement) {
  // Verify that initial DB has static prefix entries
  std::unordered_set<thrift::IpPrefix> prefixes;
  prefixes.clear();
  while (prefixes.size() != 2) {
    LOG(INFO) << "Testing initial prefix database";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 2) {
      LOG(INFO) << "Looking for 2 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
  }

  //
  // Advertise some dummy and wrong prefixes
  //

  // push some invalid loopback addresses
  mockNlHandler->sendLinkEvent("loopback", 101, true);
  mockNlHandler->sendAddrEvent("loopback", "fe80::1/128", true);
  mockNlHandler->sendAddrEvent("loopback", "fe80::2/64", true);

  // push some valid loopback addresses
  mockNlHandler->sendAddrEvent("loopback", "10.127.240.1/32", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/128", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:cafe:babe::1/128", true);

  // push some valid interface addresses with subnet
  mockNlHandler->sendAddrEvent("loopback", "10.128.241.1/24", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/64", true);

  // verify
  prefixes.clear();
  while (prefixes.size() != 7) {
    LOG(INFO) << "Testing address advertisements";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 7) {
      LOG(INFO) << "Looking for 7 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::1/128")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:cafe:babe::1/128")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("10.127.240.1/32")));

    EXPECT_EQ(1, prefixes.count(toIpPrefix("10.128.241.0/24")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::/64")));
  }

  //
  // Withdraw prefix and see it is being withdrawn
  //

  // withdraw some addresses
  mockNlHandler->sendAddrEvent("loopback", "10.127.240.1/32", false);
  mockNlHandler->sendAddrEvent("loopback", "2803:cafe:babe::1/128", false);

  // withdraw addresses with subnet
  mockNlHandler->sendAddrEvent("loopback", "10.128.241.1/24", false);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/64", false);

  // verify
  prefixes.clear();
  while (prefixes.size() != 3) {
    LOG(INFO) << "Testing address withdraws";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 3) {
      LOG(INFO) << "Looking for 3 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::1/128")));
  }

  //
  // Send link down event
  //

  // send link down
  mockNlHandler->sendLinkEvent("loopback", 101, false);

  // verify
  prefixes.clear();
  while (prefixes.size() != 2) {
    LOG(INFO) << "Testing prefix withdraws";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 2) {
      LOG(INFO) << "Looking for 2 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
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
