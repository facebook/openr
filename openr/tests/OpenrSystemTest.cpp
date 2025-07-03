/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/Constants.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/tests/OpenrWrapper.h>
#include <openr/tests/mocks/MockIoProvider.h>

using namespace std;
using namespace openr;

using apache::thrift::CompactSerializer;

namespace fb303 = facebook::fb303;

namespace {

const std::chrono::seconds kMaxOpenrSyncTime(3);

const std::chrono::milliseconds kSpark2HelloTime(100);
const std::chrono::milliseconds kSpark2FastInitHelloTime(20);
const std::chrono::milliseconds kSpark2HandshakeTime(20);
const std::chrono::milliseconds kSpark2HeartbeatTime(20);
const std::chrono::milliseconds kSpark2HandshakeHoldTime(200);
const std::chrono::milliseconds kSpark2HeartbeatHoldTime(500);
const std::chrono::milliseconds kSpark2GRHoldTime(1000);
const std::chrono::milliseconds kLinkFlapInitialBackoff(1);
const std::chrono::milliseconds kLinkFlapMaxBackoff(8);

const string iface12{"1/2"};
const string iface13{"1/3"};
const string iface14{"1/4"};
const string iface21{"2/1"};
const string iface23{"2/3"};
const string iface24{"2/4"};
const string iface31{"3/1"};
const string iface32{"3/2"};
const string iface34{"3/4"};
const string iface41{"4/1"};
const string iface42{"4/2"};
const string iface43{"4/3"};

const int ifIndex12{12};
const int ifIndex21{21};

const folly::CIDRNetwork ip1V4(folly::IPAddress("192.168.0.1"), 32);
const folly::CIDRNetwork ip2V4(folly::IPAddress("192.168.0.2"), 32);
const folly::CIDRNetwork ip3V4(folly::IPAddress("192.168.0.3"), 32);
const folly::CIDRNetwork ip4V4(folly::IPAddress("192.168.0.4"), 32);

const folly::CIDRNetwork ip1V6(folly::IPAddress("fe80::1"), 128);
const folly::CIDRNetwork ip2V6(folly::IPAddress("fe80::2"), 128);
const folly::CIDRNetwork ip3V6(folly::IPAddress("fe80::3"), 128);
const folly::CIDRNetwork ip4V6(folly::IPAddress("fe80::4"), 128);

// R1 -> R2, R3, R4
const auto adj12 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 1, 0);
const auto adj13 =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 1, 0);
const auto adj14 =
    createAdjacency("4", "1/4", "4/1", "fe80::4", "192.168.0.4", 1, 0);
// R2 -> R1, R3, R4
const auto adj21 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 1, 0);
const auto adj23 =
    createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 1, 0);
const auto adj24 =
    createAdjacency("4", "2/4", "4/2", "fe80::4", "192.168.0.4", 1, 0);
// R3 -> R1, R2, R4
const auto adj31 =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 1, 0);
const auto adj32 =
    createAdjacency("2", "3/2", "2/3", "fe80::2", "192.168.0.2", 1, 0);
const auto adj34 =
    createAdjacency("4", "3/4", "4/3", "fe80::4", "192.168.0.4", 1, 0);
// R4 -> R1, R2, R3
const auto adj41 =
    createAdjacency("1", "4/1", "1/4", "fe80::1", "192.168.0.1", 1, 0);
const auto adj42 =
    createAdjacency("2", "4/2", "2/4", "fe80::2", "192.168.0.2", 1, 0);
const auto adj43 =
    createAdjacency("3", "4/3", "3/4", "fe80::3", "192.168.0.3", 1, 0);

using NextHop = pair<string /* ifname */, folly::IPAddress /* nexthop ip */>;
// Note: use unordered_set bcoz paths in a route can be in arbitrary order
using NextHopsWithMetric =
    unordered_set<pair<NextHop /* nexthop */, int32_t /* path metric */>>;
using RouteMap = unordered_map<
    pair<string /* node name */, string /* ip prefix */>,
    NextHopsWithMetric>;

// disable V4 by default
NextHop
toNextHop(thrift::Adjacency adj, bool isV4 = false) {
  return {
      *adj.ifName(), toIPAddress(isV4 ? *adj.nextHopV4() : *adj.nextHopV6())};
}

// Note: routeMap will be modified
void
fillRouteMap(
    const string& node,
    RouteMap& routeMap,
    const thrift::RouteDatabase& routeDb) {
  for (auto const& route : *routeDb.unicastRoutes()) {
    auto prefix = toString(*route.dest());
    for (const auto& nextHop : *route.nextHops()) {
      const auto nextHopAddr = toIPAddress(*nextHop.address());
      assert(nextHop.address()->ifName());
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << nextHop.address()->ifName().value() << " : " << nextHopAddr
              << " (" << *nextHop.metric() << ")";

      routeMap[make_pair(node, prefix)].insert(
          {{nextHop.address()->ifName().value(), nextHopAddr},
           *nextHop.metric()});
    }
  }
}

} // namespace

/**
 * Fixture for abstracting out common functionality for test
 */
class OpenrFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider = std::make_shared<MockIoProvider>();

    // start mock IoProvider thread
    mockIoProviderThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting mockIoProvider thread.";
      mockIoProvider->start();
      LOG(INFO) << "mockIoProvider thread got stopped.";
    });
    mockIoProvider->waitUntilRunning();
  }

  void
  TearDown() override {
    // clean up common resources
    LOG(INFO) << "Stopping mockIoProvider thread.";
    mockIoProvider->stop();
    mockIoProviderThread->join();

    // DO NOT explicitly call stop() method for Open/R instances
    // as DESCTRUCTOR in OpenrWrapper will take care of them.
  }

  /**
   * Helper function to create OpenrWrapper
   */
  OpenrWrapper<CompactSerializer>*
  createOpenr(
      std::string nodeId,
      bool v4Enabled,
      uint32_t memLimit = openr::memLimitMB) {
    auto ptr = std::make_unique<OpenrWrapper<CompactSerializer>>(
        nodeId,
        v4Enabled,
        kSpark2HelloTime,
        kSpark2FastInitHelloTime,
        kSpark2HandshakeTime,
        kSpark2HeartbeatTime,
        kSpark2HandshakeHoldTime,
        kSpark2HeartbeatHoldTime,
        kSpark2GRHoldTime,
        kLinkFlapInitialBackoff,
        kLinkFlapMaxBackoff,
        mockIoProvider,
        memLimit);
    openrWrappers_.emplace_back(std::move(ptr));
    return openrWrappers_.back().get();
  }

  // public member variables
  std::shared_ptr<MockIoProvider> mockIoProvider{nullptr};
  std::unique_ptr<std::thread> mockIoProviderThread{nullptr};

 private:
  std::vector<std::unique_ptr<OpenrWrapper<CompactSerializer>>>
      openrWrappers_{};
};

/*
 * This test creates one single standalone Open/R instance to verify Open/R
 * initialization will finish within certain threshold time instead of endless
 * waiting. Test will verify Open/R reach INITIALIZED state ultimately.
 */
TEST_F(OpenrFixture, InitializationWithStandaloneNode) {
  fb303::fbData->resetAllData();

  auto openr = createOpenr("initialization", false /* enable_v4 */);
  openr->run();
  LOG(INFO) << "Successfully started standalone Open/R instance for testing";

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::seconds(20), [&]() {
    // build initialization key for counter check
    auto initializedEvent = static_cast<thrift::InitializationEvent>(
        int(openr::thrift::InitializationEvent::INITIALIZED));
    auto counterKey = fmt::format(
        Constants::kInitEventCounterFormat,
        apache::thrift::util::enumNameSafe(initializedEvent));
    EXPECT_TRUE(fb303::fbData->hasCounter(counterKey));
    EXPECT_GE(fb303::fbData->getCounter(counterKey), 0);

    // prevent endless running of evb
    evb.stop();
  });

  // let evb fly
  evb.run();
}

//
// Test topology:
//
//  1------2
//  |      |
//  |      |
//  3------4
//
// Test on v4 for now
//
class SimpleRingTopologyFixture : public OpenrFixture,
                                  public ::testing::WithParamInterface<bool> {};

INSTANTIATE_TEST_CASE_P(
    SimpleRingTopologyInstance, SimpleRingTopologyFixture, ::testing::Bool());

//
// Verify system metrics
//
TEST_P(SimpleRingTopologyFixture, RersouceMonitor) {
  // define interface names for the test
  mockIoProvider->addIfNameIfIndex(
      {{iface12, ifIndex12}, {iface21, ifIndex21}});
  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface12, {{iface21, 100}}},
      {iface21, {{iface12, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  bool v4Enabled(GetParam());
  v4Enabled = false;

  std::string memKey{"process.memory.rss"};
  std::string cpuKey{"process.cpu.pct"};
  std::string cpuPeakKey{"process.cpu.peak_pct"};
  std::string upTimeKey{"process.uptime.seconds"};
  uint32_t rssMemInUse{0};

  // find out rss memory in use
  {
    auto openr2 = createOpenr("2", v4Enabled);
    openr2->run();

    auto counters2 = openr2->getCounters();
    /* sleep override */
    std::this_thread::sleep_for(kMaxOpenrSyncTime);
    while (counters2.size() == 0) {
      counters2 = openr2->getCounters();
    }
    rssMemInUse = counters2[memKey] / 1e6;
  }

  uint32_t memLimitMB = static_cast<uint32_t>(rssMemInUse) + 500;
  auto openr1 = createOpenr("1", v4Enabled, memLimitMB);
  openr1->run();

  /* sleep override */
  // wait until all aquamen got synced on kvstore
  std::this_thread::sleep_for(kMaxOpenrSyncTime);

  // Wait for calling getCPUpercentage() twice for calculating the cpu% counter.
  // Check if counters contain the uptime, cpu and memory usage counters.
  auto counters1 = openr1->getCounters();
  while (true) {
    if (counters1.find(cpuKey) != counters1.end()) {
      EXPECT_EQ(counters1.contains(cpuKey), 1);
      EXPECT_EQ(counters1.contains(cpuPeakKey), 1);
      EXPECT_EQ(counters1.contains(memKey), 1);
      EXPECT_EQ(counters1.contains(upTimeKey), 1);
      break;
    }
    counters1 = openr1->getCounters();
    std::this_thread::yield();
  }
  // allocate memory to go beyond memory limit and check if watchdog
  // catches the over the limit condition
  uint32_t memUsage = static_cast<uint32_t>(counters1[memKey] / 1e6);

  if (memUsage < memLimitMB) {
    EXPECT_FALSE(openr1->watchdog->memoryLimitExceeded());
    uint32_t allocMem = memLimitMB - memUsage + 10;

    LOG(INFO) << "Allocating:" << allocMem << ", Mem in use:" << memUsage
              << ", Memory limit:" << memLimitMB << "MB";
    vector<int8_t> v((allocMem) * 0x100000);
    fill(v.begin(), v.end(), 1);
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(5));
    EXPECT_TRUE(openr1->watchdog->memoryLimitExceeded());
  } else {
    // memory already recached above the limit
    EXPECT_TRUE(openr1->watchdog->memoryLimitExceeded());
  }
}

int
main(int argc, char** argv) {
  // parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
