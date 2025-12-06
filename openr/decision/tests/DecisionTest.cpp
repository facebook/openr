/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdio>

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Optional.h>
#include <folly/Random.h>
#include <folly/futures/Promise.h>
#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/Flags.h>
#include <openr/common/MplsUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/Decision.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/decision/tests/Consts.h>
#include <openr/decision/tests/DecisionTestUtils.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/tests/utils/Utils.h>

using namespace std;
using namespace openr;
using namespace testing;

namespace fb303 = facebook::fb303;

using apache::thrift::CompactSerializer;

namespace {

const auto addr1Cidr = toIPNetwork(addr1);
const auto addr2Cidr = toIPNetwork(addr2);
const auto addr2V4Cidr = toIPNetwork(addr2V4);

const auto addr1V4ConfigPrefixEntry =
    createPrefixEntry(addr1, thrift::PrefixType::CONFIG);
const auto addr2VipPrefixEntry =
    createPrefixEntry(addr1, thrift::PrefixType::VIP);

// timeout to wait until decision debounce
// (i.e. spf recalculation, route rebuild) finished
const std::chrono::milliseconds debounceTimeoutMin{10};
const std::chrono::milliseconds debounceTimeoutMax{250};

// Empty Perf Events
const thrift::AdjacencyDatabase kEmptyAdjDb;
const apache::thrift::optional_field_ref<thrift::PerfEvents const&>
    kEmptyPerfEventRef{kEmptyAdjDb.perfEvents()};
} // anonymous namespace

//
// Start the decision thread and simulate KvStore communications
// Expect proper RouteDatabase publications to appear
//
class DecisionTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    // Reset all global counters
    fb303::fbData->resetAllData();

    auto tConfig = createConfig();
    config = std::make_shared<Config>(tConfig);

    decision = make_shared<Decision>(
        config,
        peerUpdatesQueue.getReader(),
        kvStoreUpdatesQueue.getReader(),
        staticRouteUpdatesQueue.getReader(),
        routeUpdatesQueue);

    decisionThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Decision thread starting";
      decision->run();
      LOG(INFO) << "Decision thread finishing";
    });
    decision->waitUntilRunning();

    // Reset initial KvStore sync event as not sent.
    kvStoreSyncEventSent = false;
    adjacencyDbSyncEventSent = false;

    // Override default rib policy file with file based on thread id.
    // This ensures stress run will use different file for each run.
    FLAGS_rib_policy_file = fmt::format(
        "/dev/shm/rib_policy.txt.{}",
        std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // Publish initial peers.
    publishInitialPeers();
  }

  void
  TearDown() override {
    peerUpdatesQueue.close();
    kvStoreUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
    routeUpdatesQueue.close();

    // Delete default rib policy file.
    remove(FLAGS_rib_policy_file.c_str());

    LOG(INFO) << "Stopping the decision thread";
    decision->stop();
    decisionThread->join();
    LOG(INFO) << "Decision thread got stopped";
  }

  virtual openr::thrift::OpenrConfig
  createConfig() {
    auto tConfig = getBasicOpenrConfig(
        "1",
        {},
        true /* enable v4 */,
        true /* dryrun */,
        false /* enableV4OverV6Nexthop */);

    // timeout to wait until decision debounce
    // (i.e. spf recalculation, route rebuild) finished
    tConfig.decision_config()->debounce_min_ms() = debounceTimeoutMin.count();
    tConfig.decision_config()->debounce_max_ms() = debounceTimeoutMax.count();
    tConfig.enable_best_route_selection() = true;
    tConfig.decision_config()->save_rib_policy_min_ms() = 500;
    tConfig.decision_config()->save_rib_policy_max_ms() = 2000;
    // Set a shorter timeout so we don't have to wait as long.
    tConfig.decision_config()->unblock_initial_routes_ms() = 1000;
    return tConfig;
  }

  virtual void
  publishInitialPeers() {
    thrift::PeersMap peers;
    peers.emplace("2", thrift::PeerSpec());
    PeerEvent peerEvent{
        {kTestingAreaName, AreaPeerEvent(peers, {} /*peersToDel*/)}};
    peerUpdatesQueue.push(std::move(peerEvent));
  }

  //
  // member methods
  //

  void
  verifyReceivedRoutes(const folly::CIDRNetwork& network, bool isRemoved) {
    auto startTime = std::chrono::steady_clock::now();
    while (true) {
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > debounceTimeoutMax) {
        ASSERT_TRUE(0) << fmt::format(
            "Timeout verifying prefix: {} in prefix-state. Time limit: {}",
            folly::IPAddress::networkToString(network),
            debounceTimeoutMax.count());
      }

      // Expect best route selection to be populated in route-details for addr2
      thrift::ReceivedRouteFilter filter;
      filter.prefixes() = std::vector<thrift::IpPrefix>({toIpPrefix(network)});
      auto routes = decision->getReceivedRoutesFiltered(filter).get();
      if ((!isRemoved) && routes->size()) {
        return;
      }
      if (isRemoved && routes->empty()) {
        return;
      }
      // yield CPU
      std::this_thread::yield();
    }
  }

  std::unordered_map<std::string, thrift::RouteDatabase>
  dumpRouteDb(const vector<string>& allNodes) {
    std::unordered_map<std::string, thrift::RouteDatabase> routeMap;

    for (string const& node : allNodes) {
      auto resp = decision->getDecisionRouteDb(node).get();
      EXPECT_TRUE(resp);
      EXPECT_EQ(node, *resp->thisNodeName());

      // Sort next-hop lists to ease verification code
      for (auto& route : *resp->unicastRoutes()) {
        std::sort(route.nextHops()->begin(), route.nextHops()->end());
      }
      for (auto& route : *resp->mplsRoutes()) {
        std::sort(route.nextHops()->begin(), route.nextHops()->end());
      }

      routeMap[node] = std::move(*resp);
    }

    return routeMap;
  }

  DecisionRouteUpdate
  recvRouteUpdates() {
    auto maybeRouteDb = routeUpdatesQueueReader.get();
    EXPECT_FALSE(maybeRouteDb.hasError());
    auto routeDbDelta = maybeRouteDb.value();
    return routeDbDelta;
  }

  // publish routeDb
  void
  sendKvPublication(
      const thrift::Publication& tPublication,
      bool prefixPubExists = true,
      bool sendKvStoreSyncSignal = true,
      bool sendAdjacencyDbSyncSignal = true) {
    kvStoreUpdatesQueue.push(tPublication);
    if (prefixPubExists) {
      if (sendKvStoreSyncSignal && !kvStoreSyncEventSent) {
        // Send KvStore initial synced event.
        kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);
        kvStoreSyncEventSent = true;
      }

      if (sendAdjacencyDbSyncSignal && !adjacencyDbSyncEventSent) {
        // Send Self Adjacencies synced event.
        kvStoreUpdatesQueue.push(
            thrift::InitializationEvent::ADJACENCY_DB_SYNCED);
        adjacencyDbSyncEventSent = true;
      }
    }
  }

  void
  sendStaticRoutesUpdate(const thrift::RouteDatabaseDelta& publication) {
    DecisionRouteUpdate routeUpdate;
    for (const auto& unicastRoute : *publication.unicastRoutesToUpdate()) {
      auto nhs = std::unordered_set<thrift::NextHopThrift>(
          unicastRoute.nextHops()->begin(), unicastRoute.nextHops()->end());
      routeUpdate.addRouteToUpdate(
          RibUnicastEntry(toIPNetwork(*unicastRoute.dest()), std::move(nhs)));
    }
    for (const auto& prefix : *publication.unicastRoutesToDelete()) {
      routeUpdate.unicastRoutesToDelete.push_back(toIPNetwork(prefix));
    }
    for (const auto& mplsRoute : *publication.mplsRoutesToUpdate()) {
      auto nhs = std::unordered_set<thrift::NextHopThrift>(
          mplsRoute.nextHops()->begin(), mplsRoute.nextHops()->end());
      routeUpdate.addMplsRouteToUpdate(
          RibMplsEntry(*mplsRoute.topLabel(), std::move(nhs)));
    }
    for (const auto& label : *publication.mplsRoutesToDelete()) {
      routeUpdate.mplsRoutesToDelete.push_back(label);
    }
    staticRouteUpdatesQueue.push(routeUpdate);
  }

  thrift::Value
  createPrefixValue(
      const string& node,
      int64_t version,
      thrift::PrefixDatabase const& prefixDb) {
    return createThriftValue(
        version,
        node,
        writeThriftObjStr(prefixDb, serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  thrift::Value
  createPrefixValue(
      const string& node,
      int64_t version,
      const vector<thrift::IpPrefix>& prefixes = {},
      const string& area = kTestingAreaName) {
    vector<thrift::PrefixEntry> prefixEntries;
    for (const auto& prefix : prefixes) {
      prefixEntries.emplace_back(createPrefixEntry(prefix));
    }
    return createPrefixValue(
        node, version, createPrefixDb(node, prefixEntries));
  }

  /**
   * Check whether two DecisionRouteUpdates to be equal
   */
  bool
  checkEqualRoutesDelta(
      DecisionRouteUpdate& lhsC, thrift::RouteDatabaseDelta& rhs) {
    auto lhs = lhsC.toThrift();
    std::sort(
        lhs.unicastRoutesToUpdate()->begin(),
        lhs.unicastRoutesToUpdate()->end());
    std::sort(
        rhs.unicastRoutesToUpdate()->begin(),
        rhs.unicastRoutesToUpdate()->end());

    std::sort(
        lhs.unicastRoutesToDelete()->begin(),
        lhs.unicastRoutesToDelete()->end());
    std::sort(
        rhs.unicastRoutesToDelete()->begin(),
        rhs.unicastRoutesToDelete()->end());

    return *lhs.unicastRoutesToUpdate() == *rhs.unicastRoutesToUpdate() &&
        *lhs.unicastRoutesToDelete() == *rhs.unicastRoutesToDelete();
  }

  //
  // member variables
  //

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  CompactSerializer serializer{};

  std::shared_ptr<Config> config;
  messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueueReader{
      routeUpdatesQueue.getReader()};

  // Decision owned by this wrapper.
  std::shared_ptr<Decision> decision{nullptr};

  // Thread in which decision will be running.
  std::unique_ptr<std::thread> decisionThread{nullptr};

  // Initial KvStore synced signal is sent.
  bool kvStoreSyncEventSent{false};
  bool adjacencyDbSyncEventSent{false};
};

TEST_F(DecisionTestFixture, StopDecisionWithoutInitialPeers) {
  // Close all queues.
  routeUpdatesQueue.close();
  kvStoreUpdatesQueue.close();
  staticRouteUpdatesQueue.close();
  // Initial peers are not received yet.
  peerUpdatesQueue.close();

  // decision module could stop.
  decision->stop();
}

TEST_F(DecisionTestFixture, DecisionUndrainStateTest) {
  auto publication = createThriftPublication(
      {
          {"adj:1",
           createAdjValue(
               serializer,
               "1", /* node name */
               1, /* version */
               {adj12}, /* adjacencies */
               false /* node overloaded flag */,
               1 /* nodeId*/)},
          {"adj:2",
           createAdjValue(
               serializer,
               "2", /* node name */
               1, /* version */
               {adj21}, /* adjacencies */
               false /* node overloaded flag */,
               2 /* nodeId*/)},
          createPrefixKeyValue("1", 1, addr1),
          createPrefixKeyValue("2", 1, addr2),
      },
      {} /* expired keys */,
      {} /* nodeIds*/,
      {} /* keysToUpdate */);
  sendKvPublication(publication);
  // NOTE: this is just to make sure pulication is processed by decision
  // before checking drain state inside LSDB
  recvRouteUpdates();

  /*
   * Test 1: specify the node name. Expect API to return specified node's
   *         drain state.
   */
  {
    auto state = decision->getDecisionDrainState("1").get();
    EXPECT_EQ(thrift::DrainState::UNDRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_FALSE(state->drained_interfaces());
  }
  {
    auto state = decision->getDecisionDrainState("2").get();
    EXPECT_EQ(thrift::DrainState::UNDRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_FALSE(state->drained_interfaces());
  }

  /*
   * Test 2: do not specify the node name. Expect API to return local node's
   *         drain state.
   */
  auto state = decision->getDecisionDrainState().get();
  EXPECT_EQ(thrift::DrainState::UNDRAINED, *state->drain_state());
  ASSERT_FALSE(state->soft_drained_interfaces());
  ASSERT_FALSE(state->drained_interfaces());
}

TEST_F(DecisionTestFixture, DecisionHardDrainStateTest) {
  // deliberately copy and set the link hard-drained
  auto adj21_1 = adj21;
  adj21_1.isOverloaded() = true;

  auto publication = createThriftPublication(
      {
          {"adj:1",
           createAdjValue(
               serializer,
               "1", /* node name */
               1, /* version */
               {adj12}, /* adjacencies */
               true /* node overloaded flag */,
               1 /* nodeId*/)},
          {"adj:2",
           createAdjValue(
               serializer,
               "2", /* node name */
               1, /* version */
               {adj21_1}, /* adjacencies */
               false /* node overloaded flag */,
               2 /* nodeId*/)},
          createPrefixKeyValue("1", 1, addr1),
          createPrefixKeyValue("2", 1, addr2),
      },
      {} /* expired keys */,
      {} /* nodeIds*/,
      {} /* keysToUpdate */);
  sendKvPublication(publication);
  // NOTE: this is just to make sure pulication is processed by decision
  // before checking drain state inside LSDB
  recvRouteUpdates();

  /*
   * Test 1: specify the node name. Expect API to return specified node's
   *         drain state.
   */
  {
    auto state = decision->getDecisionDrainState("1").get();
    EXPECT_EQ(thrift::DrainState::HARD_DRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_FALSE(state->drained_interfaces());
  }
  {
    auto state = decision->getDecisionDrainState("2").get();
    EXPECT_EQ(thrift::DrainState::UNDRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_TRUE(state->drained_interfaces());
    EXPECT_THAT(*state->drained_interfaces(), testing::SizeIs(1));
    EXPECT_EQ(state->drained_interfaces()->back(), *adj21_1.ifName());
  }

  /*
   * Test 2: do not specify the node name. Expect API to return local node's
   *         drain state.
   */
  {
    auto state = decision->getDecisionDrainState().get();
    EXPECT_EQ(thrift::DrainState::HARD_DRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_FALSE(state->drained_interfaces());
  }

  /*
   * Test 3: specify an unknown node name. Expect API to return the default
   * value.
   */
  {
    auto state = decision->getDecisionDrainState("unknown").get();
    EXPECT_EQ(thrift::DrainState::UNDRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_FALSE(state->drained_interfaces());
  }
}

TEST_F(DecisionTestFixture, DecisionSoftDrainStateTest) {
  auto publication = createThriftPublication(
      {
          {"adj:1",
           createAdjValue(
               serializer,
               "1", /* node name */
               1, /* version */
               {adj12}, /* adjacencies */
               false, /* overloaded flag */
               1, /* nodeId*/
               100 /* nodeMetricIncrement */)},
          {"adj:2",
           createAdjValue(
               serializer,
               "2", /* node name */
               1, /* version */
               {adj21}, /* adjacencies */
               false, /* overloaded flag */
               2 /* nodeId*/)},
          createPrefixKeyValue("1", 1, addr1),
          createPrefixKeyValue("2", 1, addr2),
      },
      {} /* expired keys */,
      {} /* nodeIds*/,
      {} /* keysToUpdate */);
  sendKvPublication(publication);
  // NOTE: this is just to make sure pulication is processed by decision
  // before checking drain state inside LSDB
  recvRouteUpdates();

  /*
   * Test 1: specify the node name. Expect API to return specified node's
   *         drain state.
   */
  {
    auto state = decision->getDecisionDrainState("1").get();
    EXPECT_EQ(thrift::DrainState::SOFT_DRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_FALSE(state->drained_interfaces());
  }
  {
    auto state = decision->getDecisionDrainState("2").get();
    EXPECT_EQ(thrift::DrainState::UNDRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_FALSE(state->drained_interfaces());
  }

  /*
   * Test 2: do not specify the node name. Expect API to return local node's
   *         drain state.
   */
  {
    auto state = decision->getDecisionDrainState().get();
    EXPECT_EQ(thrift::DrainState::SOFT_DRAINED, *state->drain_state());
    ASSERT_FALSE(state->soft_drained_interfaces());
    ASSERT_FALSE(state->drained_interfaces());
  }
}

// The following topology is used:
//
// 1---2---3
//
// We upload the link 1---2 with the initial sync and later publish
// the 2---3 link information. We then request the full routing dump
// from the decision process via respective socket.

TEST_F(DecisionTestFixture, BasicOperations) {
  //
  // publish the link state info to KvStore
  //

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());

  auto routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes()->begin(), routeDb.unicastRoutes()->end());

  auto routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));

  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));
  //
  // publish the link state info to KvStore via the KvStore pub socket
  // we simulate adding a new router R3
  //

  // Some tricks here; we need to bump the time-stamp on router 2's data, so
  // it can override existing; for router 3 we publish new key-value

  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue(serializer, "2", 3, {adj21, adj23}, false, 2)},
       {"adj:4",
        createAdjValue(serializer, "4", 1, {}, false, 4)}, // No adjacencies
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {});
  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes()->begin(),
      routeDbBefore.unicastRoutes()->end());
  sendKvPublication(publication);
  // validate routers

  // receive my local Decision routeDbDelta publication
  routeDbDelta = recvRouteUpdates();
  // only expect to add a route to addr3
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      routeDbDelta.unicastRoutesToUpdate.begin()->second.prefix,
      toIPNetwork(addr3));
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());

  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes()->begin(), routeDb.unicastRoutes()->end());
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  fillRouteMap("1", routeMap, routeDb);
  // 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj12, false, 20)}));

  // dump other nodes' routeDB
  auto routeDbMap = dumpRouteDb({"2", "3"});
  EXPECT_EQ(2, routeDbMap["2"].unicastRoutes()->size());
  EXPECT_EQ(2, routeDbMap["3"].unicastRoutes()->size());
  for (auto& [key, value] : routeDbMap) {
    fillRouteMap(key, routeMap, value);
  }

  // 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));

  // 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj32, false, 20)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));

  // remove 3
  publication = createThriftPublication(
      thrift::KeyVals{},
      {"adj:3", "prefix:3", "adj:4"} /* expired keys */,
      {},
      {});

  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes()->begin(),
      routeDbBefore.unicastRoutes()->end());

  sendKvPublication(publication);
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToDelete.size());
  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes()->begin(), routeDb.unicastRoutes()->end());

  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue(serializer, "2", 4, {adj21, adj23}, false, 2)},
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {});
  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes()->begin(),
      routeDbBefore.unicastRoutes()->end());
  sendKvPublication(publication);
  // validate routers

  // receive my local Decision routeDbDelta publication
  routeDbDelta = recvRouteUpdates();
  // only expect to add a route to addr3
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      routeDbDelta.unicastRoutesToUpdate.begin()->second.prefix,
      toIPNetwork(addr3));

  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes()->begin(), routeDb.unicastRoutes()->end());
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
}

/**
 * Publish all types of update to Decision and expect that Decision emits
 * a full route database that includes all the routes as its first update.
 *
 * Types of information updated
 * - Adjacencies (with MPLS labels)
 * - Prefixes
 */
TEST_F(DecisionTestFixture, InitialRouteUpdate) {
  // Send adj publication
  sendKvPublication(
      createThriftPublication(
          {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
           {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)}},
          {},
          {},
          {}),
      false /*prefixPubExists*/);

  // Send prefix publication
  sendKvPublication(createThriftPublication(
      {createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {}));

  // Receive & verify all the expected updates
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());
}

/**
 * Publish all types of update to Decision with only ADJACENCY_DB_SYNCED
 * Omit sending KVSTORE_SYNCED
 * Verify that route database build is blocked
 */
TEST_F(DecisionTestFixture, InitialRouteBuildBlockedForKvStore) {
  // Send adj publication
  sendKvPublication(
      createThriftPublication(
          {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
           {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)}},
          {},
          {},
          {}),
      false /*prefixPubExists*/,
      false /*sendKvStoreSyncSignla*/,
      true /*sendAdjacencyDbSyncSignal*/);

  // Send prefix publication
  sendKvPublication(
      createThriftPublication(
          {createPrefixKeyValue("1", 1, addr1),
           createPrefixKeyValue("2", 1, addr2)},
          {},
          {},
          {}),
      true /*prefixPubExists*/,
      false /*sendKvStoreSyncSignal*/,
      true /*sendAdjacencyDbSyncSignal*/);

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds{100}, [&]() {
    // Route update is never queued because adjacency is missing.
    EXPECT_EQ(0, routeUpdatesQueueReader.size());
    evb.stop();
  });
  evb.run();

  kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);
  kvStoreSyncEventSent = true;

  // Receive & verify all the expected updates
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());
}

/**
 * Publish all types of update to Decision with only KVSTORE_SYNCED
 * Omit sending ADJACENCY_DB_SYNCED
 * Verify that route database build is blocked
 */
TEST_F(DecisionTestFixture, InitialRouteBuildBlockedForAdjacencyDb) {
  // Send adj publication
  sendKvPublication(
      createThriftPublication(
          {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
           {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)}},
          {},
          {},
          {}),
      false /*prefixPubExists*/,
      true /*sendKvStoreSyncSignla*/,
      false /*sendAdjacencyDbSyncSignal*/);

  // Send prefix publication
  sendKvPublication(
      createThriftPublication(
          {createPrefixKeyValue("1", 1, addr1),
           createPrefixKeyValue("2", 1, addr2)},
          {},
          {},
          {}),
      true /*prefixPubExists*/,
      true /*sendKvStoreSyncSignal*/,
      false /*sendAdjacencyDbSyncSignal*/);

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds{100}, [&]() {
    // Route update is never queued because adjacency is missing.
    EXPECT_EQ(0, routeUpdatesQueueReader.size());
    evb.stop();
  });
  evb.run();

  kvStoreUpdatesQueue.push(thrift::InitializationEvent::ADJACENCY_DB_SYNCED);
  adjacencyDbSyncEventSent = true;

  // Receive & verify all the expected updates
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());
}

/**
 * Verify basic decision operation when flag enabled to not wait for
 * adjacency DB sync signal
 */
class DecisionSkipSelfAdjSyncTestFixture : public DecisionTestFixture {
  openr::thrift::OpenrConfig
  createConfig() override {
    auto tConfig = DecisionTestFixture::createConfig();
    // Do not wait for adjacency DB sync signal to proceed for building routes
    tConfig.enable_init_optimization() = false;

    return tConfig;
  }
};

/**
 * Publish all types of update to Decision with only KVSTORE_SYNCED
 * Omit sending ADJACENCY_DB_SYNCED
 * Verify that route database build is not blocked when config is
 * set to build routes on kvstore sync
 */
TEST_F(
    DecisionSkipSelfAdjSyncTestFixture,
    InitialRouteBuildNotBlockedForAdjacencyDb) {
  // Send adj publication
  sendKvPublication(
      createThriftPublication(
          {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
           {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)}},
          {},
          {},
          {}),
      false /*prefixPubExists*/,
      true /*sendKvStoreSyncSignla*/,
      false /*sendAdjacencyDbSyncSignal*/);

  // Send prefix publication
  sendKvPublication(
      createThriftPublication(
          {createPrefixKeyValue("1", 1, addr1),
           createPrefixKeyValue("2", 1, addr2)},
          {},
          {},
          {}),
      true /*prefixPubExists*/,
      true /*sendKvStoreSyncSignal*/,
      false /*sendAdjacencyDbSyncSignal*/);

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds{100}, [&]() {
    // Route update is never queued because adjacency is missing.
    EXPECT_EQ(1, routeUpdatesQueueReader.size());
    evb.stop();
  });
  evb.run();

  // Receive & verify all the expected updates
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());
}

TEST_F(DecisionTestFixture, MissingBidirectionalAdjacency) {
  auto publication = createThriftPublication(
      {// Include adjacency 1->2 but not 2-> 1. This will cause bidirectional
       // adjacency check to fail.
       {"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {} /* expired keys */);
  sendKvPublication(publication);

  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds{100}, [&]() {
    // Route update is never queued because adjacency is missing.
    EXPECT_EQ(0, routeUpdatesQueueReader.size());
    evb.stop();
  });
  evb.run();
}

TEST_F(DecisionTestFixture, UnblockInitialRoutesTimeout) {
  // Publish adjacency 1->2 but not 2-> 1. This will cause bidirectional
  // adjacency check to fail.
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {} /* expired keys */);
  sendKvPublication(publication);
  // If we wait long enough, the timeout will expire and route computation will
  // be unblocked.
  recvRouteUpdates();
  facebook::fb303::fbData->flushAllData();
  // For some reason, we have to call getCounters first or else hasCounter will
  // return false.
  facebook::fb303::fbData->getCounters();
  EXPECT_TRUE(
      facebook::fb303::fbData->hasCounter(
          "initialization.RIB_COMPUTED.duration_ms"));
}

/*
 * Route Origination Test:
 *  - Test 1:
 *    - static prefixes advertised from `PrefixManager`
 *    - expect `routesToUpdate` contains prefixes advertised;
 *  - Test 2:
 *    - advertise SAME prefix from `Decision`(i.e. prefix update in KvStore)
 *    - expect `routesToUpdate` contains prefixes BUT NHs overridden
 *      by `decision`;
 *  - Test 3:
 *    - withdraw static prefixes from `PrefixManager`
 *    - expect `routesToUpdate` contains prefixes BUT NHs overridden
 *      by `deicision`;
 *  - Test 4:
 *    - re-advertise static prefixes from `PrefixManager`
 *    - expect `routesToUpdate` contains prefixes BUT NHs overridden
 *      by `deicision`;
 *  - Test 5:
 *    - withdraw prefixes from `Decision`(i.e. prefix deleted in KvStore)
 *    - expect `routesToUpdate` contains static prefixes from `PrefixManager`
 *  - Test 6:
 *    - withdraw static prefixes from `PrefixManager`
 *    - expect `routesToDelete` contains static prefixes
 *  - Test7: Received self-advertised prefix publication from KvStore.
 *    - No routes will be generated by SpfSolver for self originated prefixes.
 *    - Since there are no existing routes for the prefix, no delete routes
 *      will be generated.
 */
TEST_F(DecisionTestFixture, RouteOrigination) {
  // eventbase to control the pace of tests
  OpenrEventBase evb;

  // prepare prefix/nexthops structure
  const std::string prefixV4 = "10.0.0.1/24";
  const std::string prefixV6 = "fe80::1/64";

  thrift::NextHopThrift nhV4, nhV6;
  nhV4.address() = toBinaryAddress(Constants::kLocalRouteNexthopV4.toString());
  nhV6.address() = toBinaryAddress(Constants::kLocalRouteNexthopV6.toString());

  const auto networkV4 = folly::IPAddress::createNetwork(prefixV4);
  const auto networkV6 = folly::IPAddress::createNetwork(prefixV6);
  auto routeV4 = createUnicastRoute(toIpPrefix(prefixV4), {nhV4});
  auto routeV6 = createUnicastRoute(toIpPrefix(prefixV6), {nhV6});

  // Send adj publication
  // ATTN: to trigger `buildRouteDb()`. Must provide LinkState
  //      info containing self-node id("1")
  auto scheduleAt = std::chrono::milliseconds{0};
  evb.scheduleTimeout(scheduleAt, [&]() noexcept {
    sendKvPublication(createThriftPublication(
        {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
         {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)}},
        {},
        {},
        {}));
  });

  //
  // Test1: advertise prefixes from `PrefixManager`
  //
  evb.scheduleTimeout(scheduleAt += 3 * debounceTimeoutMax, [&]() noexcept {
    auto routeDbDelta = recvRouteUpdates();

    LOG(INFO) << "Advertising static prefixes from PrefixManager";

    thrift::RouteDatabaseDelta routeDb;
    routeDb.unicastRoutesToUpdate()->emplace_back(routeV4);
    routeDb.unicastRoutesToUpdate()->emplace_back(routeV6);
    sendStaticRoutesUpdate(std::move(routeDb));
  });

  // wait for debouncer to fire
  evb.scheduleTimeout(
      scheduleAt += (debounceTimeoutMax + std::chrono::milliseconds(100)),
      [&]() noexcept {
        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        EXPECT_THAT(
            routeToUpdate.at(networkV4), testing::Truly([&networkV4](auto i) {
              return i.prefix == networkV4 && i.doNotInstall == false;
            }));
        EXPECT_THAT(
            routeToUpdate.at(networkV6), testing::Truly([&networkV6](auto i) {
              return i.prefix == networkV6 && i.doNotInstall == false;
            }));
        // NOTE: no SAME route from decision, program DROP route
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            testing::UnorderedElementsAre(nhV4));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            testing::UnorderedElementsAre(nhV6));
      });

  //
  // Test2: advertise SAME prefixes from `Decision`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Advertising SAME prefixes from Decision";

        sendKvPublication(createThriftPublication(
            {createPrefixKeyValue("2", 1, toIpPrefix(prefixV4)),
             createPrefixKeyValue("2", 1, toIpPrefix(prefixV6))},
            {},
            {},
            {}));

        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        // NOTE: route from decision takes higher priority
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            Not(testing::UnorderedElementsAre(nhV4)));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            Not(testing::UnorderedElementsAre(nhV6)));
      });

  //
  // Test3: withdraw prefixes from `PrefixManager`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Withdrawing static prefixes from PrefixManager";

        thrift::RouteDatabaseDelta routeDb;
        routeDb.unicastRoutesToDelete()->emplace_back(toIpPrefix(networkV4));
        routeDb.unicastRoutesToDelete()->emplace_back(toIpPrefix(networkV6));
        sendStaticRoutesUpdate(std::move(routeDb));
      });

  // wait for debouncer to fire
  evb.scheduleTimeout(
      scheduleAt += (debounceTimeoutMax + std::chrono::milliseconds(100)),
      [&]() noexcept {
        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        // NOTE: route from Decision is the ONLY output
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            Not(testing::UnorderedElementsAre(nhV4)));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            Not(testing::UnorderedElementsAre(nhV6)));
      });

  //
  // Test4: re-advertise prefixes from `PrefixManager`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Re-advertising static prefixes from PrefixManager";

        thrift::RouteDatabaseDelta routeDb;
        routeDb.unicastRoutesToUpdate()->emplace_back(routeV4);
        routeDb.unicastRoutesToUpdate()->emplace_back(routeV6);
        sendStaticRoutesUpdate(std::move(routeDb));
      });

  // wait for debouncer to fire
  evb.scheduleTimeout(
      scheduleAt += (debounceTimeoutMax + std::chrono::milliseconds(100)),
      [&]() noexcept {
        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        // NOTE: route from decision takes higher priority
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            Not(testing::UnorderedElementsAre(nhV4)));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            Not(testing::UnorderedElementsAre(nhV6)));
      });

  //
  // Test5: withdraw prefixes from `Decision`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Withdrawing prefixes from Decision";

        sendKvPublication(createThriftPublication(
            {createPrefixKeyValue(
                 "2", 1, toIpPrefix(prefixV4), kTestingAreaName, true),
             createPrefixKeyValue(
                 "2", 1, toIpPrefix(prefixV6), kTestingAreaName, true)},
            {},
            {},
            {}));

        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        // NOTE: no routes from decision. Program DROP routes.
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            testing::UnorderedElementsAre(nhV4));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            testing::UnorderedElementsAre(nhV6));
      });

  //
  // Test6: withdraw prefixes from `PrefixManager`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Withdrawing prefixes from PrefixManager";

        thrift::RouteDatabaseDelta routeDb;
        routeDb.unicastRoutesToDelete()->emplace_back(toIpPrefix(networkV4));
        routeDb.unicastRoutesToDelete()->emplace_back(toIpPrefix(networkV6));
        sendStaticRoutesUpdate(std::move(routeDb));
      });

  // wait for debouncer to fire
  evb.scheduleTimeout(
      scheduleAt += (debounceTimeoutMax + std::chrono::milliseconds(100)),
      [&]() noexcept {
        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(0));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(2));

        EXPECT_THAT(
            routeDbDelta.unicastRoutesToDelete,
            testing::UnorderedElementsAre(networkV4, networkV6));
      });

  //
  // Test7: Received self-advertised publication from KvStore. No routes will be
  // generated.
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        sendKvPublication(createThriftPublication(
            {createPrefixKeyValue(
                "1", 1, toIpPrefix(prefixV4), kTestingAreaName)},
            {},
            {},
            {}));
        // No unicast routes are generated.
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(0));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        evb.stop();
      });

  // magic happens
  evb.run();
}

// The following topology is used:
//  1--- A ---2
//  |         |
//  B         A
//  |         |
//  3--- B ---4
//
// area A: adj12, adj24
// area B: adj13, adj34
TEST_F(DecisionTestFixture, MultiAreaBestPathCalculation) {
  //
  // publish area A adj and prefix
  // "1" originate addr1 into A
  // "2" originate addr2 into A
  //
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21, adj24}, false, 2)},
       {"adj:4", createAdjValue(serializer, "4", 1, {adj42}, false, 4)},
       createPrefixKeyValue("1", 1, addr1, kTestingAreaName),
       createPrefixKeyValue("2", 1, addr2, kTestingAreaName)},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      kTestingAreaName);

  sendKvPublication(publication);
  recvRouteUpdates();

  //
  // publish area B adj and prefix
  // "3" originate addr3 into B
  // "4" originate addr4 into B
  //
  publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj13}, false, 1)},
       {"adj:3", createAdjValue(serializer, "3", 1, {adj31, adj34}, false, 3)},
       {"adj:4", createAdjValue(serializer, "4", 1, {adj43}, false, 4)},
       createPrefixKeyValue("3", 1, addr3, "B"),
       createPrefixKeyValue("4", 1, addr4, "B")},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  auto routeDb1 = dumpRouteDb({"1"})["1"];
  auto routeDb2 = dumpRouteDb({"2"})["2"];
  auto routeDb3 = dumpRouteDb({"3"})["3"];
  auto routeDb4 = dumpRouteDb({"4"})["4"];

  // routeDb1 from node "1"
  {
    auto routeToAddr2 = createUnicastRoute(
        addr2,
        {createNextHopFromAdj(
            adj12, false, 10, std::nullopt, kTestingAreaName)});
    auto routeToAddr3 = createUnicastRoute(
        addr3, {createNextHopFromAdj(adj13, false, 10, std::nullopt, "B")});
    // addr4 is only originated in area B
    auto routeToAddr4 = createUnicastRoute(
        addr4, {createNextHopFromAdj(adj13, false, 20, std::nullopt, "B")});
    EXPECT_THAT(*routeDb1.unicastRoutes(), testing::SizeIs(3));
    EXPECT_THAT(
        *routeDb1.unicastRoutes(),
        testing::UnorderedElementsAre(
            routeToAddr2, routeToAddr3, routeToAddr4));
  }

  // routeDb2 from node "2" will only see addr1 in area A
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1,
        {createNextHopFromAdj(
            adj21, false, 10, std::nullopt, kTestingAreaName)});
    EXPECT_THAT(*routeDb2.unicastRoutes(), testing::SizeIs(1));
    EXPECT_THAT(
        *routeDb2.unicastRoutes(), testing::UnorderedElementsAre(routeToAddr1));
  }

  // routeDb3 will only see addr4 in area B
  {
    auto routeToAddr4 = createUnicastRoute(
        addr4, {createNextHopFromAdj(adj34, false, 10, std::nullopt, "B")});
    EXPECT_THAT(*routeDb3.unicastRoutes(), testing::SizeIs(1));
    EXPECT_THAT(
        *routeDb3.unicastRoutes(), testing::UnorderedElementsAre(routeToAddr4));
  }

  // routeDb4
  {
    auto routeToAddr2 = createUnicastRoute(
        addr2,
        {createNextHopFromAdj(
            adj42, false, 10, std::nullopt, kTestingAreaName)});
    auto routeToAddr3 = createUnicastRoute(
        addr3, {createNextHopFromAdj(adj43, false, 10, std::nullopt, "B")});
    // addr1 is only originated in area A
    auto routeToAddr1 = createUnicastRoute(
        addr1,
        {createNextHopFromAdj(
            adj42, false, 20, std::nullopt, kTestingAreaName)});
    EXPECT_THAT(*routeDb4.unicastRoutes(), testing::SizeIs(3));
    EXPECT_THAT(
        *routeDb4.unicastRoutes(),
        testing::UnorderedElementsAre(
            routeToAddr2, routeToAddr3, routeToAddr1));
  }

  //
  // "1" originate addr1 into B
  //
  publication = createThriftPublication(
      {createPrefixKeyValue("1", 1, addr1, "B")},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  routeDb3 = dumpRouteDb({"3"})["3"];
  routeDb4 = dumpRouteDb({"4"})["4"];

  // routeMap3 now should see addr1 in areaB
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1, {createNextHopFromAdj(adj31, false, 10, std::nullopt, "B")});
    EXPECT_THAT(*routeDb3.unicastRoutes(), testing::Contains(routeToAddr1));
  }

  // routeMap4 now could reach addr1 through areaA or areaB
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1,
        {createNextHopFromAdj(adj43, false, 20, std::nullopt, "B"),
         createNextHopFromAdj(
             adj42, false, 20, std::nullopt, kTestingAreaName)});
    EXPECT_THAT(*routeDb4.unicastRoutes(), testing::Contains(routeToAddr1));
  }
}

// MultiArea Tology topology is used:
//  1--- A ---2
//  |
//  B
//  |
//  3
//
// area A: adj12
// area B: adj13
TEST_F(DecisionTestFixture, SelfRedistributePrefixPublication) {
  //
  // publish area A adj and prefix
  // "2" originate addr2 into A
  //
  auto originKeyStr =
      PrefixKey("2", toIPNetwork(addr2), kTestingAreaName).getPrefixKeyV2();
  auto originPfx = createPrefixEntry(addr2);
  originPfx.area_stack() = {"65000"};
  auto originPfxVal =
      createPrefixValue("2", 1, createPrefixDb("2", {originPfx}));

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       {originKeyStr, originPfxVal}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      kTestingAreaName);

  sendKvPublication(publication);
  recvRouteUpdates();

  //
  // publish area B adj and prefix
  //
  publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj13}, false, 1)},
       {"adj:3", createAdjValue(serializer, "3", 1, {adj31}, false, 3)}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  //
  // "1" reditribute addr2 into B
  //   - this should not cause prefix db update
  //   - not route update
  //
  auto redistributeKeyStr =
      PrefixKey("1", toIPNetwork(addr2), "B").getPrefixKeyV2();
  auto redistributePfx = createPrefixEntry(addr2, thrift::PrefixType::RIB);
  redistributePfx.area_stack() = {"65000", kTestingAreaName};
  auto redistributePfxVal =
      createPrefixValue("1", 1, createPrefixDb("1", {redistributePfx}, "B"));

  publication = createThriftPublication(
      {{redistributeKeyStr, redistributePfxVal}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      "B");
  sendKvPublication(publication);

  // wait for publication to be processed
  /* sleep override */
  std::this_thread::sleep_for(
      debounceTimeoutMax + std::chrono::milliseconds(100));

  EXPECT_EQ(0, routeUpdatesQueueReader.size());
}

/**
 * Exhaustively RibPolicy feature in Decision. The intention here is to
 * verify the functionality of RibPolicy in Decision module. RibPolicy
 * is also unit-tested for it's complete correctness and we don't aim
 * it here.
 *
 * Test covers
 * - Get policy without setting (exception case)
 * - Set policy
 * - Get policy after setting
 * - Verify that set-policy triggers the route database change (apply policy)
 * - Set the policy with 0 weight. See that route dis-appears
 * - Expire policy. Verify it triggers the route database change (undo policy)
 */
TEST_F(DecisionTestFixture, RibPolicy) {
  // Setup topology and prefixes. 1 unicast route will be computed
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  sendKvPublication(publication);

  // Expect route update. Verify next-hop weight to be 0 (ECMP)
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    EXPECT_EQ(
        0,
        *updates.unicastRoutesToUpdate.begin()
             ->second.nexthops.begin()
             ->weight());
  }

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight()->emplace("2", 2);
  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher()->prefixes() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action()->set_weight() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements()->emplace_back(policyStatement);
  policy.ttl_secs() = 1;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  {
    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
    EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());
  }

  // Expect the route database change with next-hop weight to be 2
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    EXPECT_EQ(
        2,
        *updates.unicastRoutesToUpdate.begin()
             ->second.nexthops.begin()
             ->weight());
  }

  // Set the policy with empty weight. Expect route remains intact and error
  // counter is reported
  policy.statements()->at(0).action()->set_weight()->neighbor_to_weight()["2"] =
      0;
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());
  {
    auto updates = recvRouteUpdates();
    EXPECT_EQ(1, updates.unicastRoutesToUpdate.size());
    ASSERT_EQ(0, updates.unicastRoutesToDelete.size());
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.count(toIPNetwork(addr2)));
    for (auto& nh :
         updates.unicastRoutesToUpdate.at(toIPNetwork(addr2)).nexthops) {
      EXPECT_EQ(0, *nh.weight());
    }
    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("decision.rib_policy.invalidated_routes.count"));
  }

  // trigger addr2 recalc by flapping the advertisement
  publication = createThriftPublication(
      {createPrefixKeyValue(
          "2", 2, addr2, kTestingAreaName, true /* withdraw */)},
      {},
      {},
      {});
  sendKvPublication(publication);
  publication = createThriftPublication(
      {createPrefixKeyValue("2", 3, addr2)}, {}, {}, {});
  sendKvPublication(publication);

  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    ASSERT_EQ(0, updates.unicastRoutesToDelete.size());
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.count(toIPNetwork(addr2)));
    for (auto& nh :
         updates.unicastRoutesToUpdate.at(toIPNetwork(addr2)).nexthops) {
      EXPECT_EQ(0, *nh.weight());
    }
    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(2, counters.at("decision.rib_policy.invalidated_routes.count"));
  }

  // Let the policy expire. Wait for another route database change
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(0, updates.unicastRoutesToUpdate.size());

    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_GE(0, *retrievedPolicy.ttl_secs());
  }
}

/**
 * Verifies that error is set if RibPolicy is invalid
 */
TEST_F(DecisionTestFixture, RibPolicyError) {
  // Set empty rib policy
  auto sf = decision->setRibPolicy(thrift::RibPolicy{});

  // Expect an error to be set immediately (validation happens inline)
  EXPECT_TRUE(sf.isReady());
  EXPECT_TRUE(sf.hasException());
  EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
}

/**
 * Verifies that a policy gets cleared
 */
TEST_F(DecisionTestFixture, RibPolicyClear) {
  // Setup topology and prefixes. 1 unicast route will be computed
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {});
  sendKvPublication(publication);

  // Expect route update.
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    EXPECT_EQ(
        0,
        *updates.unicastRoutesToUpdate.begin()
             ->second.nexthops.begin()
             ->weight());
  }

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight()->emplace("2", 2);
  actionWeight.neighbor_to_weight()->emplace("1", 1);

  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher()->prefixes() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action()->set_weight() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements()->emplace_back(policyStatement);
  policy.ttl_secs() = 1;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  {
    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
    EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());
  }

  // Expect route update. Verify next-hop weight to be 2 (ECMP)
  auto updates = recvRouteUpdates();
  ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      2,
      *updates.unicastRoutesToUpdate.begin()
           ->second.nexthops.begin()
           ->weight());

  // Clear rib policy and expect nexthop weight change
  EXPECT_NO_THROW(decision->clearRibPolicy());

  updates = recvRouteUpdates();
  ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      0,
      *updates.unicastRoutesToUpdate.begin()
           ->second.nexthops.begin()
           ->weight());

  // Verify that get rib policy throws no exception
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);
}

/**
 * Verifies that set/get APIs throws exception if RibPolicy feature is not
 * enabled.
 */
class DecisionNoRibPolicyTestFixture : public DecisionTestFixture {
  openr::thrift::OpenrConfig
  createConfig() override {
    auto tConfig = DecisionTestFixture::createConfig();
    // Disable rib_policy feature
    tConfig.enable_rib_policy() = false;

    return tConfig;
  }
};

TEST_F(DecisionNoRibPolicyTestFixture, RibPolicyFeatureKnob) {
  ASSERT_FALSE(config->isRibPolicyEnabled());

  // dummy event to unblock decision module from initialization
  PeerEvent event;
  peerUpdatesQueue.push(std::move(event));

  // SET
  {
    // Create valid rib policy
    thrift::RibRouteActionWeight actionWeight;
    actionWeight.neighbor_to_weight()->emplace("2", 2);
    thrift::RibPolicyStatement policyStatement;
    policyStatement.matcher()->prefixes() =
        std::vector<thrift::IpPrefix>({addr2});
    policyStatement.action()->set_weight() = actionWeight;
    thrift::RibPolicy policy;
    policy.statements()->emplace_back(policyStatement);
    policy.ttl_secs() = 1;

    auto sf = decision->setRibPolicy(policy);
    EXPECT_TRUE(sf.isReady());
    EXPECT_TRUE(sf.hasException());
    EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
  }

  // GET
  {
    auto sf = decision->getRibPolicy();
    EXPECT_TRUE(sf.isReady());
    EXPECT_TRUE(sf.hasException());
    EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
  }
}

/**
 * Test graceful restart support of Rib policy in Decision.
 *
 * Test covers
 * - Set policy
 * - Get policy after setting
 * - Wait longer than debounce time so Decision have saved Rib policy
 * - Create a new Decision instance to load the still live Rib policy
 * - Setup initial topology and prefixes to trigger route computation
 * - Verify that loaded Rib policy is applied on generated routes
 */
TEST_F(DecisionTestFixture, GracefulRestartSupportForRibPolicy) {
  auto saveRibPolicyMaxMs =
      *config->getConfig().decision_config()->save_rib_policy_max_ms();

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight()->emplace("2", 2);
  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher()->prefixes() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action()->set_weight() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements()->emplace_back(policyStatement);
  // Set policy ttl as long as 10*saveRibPolicyMaxMs.
  policy.ttl_secs() = saveRibPolicyMaxMs * 10 / 1000;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  {
    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
    EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());
  }

  std::unique_ptr<Decision> decision{nullptr};
  std::unique_ptr<std::thread> decisionThread{nullptr};
  int scheduleAt{0};

  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += saveRibPolicyMaxMs),
      [&]() noexcept {
        // Wait for saveRibPolicyMaxMs to make sure Rib policy is saved to file.
        messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
        messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;
        messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
        messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
        auto routeUpdatesQueueReader = routeUpdatesQueue.getReader();
        decision = std::make_unique<Decision>(
            config,
            peerUpdatesQueue.getReader(),
            kvStoreUpdatesQueue.getReader(),
            staticRouteUpdatesQueue.getReader(),
            routeUpdatesQueue);
        decisionThread =
            std::make_unique<std::thread>([&]() { decision->run(); });
        decision->waitUntilRunning();

        // Publish initial batch of detected peers.
        thrift::PeersMap peers;
        peers.emplace("2", thrift::PeerSpec());
        PeerEvent peerEvent{
            {kTestingAreaName, AreaPeerEvent(peers, {} /*peersToDel*/)}};
        peerUpdatesQueue.push(std::move(peerEvent));

        // Setup topology and prefixes. 1 unicast route will be computed
        auto publication = createThriftPublication(
            {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
             {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
             {"prefix:1", createPrefixValue("1", 1, {addr1})},
             {"prefix:2", createPrefixValue("2", 1, {addr2})}},
            {},
            {},
            {},
            kTestingAreaName);
        kvStoreUpdatesQueue.push(publication);
        kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);
        kvStoreUpdatesQueue.push(
            thrift::InitializationEvent::ADJACENCY_DB_SYNCED);

        // Expect route update with live rib policy applied.
        auto maybeRouteDb = routeUpdatesQueueReader.get();
        EXPECT_FALSE(maybeRouteDb.hasError());
        auto updates = maybeRouteDb.value();
        ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
        EXPECT_EQ(
            2,
            *updates.unicastRoutesToUpdate.begin()
                 ->second.nexthops.begin()
                 ->weight());

        // Get rib policy and verify
        auto retrievedPolicy = decision->getRibPolicy().get();
        EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
        EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());

        kvStoreUpdatesQueue.close();
        staticRouteUpdatesQueue.close();
        routeUpdatesQueue.close();
        peerUpdatesQueue.close();

        evb.stop();
      });

  // let magic happen
  evb.run();
  decision->stop();
  decisionThread->join();
}

/**
 * Test Decision ignores expired rib policy.
 *
 * Test covers
 * - Set policy
 * - Get policy after setting
 * - Wait long enough so Decision have saved Rib policy and policy expired
 * - Create a new Decision instance which will skip loading expired Rib policy
 * - Setup initial topology and prefixes to trigger route computation
 * - Verify that expired Rib policy is not applied on generated routes
 */
TEST_F(DecisionTestFixture, SaveReadStaleRibPolicy) {
  auto saveRibPolicyMaxMs =
      *config->getConfig().decision_config()->save_rib_policy_max_ms();

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight()->emplace("2", 2);
  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher()->prefixes() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action()->set_weight() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements()->emplace_back(policyStatement);
  policy.ttl_secs() = saveRibPolicyMaxMs / 1000;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  auto retrievedPolicy = decision->getRibPolicy().get();
  EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
  EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());

  std::unique_ptr<Decision> decision{nullptr};
  std::unique_ptr<std::thread> decisionThread{nullptr};

  int scheduleAt{0};
  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 2 * saveRibPolicyMaxMs), [&]() {
        // Wait for 2 * saveRibPolicyMaxMs.
        // This makes sure expired rib policy is saved to file.
        messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
        messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;
        messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
        messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
        auto routeUpdatesQueueReader = routeUpdatesQueue.getReader();
        decision = std::make_unique<Decision>(
            config,
            peerUpdatesQueue.getReader(),
            kvStoreUpdatesQueue.getReader(),
            staticRouteUpdatesQueue.getReader(),
            routeUpdatesQueue);
        decisionThread = std::make_unique<std::thread>([&]() {
          LOG(INFO) << "Decision thread starting";
          decision->run();
          LOG(INFO) << "Decision thread finishing";
        });
        decision->waitUntilRunning();

        // Publish initial batch of detected peers.
        thrift::PeersMap peers;
        peers.emplace("2", thrift::PeerSpec());
        PeerEvent peerEvent{
            {kTestingAreaName, AreaPeerEvent(peers, {} /*peersToDel*/)}};
        peerUpdatesQueue.push(std::move(peerEvent));

        // Setup topology and prefixes. 1 unicast route will be computed
        auto publication = createThriftPublication(
            {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
             {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
             {"prefix:1", createPrefixValue("1", 1, {addr1})},
             {"prefix:2", createPrefixValue("2", 1, {addr2})}},
            {},
            {},
            {});
        kvStoreUpdatesQueue.push(publication);
        kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);
        kvStoreUpdatesQueue.push(
            thrift::InitializationEvent::ADJACENCY_DB_SYNCED);

        // Expect route update without rib policy applied.
        auto maybeRouteDb = routeUpdatesQueueReader.get();
        EXPECT_FALSE(maybeRouteDb.hasError());
        auto updates = maybeRouteDb.value();
        ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
        EXPECT_EQ(
            0,
            *updates.unicastRoutesToUpdate.begin()
                 ->second.nexthops.begin()
                 ->weight());

        // Expired rib policy was not loaded.
        EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

        kvStoreUpdatesQueue.close();
        staticRouteUpdatesQueue.close();
        routeUpdatesQueue.close();
        peerUpdatesQueue.close();

        evb.stop();
      });

  // let magic happen
  evb.run();
  decision->stop();
  decisionThread->join();
}

// The following topology is used:
//
//         100
//  1--- ---------- 2
//   \_           _/
//      \_ ____ _/
//          800

// We upload parallel link 1---2 with the initial sync and later bring down
// the one with lower metric. We then verify updated route database is
// received
//

TEST_F(DecisionTestFixture, ParallelLinks) {
  auto parallelAdj12_1 =
      createAdjacency("2", "1/2-1", "2/1-1", "fe80::2", "192.168.0.2", 100, 0);
  auto parallelAdj12_2 =
      createAdjacency("2", "1/2-2", "2/1-2", "fe80::2", "192.168.0.2", 800, 0);
  auto parallelAdj21_1 =
      createAdjacency("1", "2/1-1", "1/2-1", "fe80::1", "192.168.0.1", 100, 0);
  auto parallelAdj21_2 =
      createAdjacency("1", "2/1-2", "1/2-2", "fe80::1", "192.168.0.1", 800, 0);

  auto publication = createThriftPublication(
      {{"adj:1",
        createAdjValue(serializer, "1", 1, {parallelAdj12_1, parallelAdj12_2})},
       {"adj:2",
        createAdjValue(serializer, "2", 1, {parallelAdj21_1, parallelAdj21_2})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  auto routeDb = dumpRouteDb({"1"})["1"];
  auto routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(parallelAdj12_1, false, 100)}));

  publication = createThriftPublication(
      {{"adj:2", createAdjValue(serializer, "2", 2, {parallelAdj21_2})}},
      {},
      {},
      {});

  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(parallelAdj12_2, false, 800)}));

  // restore the original state
  publication = createThriftPublication(
      {{"adj:2",
        createAdjValue(
            serializer, "2", 2, {parallelAdj21_1, parallelAdj21_2})}},
      {},
      {},
      {});
  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(parallelAdj12_1, false, 100)}));

  // overload the least cost link
  auto parallelAdj21_1_overloaded = parallelAdj21_1;
  parallelAdj21_1_overloaded.isOverloaded() = true;

  publication = createThriftPublication(
      {{"adj:2",
        createAdjValue(
            serializer,
            "2",
            2,
            {parallelAdj21_1_overloaded, parallelAdj21_2})}},
      {},
      {},
      {});
  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(parallelAdj12_2, false, 800)}));
}

// The following topology is used:
//
// 1---2---3---4
//
// We upload the link 1---2 with the initial sync and later publish
// the 2---3 & 3---4 link information. We expect it to trigger SPF only once.
//
TEST_F(DecisionTestFixture, PubDebouncing) {
  //
  // publish the link state info to KvStore
  //

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12})},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);
  EXPECT_EQ(0, counters["decision.route_build_runs.count"]);

  sendKvPublication(publication);
  recvRouteUpdates();

  // validate SPF after initial sync, no rebouncing here
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters["decision.spf_runs.count"]);
  EXPECT_EQ(1, counters["decision.route_build_runs.count"]);

  //
  // publish the link state info to KvStore via the KvStore pub socket
  // we simulate adding a new router R3
  //

  // Some tricks here; we need to bump the time-stamp on router 2's data, so
  // it can override existing; for router 3 we publish new key-value
  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32})},
       {"adj:2", createAdjValue(serializer, "2", 3, {adj21, adj23})},
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {});
  sendKvPublication(publication);

  // we simulate adding a new router R4

  // Some tricks here; we need to bump the time-stamp on router 3's data, so
  // it can override existing;

  publication = createThriftPublication(
      {{"adj:4", createAdjValue(serializer, "4", 1, {adj43})},
       {"adj:3", createAdjValue(serializer, "3", 5, {adj32, adj34})}},
      {},
      {},
      {});
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters["decision.spf_runs.count"]);
  EXPECT_EQ(2, counters["decision.route_build_runs.count"]);

  //
  // Only publish prefix updates
  //
  auto getRouteForPrefixCount =
      counters.at("decision.get_route_for_prefix.count");
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 1, addr4)}, {}, {}, {});
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters["decision.spf_runs.count"]);
  // only prefix changed no full rebuild needed
  EXPECT_EQ(2, counters["decision.route_build_runs.count"]);

  EXPECT_EQ(
      getRouteForPrefixCount + 1,
      counters["decision.get_route_for_prefix.count"]);

  //
  // publish adj updates right after prefix updates
  // Decision is supposed to only trigger spf recalculation

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 2, addr4),
       createPrefixKeyValue("4", 2, addr5)},
      {},
      {},
      {});
  sendKvPublication(publication);

  publication = createThriftPublication(
      {{"adj:2", createAdjValue(serializer, "2", 5, {adj21})}}, {}, {}, {});
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(3, counters["decision.spf_runs.count"]);
  EXPECT_EQ(3, counters["decision.route_build_runs.count"]);

  //
  // publish multiple prefix updates in a row
  // Decision is supposed to process prefix update only once

  // Some tricks here; we need to bump the version on router 4's data, so
  // it can override existing;

  getRouteForPrefixCount = counters.at("decision.get_route_for_prefix.count");
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 5, addr4)}, {}, {}, {});
  sendKvPublication(publication);

  publication = createThriftPublication(
      {createPrefixKeyValue("4", 7, addr4),
       createPrefixKeyValue("4", 7, addr6)},
      {},
      {},
      {});
  sendKvPublication(publication);

  publication = createThriftPublication(
      {createPrefixKeyValue("4", 8, addr4),
       createPrefixKeyValue("4", 8, addr5),
       createPrefixKeyValue("4", 8, addr6)},
      {},
      {},
      {});
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  // only prefix has changed so spf_runs is unchanged
  EXPECT_EQ(3, counters["decision.spf_runs.count"]);
  // addr6 is seen to have been advertised in this  interval
  EXPECT_EQ(
      getRouteForPrefixCount + 1,
      counters["decision.get_route_for_prefix.count"]);
}

//
// Send unrelated key-value pairs to Decision
// Make sure they do not trigger SPF runs, but rather ignored
//
TEST_F(DecisionTestFixture, NoSpfOnIrrelevantPublication) {
  //
  // publish the link state info to KvStore, but use different markers
  // those must be ignored by the decision module
  //
  auto publication = createThriftPublication(
      {{"adj2:1", createAdjValue(serializer, "1", 1, {adj12})},
       {"adji2:2", createAdjValue(serializer, "2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

  // make sure the counter did not increment
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);
}

//
// Send duplicate key-value pairs to Decision
// Make sure subsquent duplicates are ignored.
//
TEST_F(DecisionTestFixture, NoSpfOnDuplicatePublication) {
  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  auto const publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12})},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

  // make sure counter is incremented
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters["decision.spf_runs.count"]);

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

  // make sure counter is not incremented
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters["decision.spf_runs.count"]);
}

/**
 * Test to verify route calculation when a prefix is advertised from more than
 * one node.
 *
 *
 *  node4(p4)
 *     |
 *   5 |
 *     |         10
 *  node1(p1) --------- node2(p2)
 *     |
 *     | 10
 *     |
 *  node3(p2)
 */
TEST_F(DecisionTestFixture, DuplicatePrefixes) {
  // Note: local copy overwriting global ones, to be changed in this test
  auto localAdj14 =
      createAdjacency("4", "1/4", "4/1", "fe80::4", "192.168.0.4", 5, 0);
  auto localAdj41 =
      createAdjacency("1", "4/1", "1/4", "fe80::1", "192.168.0.1", 5, 0);
  auto localAdj12 =
      createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 0);
  auto localAdj21 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 0);

  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  auto publication = createThriftPublication(
      {{"adj:1",
        createAdjValue(serializer, "1", 1, {localAdj14, localAdj12, adj13})},
       {"adj:2", createAdjValue(serializer, "2", 1, {localAdj21})},
       {"adj:3", createAdjValue(serializer, "3", 1, {adj31})},
       {"adj:4", createAdjValue(serializer, "4", 1, {localAdj41})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2),
       // node3 has same address w/ node2
       createPrefixKeyValue("3", 1, addr2),
       createPrefixKeyValue("4", 1, addr4)},
      {},
      {},
      {});

  sendKvPublication(publication);
  recvRouteUpdates();

  // Expect best route selection to be populated in route-details for addr2
  {
    thrift::ReceivedRouteFilter filter;
    filter.prefixes() = std::vector<thrift::IpPrefix>({addr2});
    auto routes = decision->getReceivedRoutesFiltered(filter).get();
    ASSERT_EQ(1, routes->size());

    auto const& routeDetails = routes->at(0);
    EXPECT_EQ(2, routeDetails.bestKeys()->size());
    EXPECT_EQ("2", routeDetails.bestKey()->node().value());
  }

  // Query new information
  // validate routers
  auto routeMapList = dumpRouteDb({"1", "2", "3", "4"});
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  RouteMap routeMap;
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
  }

  // 1
  EXPECT_EQ(2, routeMapList["1"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(localAdj12, false, 10),
           createNextHopFromAdj(adj13, false, 10)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(localAdj21, false, 10)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(localAdj41, false, 15)}));

  /**
   * Overload node-2 and node-4. Now we on node-1 will only route p2 toward
   * node-3 but will still have route p4 toward node-4 since it's unicast
   *
   *  node4(p4)
   *     |
   *   5 |
   *     |         10     (overloaded)
   *  node1(p1) --------- node2(p2)
   *     |
   *     | 10
   *     |
   *  node3(p2)
   */

  publication = createThriftPublication(
      {{"adj:2",
        createAdjValue(
            serializer, "2", 1, {localAdj21}, true /* overloaded */)},
       {"adj:4",
        createAdjValue(
            serializer, "4", 1, {localAdj41}, true /* overloaded */)}},
      {},
      {},
      {});

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvRouteUpdates();

  routeMapList = dumpRouteDb({"1"});
  RouteMap routeMap2;
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap2, value);
  }
  EXPECT_EQ(
      routeMap2[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));

  EXPECT_EQ(
      routeMap2[make_pair("1", toString(addr4))],
      NextHops({createNextHopFromAdj(localAdj14, false, 5)}));

  /**
   * Increase the distance between node-1 and node-2 to 100. Now we on node-1
   * will reflect weights into nexthops and FIB will not do multipath
   *
   *  node4(p4)
   *     |
   *   5 |
   *     |         100
   *  node1(p1) --------- node2(p2)
   *     |
   *     | 10
   *     |
   *  node3(p2)
   */
  localAdj12.metric() = 100;
  localAdj21.metric() = 100;

  publication = createThriftPublication(
      {{"adj:1",
        createAdjValue(serializer, "1", 2, {localAdj12, adj13, localAdj14})},
       {"adj:2", createAdjValue(serializer, "2", 2, {localAdj21, adj23})}},
      {},
      {},
      {});

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvRouteUpdates();

  // Query new information
  // validate routers
  routeMapList = dumpRouteDb({"1", "2", "3", "4"});
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  routeMap.clear();
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
  }

  // 1
  EXPECT_EQ(2, routeMapList["1"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(localAdj21, false, 100)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(localAdj41, false, 15)}));
}

/**
 * Tests reliability of Decision SUB socket. We overload SUB socket with lot
 * of messages and make sure none of them are lost. We make decision compute
 * routes for a large network topology taking good amount of CPU time. We
 * do not try to validate routes here instead we validate messages processed
 * by decision and message sent by us.
 *
 * Topology consists of 1000 nodes linear where node-i connects to 3 nodes
 * before it and 3 nodes after it.
 *
 */
TEST_F(DecisionTestFixture, DecisionSubReliability) {
  thrift::Publication initialPub;
  initialPub.area() = kTestingAreaName;

  std::string keyToDup;

  // Create full topology
  for (int i = 1; i <= 1000; i++) {
    const std::string src = folly::to<std::string>(i);

    // Create prefixDb value
    const auto addr = toIpPrefix(fmt::format("face:cafe:babe::{}/128", i));
    auto kv = createPrefixKeyValue(src, 1, addr);
    if (1 == i) {
      // arbitrarily choose the first key to send duplicate publications for
      keyToDup = kv.first;
    }
    initialPub.keyVals()->emplace(kv);

    // Create adjDb value
    vector<thrift::Adjacency> adjs;
    for (int j = std::max(1, i - 3); j <= std::min(1000, i + 3); j++) {
      if (i == j) {
        continue;
      }
      const std::string dst = folly::to<std::string>(j);
      auto adj = createAdjacency(
          dst,
          fmt::format("{}/{}", src, dst),
          fmt::format("{}/{}", dst, src),
          fmt::format("fe80::{}", dst),
          "192.168.0.1" /* unused */,
          10 /* metric */,
          0 /* adj label */);
      adjs.emplace_back(std::move(adj));
    }
    initialPub.keyVals()->emplace(
        fmt::format("adj:{}", src), createAdjValue(serializer, src, 1, adjs));
  }

  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  sendKvPublication(initialPub);

  //
  // Hammer Decision with lot of duplicate publication for 2 * ThrottleTimeout
  // We want to ensure that we hammer Decision for atleast once during it's
  // SPF run. This will cause lot of pending publications on Decision. This
  // is not going to cause any SPF computation
  //
  thrift::Publication duplicatePub;
  duplicatePub.area() = kTestingAreaName;
  duplicatePub.keyVals()[keyToDup] = initialPub.keyVals()->at(keyToDup);
  int64_t totalSent = 0;
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (diff > (2 * debounceTimeoutMax)) {
      LOG(INFO) << "Hammered decision with " << totalSent
                << " updates. Stopping";
      break;
    }
    ++totalSent;
    sendKvPublication(duplicatePub);
  }

  // Receive RouteUpdate from Decision
  auto routeUpdates1 = recvRouteUpdates();
  // Route to all nodes except mine.
  EXPECT_EQ(999, routeUpdates1.unicastRoutesToUpdate.size());

  //
  // Advertise prefix update. Decision gonna take some good amount of time to
  // process this last update (as it has many queued updates).
  //
  thrift::Publication newPub;
  newPub.area() = kTestingAreaName;

  auto newAddr = toIpPrefix("face:b00c:babe::1/128");
  newPub.keyVals() = {createPrefixKeyValue("1", 1, newAddr)};
  LOG(INFO) << "Advertising prefix update";
  sendKvPublication(newPub);
  // Receive RouteDelta from Decision
  auto routeUpdates2 = recvRouteUpdates();
  // Expect no routes delta
  EXPECT_EQ(0, routeUpdates2.unicastRoutesToUpdate.size());

  //
  // Verify counters information
  //

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters["decision.spf_runs.count"]);
}

//
// This test aims to verify counter reporting from Decision module
//
TEST_F(DecisionTestFixture, CountersTest) {
  // Verifiy some initial/default counters
  {
    decision->updateGlobalCounters();
    const auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(counters.at("decision.num_nodes"), 1);
  }

  // set up first publication

  // Node1 and Node2 has both v4/v6 loopbacks, Node3 has only V6
  auto bgpPrefixEntry1 = createPrefixEntry( // Missing loopback
      toIpPrefix("10.2.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.2.0.0/16",
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP);
  auto bgpPrefixEntry2 = createPrefixEntry( // Missing metric vector
      toIpPrefix("10.3.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.3.0.0/16",
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      std::nullopt /* missing metric vector */);
  thrift::KeyVals pubKvs = {
      {"adj:1", createAdjValue(serializer, "1", 1, {adj12, adj13}, false, 1)},
      {"adj:2", createAdjValue(serializer, "2", 1, {adj21, adj23}, false, 2)},
      {"adj:3", createAdjValue(serializer, "3", 1, {adj31}, false, 3)},
      // Disconnected node
      {"adj:4", createAdjValue(serializer, "4", 1, {}, false, 4)}};
  pubKvs.emplace(createPrefixKeyValue("1", 1, addr1));
  pubKvs.emplace(createPrefixKeyValue("1", 1, addr1V4));

  pubKvs.emplace(createPrefixKeyValue("2", 1, addr2));
  pubKvs.emplace(createPrefixKeyValue("2", 1, addr2V4));

  pubKvs.emplace(createPrefixKeyValue("3", 1, addr3));
  pubKvs.emplace(createPrefixKeyValue("3", 1, bgpPrefixEntry1));

  pubKvs.emplace(createPrefixKeyValue("4", 1, addr4));
  pubKvs.emplace(createPrefixKeyValue("4", 1, bgpPrefixEntry2));

  // Node1 connects to 2/3, Node2 connects to 1, Node3 connects to 1
  // Node2 has partial adjacency
  auto publication0 = createThriftPublication(pubKvs, {}, {}, {});
  sendKvPublication(publication0);
  const auto routeDb = recvRouteUpdates();
  for (const auto& [_, uniRoute] : routeDb.unicastRoutesToUpdate) {
    EXPECT_NE(
        folly::IPAddress::networkToString(uniRoute.prefix), "10.1.0.0/16");
  }

  // Verify counters
  decision->updateGlobalCounters();
  const auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.num_partial_adjacencies"), 1);
  EXPECT_EQ(counters.at("decision.num_complete_adjacencies"), 2);
  EXPECT_EQ(counters.at("decision.num_nodes"), 4);
  EXPECT_EQ(counters.at("decision.num_prefixes"), 8);
  EXPECT_EQ(counters.at("decision.no_route_to_prefix.count.60"), 2);

  // fully disconnect node 2
  auto publication1 = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 2, {adj13}, false, 1)}},
      {},
      {},
      {});
  sendKvPublication(publication1);
  // wait for update
  recvRouteUpdates();

  decision->updateGlobalCounters();
  EXPECT_EQ(
      fb303::fbData->getCounters().at("decision.num_partial_adjacencies"), 0);
}

TEST_F(DecisionTestFixture, ExceedMaxBackoff) {
  for (int i = debounceTimeoutMin.count(); true; i *= 2) {
    auto nodeName = std::to_string(i);
    auto publication = createThriftPublication(
        {createPrefixKeyValue(nodeName, 1, addr1)}, {}, {}, {});
    sendKvPublication(publication);
    if (i >= debounceTimeoutMax.count()) {
      break;
    }
  }

  // wait for debouncer to try to fire
  /* sleep override */
  std::this_thread::sleep_for(
      debounceTimeoutMax + std::chrono::milliseconds(100));
  // send one more update
  auto publication = createThriftPublication(
      {createPrefixKeyValue("2", 1, addr1)}, {}, {}, {});
  sendKvPublication(publication);
}

//
// Mixed type prefix announcements (e.g. prefix1 with type BGP and type RIB )
// are allowed when enableBestRouteSelection_ = true,
// Otherwise prefix will be skipped in route programming.
//
TEST_F(DecisionTestFixture, PrefixWithMixedTypeRoutes) {
  // Verifiy some initial/default counters
  {
    decision->updateGlobalCounters();
    const auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(counters.at("decision.num_nodes"), 1);
  }

  // set up first publication

  // node 2/3 announce loopbacks
  {
    const auto localPrefixDb2 = createPrefixDb(
        "2", {createPrefixEntry(addr2), createPrefixEntry(addr2V4)});
    const auto localPrefixDb3 = createPrefixDb(
        "3", {createPrefixEntry(addr3), createPrefixEntry(addr3V4)});

    // Node1 connects to 2/3, Node2 connects to 1, Node3 connects to 1
    auto publication = createThriftPublication(
        {{"adj:1",
          createAdjValue(serializer, "1", 1, {adj12, adj13}, false, 1)},
         {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
         {"adj:3", createAdjValue(serializer, "3", 1, {adj31}, false, 3)},
         createPrefixKeyValue("2", 1, addr2),
         createPrefixKeyValue("2", 1, addr2V4),
         createPrefixKeyValue("3", 1, addr3),
         createPrefixKeyValue("3", 1, addr3V4)},
        {},
        {},
        {});
    sendKvPublication(publication);
    recvRouteUpdates();
  }

  // Node2 annouce prefix in BGP type,
  // Node3 announce prefix in Rib type
  {
    auto bgpPrefixEntry = createPrefixEntry(
        toIpPrefix("10.1.0.0/16"),
        thrift::PrefixType::BGP,
        "data=10.1.0.0/16",
        thrift::PrefixForwardingType::IP,
        thrift::PrefixForwardingAlgorithm::SP_ECMP);
    auto ribPrefixEntry = createPrefixEntry(
        toIpPrefix("10.1.0.0/16"),
        thrift::PrefixType::RIB,
        "",
        thrift::PrefixForwardingType::IP,
        thrift::PrefixForwardingAlgorithm::SP_ECMP);

    auto publication = createThriftPublication(
        // node 2 announce BGP prefix with loopback
        {createPrefixKeyValue("2", 1, bgpPrefixEntry),
         createPrefixKeyValue("3", 1, ribPrefixEntry)},
        {},
        {},
        {});
    sendKvPublication(publication);
    recvRouteUpdates();
  }
}

/**
 * Test fixture for testing initial RIB computation in OpenR initialization
 * process.
 */
class InitialRibBuildTestFixture : public DecisionTestFixture {
  openr::thrift::OpenrConfig
  createConfig() override {
    auto tConfig = DecisionTestFixture::createConfig();

    // Set config originated prefixes.
    thrift::OriginatedPrefix originatedPrefixV4;
    originatedPrefixV4.prefix() = toString(addr1V4);
    originatedPrefixV4.minimum_supporting_routes() = 0;
    originatedPrefixV4.install_to_fib() = true;
    tConfig.originated_prefixes() = {originatedPrefixV4};

    // Enable Vip service.
    tConfig.enable_vip_service() = true;
    tConfig.vip_service_config() = vipconfig::config::VipServiceConfig();

    return tConfig;
  }

  void
  publishInitialPeers() override {
    // Do not publish peers information. Test case below will handle that.
  }
};

/*
 * Verify OpenR initialzation could succeed at current node (1),
 * - Receives adjacencies 1->2 (only used by 2) and 2->1 (only used by 1)
 * - Receives initial up peers 2 and 3 (Decision needs to wait for adjacencies
 *   with both peers).
 * - Receives CONFIG type static routes.
 * - Receives BGP or VIP type static routes
 * - Receives peer down event for node 3 (Decision does not need to wait for
 *   adjacencies with peer 3 anymore).
 * - Initial route computation is triggered, generating static routes.
 * - Receives updated adjacency 1->2 (can be used by anyone). Node 1 and 2 get
 *   connected, thus computed routes for prefixes advertised by node 2 and
 *   label route of node 2.
 */
TEST_F(InitialRibBuildTestFixture, PrefixWithVipRoutes) {
  // Send adj publication (current node is 1).
  // * adjacency "1->2" can only be used by node 2,
  // * adjacency "2->1" can only be used by node 1.
  // Link 1<->2 is not up since "1->2" cannot be used by node 1.
  // However, the two adjacencies will unblock
  sendKvPublication(
      createThriftPublication(
          {{"adj:1",
            createAdjValue(serializer, "1", 1, {adj12OnlyUsedBy2}, false, 1)},
           {"adj:2",
            createAdjValue(serializer, "2", 1, {adj21OnlyUsedBy1}, false, 2)}},
          {},
          {},
          {}),
      false /*prefixPubExists*/);

  int scheduleAt{0};
  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // KvStore publication is not process yet since initial peers are not
        // received.
        auto adjDb = decision->getDecisionAdjacenciesFiltered().get();
        ASSERT_EQ(adjDb->size(), 0);

        // Add initial UP peers "2" and "3".
        // Initial RIB computation will be blocked until dual directional
        // adjacencies are received for both peers.
        thrift::PeersMap peers;
        peers.emplace("2", thrift::PeerSpec());
        peers.emplace("3", thrift::PeerSpec());
        PeerEvent peerEvent{
            {kTestingAreaName, AreaPeerEvent(peers, {} /*peersToDel*/)}};
        peerUpdatesQueue.push(std::move(peerEvent));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // KvStore publication is processed and adjacence is extracted.
        auto adjDb = decision->getDecisionAdjacenciesFiltered().get();
        ASSERT_NE(adjDb->size(), 0);

        // Received KvStoreSynced signal.
        auto publication = createThriftPublication(
            /* prefix key format v2 */
            {createPrefixKeyValue("2", 1, addr1, kTestingAreaName, false)},
            /* expired-keys */
            {});
        sendKvPublication(publication);
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial RIB computation not triggered yet.
        EXPECT_EQ(0, routeUpdatesQueueReader.size());

        // Received static unicast routes for config originated prefixes.
        DecisionRouteUpdate configStaticRoutes;
        configStaticRoutes.prefixType = thrift::PrefixType::CONFIG;

        configStaticRoutes.addRouteToUpdate(RibUnicastEntry(
            toIPNetwork(addr1V4),
            {},
            addr1V4ConfigPrefixEntry,
            Constants::kDefaultArea.toString()));
        staticRouteUpdatesQueue.push(std::move(configStaticRoutes));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial RIB computation not triggered yet.
        EXPECT_EQ(0, routeUpdatesQueueReader.size());

        // Received static unicast routes for VIP prefixes.
        DecisionRouteUpdate vipStaticRoutes;
        vipStaticRoutes.prefixType = thrift::PrefixType::VIP;
        vipStaticRoutes.addRouteToUpdate(RibUnicastEntry(
            toIPNetwork(addr2V4),
            {},
            addr2VipPrefixEntry,
            Constants::kDefaultArea.toString()));
        staticRouteUpdatesQueue.push(std::move(vipStaticRoutes));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial RIB computation not triggered yet.
        EXPECT_EQ(0, routeUpdatesQueueReader.size());

        // Initial UP peer "3" goes down. Open/R initialization does not wait
        // for adjacency with the peer.
        PeerEvent newPeerEvent{
            {kTestingAreaName, AreaPeerEvent({} /*peersToAdd*/, {"3"})}};
        peerUpdatesQueue.push(std::move(newPeerEvent));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial RIB computation is triggered.
        // Generated static routes and node label route for node 1.
        auto routeDbDelta = recvRouteUpdates();

        // Static config originated route and static VIP route.
        EXPECT_EQ(2, routeDbDelta.unicastRoutesToUpdate.size());

        // Send adj publication.
        // Updated adjacency for peer "2" is received,
        // * adjacency "1->2" can be used by all nodes.
        sendKvPublication(createThriftPublication(
            {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)}},
            {},
            {},
            {}));

        routeDbDelta = recvRouteUpdates();
        // Unicast route for addr1 advertised by node 2.
        EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
        EXPECT_EQ(
            routeDbDelta.unicastRoutesToUpdate.begin()->second.prefix,
            toIPNetwork(addr1));

        evb.stop();
      });
  // let magic happen
  evb.run();
}

/**
 * Test fixture for testing Decision module with V4 over V6 nexthop feature.
 */
class DecisionV4OverV6NexthopTestFixture : public DecisionTestFixture {
 protected:
  /**
   * The only differences between this test fixture and the DecisionTetFixture
   * is the config where here we enable V4OverV6Nexthop
   */
  openr::thrift::OpenrConfig
  createConfig() override {
    tConfig_ = getBasicOpenrConfig(
        "1", /* nodeName */
        {}, /* areaCfg */
        true, /* enable v4 */
        false, /* dryrun */
        true /* enableV4OverV6Nexthop */);
    return tConfig_;
  }

  openr::thrift::OpenrConfig tConfig_;
};

/**
 * Similar as the BasicalOperations, we test the Decision module with the
 * v4_over_v6_nexthop feature enabled.
 *
 * We are using the topology: 1---2---3
 *
 * We upload the link 1--2 with initial sync and later publish the 2---3 link
 * information. We check the nexthop from full routing dump and other fields.
 */
TEST_F(DecisionV4OverV6NexthopTestFixture, BasicOperationsV4OverV6Nexthop) {
  // First make sure the v4 over v6 nexthop is enabled
  EXPECT_TRUE(*tConfig_.v4_over_v6_nexthop());

  // public the link state info to KvStore
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1V4),
       createPrefixKeyValue("2", 1, addr2V4)},
      {},
      {},
      {});

  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();

  auto routeDb = dumpRouteDb({"1"})["1"];

  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj12,
          true /*isV4*/,
          10,
          std::nullopt,
          kTestingAreaName,
          true /*v4OverV6Nexthop*/)}));

  // for router 3 we publish new key-value
  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue(serializer, "2", 2, {adj21, adj23}, false, 2)},
       createPrefixKeyValue("3", 1, addr3V4)},
      {},
      {},
      {});

  sendKvPublication(publication);

  routeDb = dumpRouteDb({"1"})["1"];
  fillRouteMap("1", routeMap, routeDb);

  // nexthop checking for node 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj12, true, 10, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3V4))],
      NextHops({createNextHopFromAdj(
          adj12, true, 20, std::nullopt, kTestingAreaName, true)}));

  auto routeDbMap = dumpRouteDb({"2", "3"});
  for (auto& [key, value] : routeDbMap) {
    fillRouteMap(key, routeMap, value);
  }

  // nexthop checking for node 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1V4))],
      NextHops({createNextHopFromAdj(
          adj21, true, 10, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3V4))],
      NextHops({createNextHopFromAdj(
          adj23, true, 10, std::nullopt, kTestingAreaName, true)}));

  // nexthop checking for node 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1V4))],
      NextHops({createNextHopFromAdj(
          adj32, true, 20, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj32, true, 10, std::nullopt, kTestingAreaName, true)}));
}

/**
 * Test fixture for testing Decision module with V4 over V6 nexthop feature.
 */
class DecisionV4OverV6NexthopWithNoV4TestFixture : public DecisionTestFixture {
 protected:
  /**
   * The only differences between this test fixture and the DecisionTetFixture
   * is the config where here we enable V4OverV6Nexthop
   */
  openr::thrift::OpenrConfig
  createConfig() override {
    tConfig_ = getBasicOpenrConfig(
        "1", /* nodeName */
        {}, /* areaCfg */
        false, /* enable v4 */
        false, /* dryrun */
        true /* enableV4OverV6Nexthop */);
    return tConfig_;
  }

  openr::thrift::OpenrConfig tConfig_;
};

/**
 * Similar as the BasicalOperations, we test the Decision module with the
 * v4_over_v6_nexthop feature enabled.
 *
 * We are using the topology: 1---2---3
 *
 * We upload the link 1--2 with initial sync and later publish the 2---3 link
 * information. We check the nexthop from full routing dump and other fields.
 */
TEST_F(
    DecisionV4OverV6NexthopWithNoV4TestFixture,
    BasicOperationsV4OverV6NexthopWithNoV4Interface) {
  // First make sure the v4 over v6 nexthop is enabled
  EXPECT_TRUE(*tConfig_.v4_over_v6_nexthop());

  // public the link state info to KvStore
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1V4),
       createPrefixKeyValue("2", 1, addr2V4)},
      {},
      {},
      {});

  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();

  auto routeDb = dumpRouteDb({"1"})["1"];

  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj12,
          true /*isV4*/,
          10,
          std::nullopt,
          kTestingAreaName,
          true /*v4OverV6Nexthop*/)}));

  // for router 3 we publish new key-value
  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue(serializer, "2", 2, {adj21, adj23}, false, 2)},
       createPrefixKeyValue("3", 1, addr3V4)},
      {},
      {},
      {});

  sendKvPublication(publication);

  routeDb = dumpRouteDb({"1"})["1"];
  fillRouteMap("1", routeMap, routeDb);

  // nexthop checking for node 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj12, true, 10, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3V4))],
      NextHops({createNextHopFromAdj(
          adj12, true, 20, std::nullopt, kTestingAreaName, true)}));

  auto routeDbMap = dumpRouteDb({"2", "3"});
  for (auto& [key, value] : routeDbMap) {
    fillRouteMap(key, routeMap, value);
  }

  // nexthop checking for node 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1V4))],
      NextHops({createNextHopFromAdj(
          adj21, true, 10, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3V4))],
      NextHops({createNextHopFromAdj(
          adj23, true, 10, std::nullopt, kTestingAreaName, true)}));

  // nexthop checking for node 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1V4))],
      NextHops({createNextHopFromAdj(
          adj32, true, 20, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj32, true, 10, std::nullopt, kTestingAreaName, true)}));
}

TEST(DecisionPendingUpdates, needsFullRebuild) {
  openr::detail::DecisionPendingUpdates updates("node1");
  LinkState::LinkStateChange linkStateChange;

  linkStateChange.linkAttributesChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  updates.applyLinkStateChange("node1", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());

  updates.reset();
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  linkStateChange.linkAttributesChanged = false;
  linkStateChange.topologyChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());

  updates.reset();
  linkStateChange.topologyChanged = false;
  linkStateChange.nodeLabelChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());
}

TEST(DecisionPendingUpdates, updatedPrefixes) {
  openr::detail::DecisionPendingUpdates updates("node1");

  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());

  // empty update no change
  updates.applyPrefixStateChange({}, kEmptyPerfEventRef);
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());

  updates.applyPrefixStateChange(
      {addr1Cidr, toIPNetwork(addr2V4)}, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_THAT(
      updates.updatedPrefixes(),
      testing::UnorderedElementsAre(addr1Cidr, addr2V4Cidr));
  updates.applyPrefixStateChange({addr2Cidr}, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_THAT(
      updates.updatedPrefixes(),
      testing::UnorderedElementsAre(addr1Cidr, addr2V4Cidr, addr2Cidr));

  updates.reset();
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());
}

/*
 * @brief  Verify that we report counters of link event propagation time
 *         correctly. Verification at the unit of updateAdjacencyDatabase
 *         API calls
 */
TEST_F(DecisionTestFixture, TestLinkPropagationWithAdjDbUpdateApi) {
  auto now = getUnixTimeStampMs();
  std::string nodeName("1");
  auto linkState = LinkState(kTestingAreaName, nodeName);
  fb303::fbData->resetAllData();

  auto adj1 =
      createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
  auto adjDb1 = createAdjDb("1", {adj1}, 1);
  linkState.updateAdjacencyDatabase(adjDb1, kTestingAreaName);

  /*
   * Up link event during initialization
   * Propagation time reporting is skipped during initialization
   */
  auto adj2 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);
  auto adjDb2 = createAdjDb("2", {adj2}, 2);
  thrift::LinkStatusRecords rec1;
  rec1.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::UP;
  rec1.linkStatusMap()["2/1"].unixTs() = now - 10;
  adjDb2.linkStatusRecords() = rec1;
  linkState.updateAdjacencyDatabase(adjDb2, kTestingAreaName, true);

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters["decision.linkstate.up.propagation_time_ms.avg.60"], 0);

  /*
   * Down link event post initialization
   * Propagation time reporting is to occur from here onwards
   */
  adjDb2 = createAdjDb("2", {}, 2);
  rec1.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::DOWN;
  rec1.linkStatusMap()["2/1"].unixTs() = now - 100;
  adjDb2.linkStatusRecords() = rec1;
  linkState.updateAdjacencyDatabase(adjDb2, kTestingAreaName);

  counters = fb303::fbData->getCounters();
  EXPECT_GE(
      counters["decision.linkstate.down.propagation_time_ms.avg.60"], 100);

  // Down link event with timestamp is not updated, then it's skipped
  fb303::fbData->resetAllData();
  adjDb2 = createAdjDb("2", {}, 2);
  rec1.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::DOWN;
  rec1.linkStatusMap()["2/1"].unixTs() = 0;
  adjDb2.linkStatusRecords() = rec1;
  linkState.updateAdjacencyDatabase(adjDb2, kTestingAreaName);
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters["decision.linkstate.down.propagation_time_ms.avg.60"], 0);
}

/*
 * @brief  Verify that we report counters of link event propagation time
 *         correctly. Verified at the unit of Thrift Publications and
 *         before/after initialization signals
 */
TEST_F(DecisionTestFixture, TestLinkPropagationInitializationSignal) {
  auto now = getUnixTimeStampMs();
  fb303::fbData->resetAllData();

  thrift::LinkStatusRecords lsRec1, lsRec2;
  lsRec1.linkStatusMap()["1/2"].status() = thrift::LinkStatusEnum::UP;
  lsRec1.linkStatusMap()["1/2"].unixTs() = now - 10;
  lsRec2.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::UP;
  lsRec2.linkStatusMap()["2/1"].unixTs() = now - 10;

  auto publication = createThriftPublication(
      {{"adj:1",
        createAdjValueWithLinkStatus(
            serializer, "1", 1, {adj12}, lsRec1, false, 1)},
       {"adj:2",
        createAdjValueWithLinkStatus(
            serializer, "2", 1, {adj21}, lsRec2, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  sendKvPublication(publication);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  /*
   * Initial updates, before Initialization shall not produce
   * propagation time counters
   */
  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters["decision.linkstate.up.propagation_time_ms.avg.60"], 0);

  kvStoreUpdatesQueue.push(thrift::InitializationEvent::ADJACENCY_DB_SYNCED);

  lsRec1.linkStatusMap()["1/2"].status() = thrift::LinkStatusEnum::DOWN;
  lsRec1.linkStatusMap()["1/2"].unixTs() = now - 4;
  lsRec2.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::DOWN;
  lsRec2.linkStatusMap()["2/1"].unixTs() = now - 4;

  publication = createThriftPublication(
      {{"adj:1",
        createAdjValueWithLinkStatus(serializer, "1", 2, {}, lsRec1, false, 1)},
       {"adj:2",
        createAdjValueWithLinkStatus(
            serializer, "2", 2, {}, lsRec2, false, 2)}},
      {},
      {},
      {});
  sendKvPublication(publication);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  /*
   * Update before initialization complete
   */
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters["decision.linkstate.up.propagation_time_ms.avg.60"], 0);

  kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);
  kvStoreUpdatesQueue.push(thrift::InitializationEvent::ADJACENCY_DB_SYNCED);

  lsRec1.linkStatusMap()["1/2"].status() = thrift::LinkStatusEnum::UP;
  lsRec1.linkStatusMap()["1/2"].unixTs() = now - 4;
  lsRec2.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::UP;
  lsRec2.linkStatusMap()["2/1"].unixTs() = now - 4;

  publication = createThriftPublication(
      {{"adj:1",
        createAdjValueWithLinkStatus(
            serializer, "1", 2, {adj12}, lsRec1, false, 1)},
       {"adj:2",
        createAdjValueWithLinkStatus(
            serializer, "2", 2, {adj21}, lsRec2, false, 2)}},
      {},
      {},
      {});
  sendKvPublication(publication);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  /*
   * This publication is after initialization, verify that it produces
   * propagation time counters
   */
  counters = fb303::fbData->getCounters();
  EXPECT_GT(counters["decision.linkstate.up.propagation_time_ms.avg.60"], 1);
  EXPECT_LT(counters["decision.linkstate.up.propagation_time_ms.avg.60"], 4000);
}

/*
 * @brief  Verify that we report counters of link event propagation time
 *         correctly. Verified at the unit of Thrift Publications and
 *         first/subsequent publication calls
 */
TEST_F(DecisionTestFixture, TestLinkPropagationWithFirstPubCall) {
  auto now = getUnixTimeStampMs();
  fb303::fbData->resetAllData();

  kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);
  kvStoreUpdatesQueue.push(thrift::InitializationEvent::ADJACENCY_DB_SYNCED);

  thrift::LinkStatusRecords lsRec1, lsRec2;
  lsRec1.linkStatusMap()["1/2"].status() = thrift::LinkStatusEnum::UP;
  lsRec1.linkStatusMap()["1/2"].unixTs() = now - 10;
  lsRec2.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::UP;
  lsRec2.linkStatusMap()["2/1"].unixTs() = now - 10;

  auto publication = createThriftPublication(
      {{"adj:1",
        createAdjValueWithLinkStatus(
            serializer, "1", 1, {adj12}, lsRec1, false, 1)},
       {"adj:2",
        createAdjValueWithLinkStatus(
            serializer, "2", 1, {adj21}, lsRec2, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  sendKvPublication(publication);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  /*
   * First publication after initialization, propagation time counters
   * should not be updated
   */
  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters["decision.linkstate.up.propagation_time_ms.avg.60"], 0);

  lsRec1.linkStatusMap()["1/2"].status() = thrift::LinkStatusEnum::DOWN;
  lsRec1.linkStatusMap()["1/2"].unixTs() = now - 4;
  lsRec2.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::DOWN;
  lsRec2.linkStatusMap()["2/1"].unixTs() = now - 4;

  publication = createThriftPublication(
      {{"adj:1",
        createAdjValueWithLinkStatus(serializer, "1", 2, {}, lsRec1, false, 1)},
       {"adj:2",
        createAdjValueWithLinkStatus(
            serializer, "2", 2, {}, lsRec2, false, 2)}},
      {},
      {},
      {});
  sendKvPublication(publication);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  /*
   * Second publication after initialization, propagation time counters
   * should have been updated
   */
  counters = fb303::fbData->getCounters();
  EXPECT_GT(counters["decision.linkstate.down.propagation_time_ms.avg.60"], 1);
  EXPECT_LT(
      counters["decision.linkstate.down.propagation_time_ms.avg.60"], 4000);
}

TEST(DecisionPendingUpdates, perfEvents) {
  openr::detail::DecisionPendingUpdates updates("node1");
  LinkState::LinkStateChange linkStateChange;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_THAT(*updates.perfEvents()->events(), testing::SizeIs(1));
  EXPECT_EQ(
      *updates.perfEvents()->events()->front().eventDescr(),
      "DECISION_RECEIVED");
  thrift::PrefixDatabase perfEventDb;
  perfEventDb.perfEvents() = openr::thrift::PerfEvents();
  auto& earlierEvents = *perfEventDb.perfEvents();
  earlierEvents.events()->emplace_back();
  *earlierEvents.events()->back().nodeName() = "node3";
  *earlierEvents.events()->back().eventDescr() = "EARLIER";
  earlierEvents.events()->back().unixTs() = 1;
  updates.applyPrefixStateChange({}, perfEventDb.perfEvents());

  // expect what we hasd to be displaced by this
  EXPECT_THAT(*updates.perfEvents()->events(), testing::SizeIs(2));
  EXPECT_EQ(*updates.perfEvents()->events()->front().eventDescr(), "EARLIER");
  EXPECT_EQ(
      *updates.perfEvents()->events()->back().eventDescr(),
      "DECISION_RECEIVED");
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  const folly::Init init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
