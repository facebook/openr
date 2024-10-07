/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;

class KvStoreThriftTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // nothing to do
  }

  void
  TearDown() override {
    // close kvStoreReaderQueue to unblock server bring down
    for (auto& store : stores_) {
      store->closeQueue();
    }

    // tear down kvStore instances
    for (auto& store : stores_) {
      store->stop();
    }
  }

  void
  createKvStore(const std::string& nodeId) {
    // create KvStoreConfig
    thrift::KvStoreConfig kvStoreConfig;
    kvStoreConfig.node_name() = nodeId;
    const std::unordered_set<std::string> areaIds{kTestingAreaName};

    stores_.emplace_back(
        std::make_shared<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            areaIds, kvStoreConfig));
    stores_.back()->run();
  }
  void
  createKvStore(const std::string& nodeId, const std::string areaId) {
    // create KvStoreConfig
    thrift::KvStoreConfig kvStoreConfig;
    kvStoreConfig.node_name() = nodeId;
    const std::unordered_set<std::string> areaIds{areaId};

    stores_.emplace_back(
        std::make_shared<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            areaIds, kvStoreConfig));
    stores_.back()->run();
  }

  bool
  verifyKvStoreKeyVal(
      KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* kvStore,
      const std::string& key,
      const thrift::Value& thriftVal,
      const AreaId& area,
      std::optional<std::chrono::milliseconds> processingTimeout =
          Constants::kPlatformProcTimeout) noexcept {
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > processingTimeout.value()) {
        LOG(ERROR) << "Timeout verifying key: " << key
                   << " inside KvStore: " << kvStore->getNodeId();
        break;
      }
      auto val = kvStore->getKey(area, key);
      if (val.has_value() and val.value() == thriftVal) {
        return true;
      }

      // yield to avoid hogging the process
      std::this_thread::yield();
    }
    return false;
  }

  bool
  verifyKvStorePeerState(
      KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* kvStore,
      const std::string& peerName,
      thrift::KvStorePeerState expPeerState,
      const AreaId& area,
      std::optional<std::chrono::milliseconds> processingTimeout =
          Constants::kPlatformProcTimeout) noexcept {
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > processingTimeout.value()) {
        LOG(ERROR)
            << "Timeout verifying state: "
            << apache::thrift::util::enumNameSafe<thrift::KvStorePeerState>(
                   expPeerState)
            << " against peer: " << peerName;
        break;
      }
      auto state = kvStore->getPeerState(area, peerName);
      if (state.has_value() and state.value() == expPeerState) {
        return true;
      }

      // yield to avoid hogging the process
      std::this_thread::yield();
    }
    return false;
  }

  // initialize maximum waiting time to check key-val:
  const std::chrono::milliseconds waitTime_{1000};

  // vector of KvStores created
  std::vector<
      std::shared_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>>
      stores_{};
};

//
// class for simple topology creation, which has:
//  1) Create a 2 kvstore intances with `enableKvStoreThrift` knob open;
//  2) Inject different keys to different store and make sure it is
//     mutual exclusive;
//
class SimpleKvStoreThriftTestFixture : public KvStoreThriftTestFixture {
 protected:
  void
  createSimpleThriftTestTopo() {
    // spin up one kvStore instance and thriftServer
    createKvStore(node1);

    // spin up another kvStore instance and thriftServer
    createKvStore(node2);

    // injecting different key-value in diff stores
    thriftVal1 = createThriftValue(
        1, stores_.front()->getNodeId(), std::string("value1"));
    thriftVal2 = createThriftValue(
        2, stores_.back()->getNodeId(), std::string("value2"));
    EXPECT_TRUE(stores_.front()->setKey(kTestingAreaName, key1, thriftVal1));
    EXPECT_TRUE(stores_.back()->setKey(kTestingAreaName, key2, thriftVal2));

    // check key ONLY exists in one store, not the other
    EXPECT_TRUE(stores_.front()->getKey(kTestingAreaName, key1).has_value());
    EXPECT_FALSE(stores_.back()->getKey(kTestingAreaName, key1).has_value());
    EXPECT_FALSE(stores_.front()->getKey(kTestingAreaName, key2).has_value());
    EXPECT_TRUE(stores_.back()->getKey(kTestingAreaName, key2).has_value());
  }

  uint16_t
  generateRandomDiffPort(const std::unordered_set<uint16_t>& ports) {
    while (true) {
      // generate port between 1 - 65535
      uint16_t randPort = folly::Random::rand32() % 65535 + 1;
      if (not ports.count(randPort)) {
        return randPort;
      }
      // avoid hogging process
      std::this_thread::yield();
    }
  }

 public:
  const std::string key1{"key1"};
  const std::string key2{"key2"};
  const std::string node1{"node-1"};
  const std::string node2{"node-2"};
  thrift::Value thriftVal1{};
  thrift::Value thriftVal2{};
};

//
// Positive case for initial full-sync over thrift
//
// 1) Start 2 kvStores and 2 corresponding thrift servers.
// 2) Add peer to each other;
// 3) Make sure full-sync is performed and reach global consistency;
// 4) Remove peers to check `KvStoreThriftPeers` data-strcuture;
//
TEST_F(SimpleKvStoreThriftTestFixture, InitialThriftSync) {
  // create 2 nodes topology for thrift peers
  createSimpleThriftTestTopo();

  // build peerSpec for thrift peer connection
  auto store1 = stores_.front();
  auto store2 = stores_.back();
  auto peerSpec1 = store1->getPeerSpec();
  auto peerSpec2 = store2->getPeerSpec();

  // eventbase to schedule callbacks at certain time spot
  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    //
    // Step1: Add peer to each other's KvStore instances
    //        Expect full-sync request exchanged;
    //
    EXPECT_TRUE(
        store1->addPeer(kTestingAreaName, store2->getNodeId(), peerSpec2));
    EXPECT_TRUE(
        store2->addPeer(kTestingAreaName, store1->getNodeId(), peerSpec1));

    // verifying keys are exchanged between peers
    EXPECT_TRUE(verifyKvStorePeerState(
        store1.get(),
        store2->getNodeId(),
        thrift::KvStorePeerState::INITIALIZED,
        kTestingAreaName));
    EXPECT_TRUE(verifyKvStorePeerState(
        store2.get(),
        store1->getNodeId(),
        thrift::KvStorePeerState::INITIALIZED,
        kTestingAreaName));

    // dump peers to make sure they are aware of each other
    peerSpec1.state() = thrift::KvStorePeerState::INITIALIZED;
    peerSpec2.state() = thrift::KvStorePeerState::INITIALIZED;

    auto store2NodeId = store2->getNodeId();
    std::unordered_map<std::string, thrift::PeerSpec> expPeer1_1 = {
        {store2NodeId, peerSpec2}};
    auto store1NodeId = store1->getNodeId();
    std::unordered_map<std::string, thrift::PeerSpec> expPeer2_1 = {
        {store1NodeId, peerSpec1}};

    auto cmpPeer = store1->getPeers(kTestingAreaName);
    const auto cmpElapsedTime{2000};
    EXPECT_EQ(*cmpPeer[store2NodeId].peerAddr(), *peerSpec2.peerAddr());
    EXPECT_EQ(*cmpPeer[store2NodeId].ctrlPort(), *peerSpec2.ctrlPort());
    EXPECT_EQ(*cmpPeer[store2NodeId].state(), *peerSpec2.state());
    EXPECT_LT(*cmpPeer[store2NodeId].stateElapsedTimeMs(), cmpElapsedTime);
    EXPECT_EQ(*cmpPeer[store2NodeId].flaps(), 0);

    cmpPeer = store2->getPeers(kTestingAreaName);
    EXPECT_EQ(*cmpPeer[store1NodeId].peerAddr(), *peerSpec1.peerAddr());
    EXPECT_EQ(*cmpPeer[store1NodeId].ctrlPort(), *peerSpec1.ctrlPort());
    EXPECT_EQ(*cmpPeer[store1NodeId].state(), *peerSpec1.state());
    EXPECT_LT(*cmpPeer[store1NodeId].stateElapsedTimeMs(), cmpElapsedTime);
    EXPECT_EQ(*cmpPeer[store1NodeId].flaps(), 0);

    EXPECT_TRUE(
        verifyKvStoreKeyVal(store1.get(), key2, thriftVal2, kTestingAreaName));
    EXPECT_TRUE(
        verifyKvStoreKeyVal(store2.get(), key1, thriftVal1, kTestingAreaName));

    EXPECT_TRUE(
        verifyKvStoreKeyVal(store1.get(), key2, thriftVal2, kTestingAreaName));
    EXPECT_TRUE(
        verifyKvStoreKeyVal(store2.get(), key1, thriftVal1, kTestingAreaName));

    EXPECT_EQ(2, store1->dumpAll(kTestingAreaName).size());
    EXPECT_EQ(2, store2->dumpAll(kTestingAreaName).size());

    //
    // Step2: Update peer with different thrift peerAddr
    //        Expect full-sync request being sent;
    //
    store2.reset(); // shared_ptr needs to be cleaned up everywhere!
    stores_.back()->closeQueue();
    stores_.back()->stop();
    stores_.pop_back();

    // recreate store2 and corresponding thriftServer
    createKvStore(node2);
    store2 = stores_.back();
    auto newPeerSpec = store2->getPeerSpec();

    // TODO: add counter verification for state change to IDLE
    EXPECT_TRUE(
        store1->addPeer(kTestingAreaName, store2->getNodeId(), newPeerSpec));

    // verify another full-sync request being sent
    EXPECT_TRUE(verifyKvStorePeerState(
        store1.get(),
        store2->getNodeId(),
        thrift::KvStorePeerState::INITIALIZED,
        kTestingAreaName));

    newPeerSpec.state() = thrift::KvStorePeerState::INITIALIZED;
    store2NodeId = store2->getNodeId();
    std::unordered_map<std::string, thrift::PeerSpec> newExpPeer = {
        {store2NodeId, newPeerSpec}};

    cmpPeer = store1->getPeers(kTestingAreaName);
    EXPECT_EQ(*cmpPeer[store2NodeId].peerAddr(), *newPeerSpec.peerAddr());
    EXPECT_EQ(*cmpPeer[store2NodeId].ctrlPort(), *newPeerSpec.ctrlPort());
    EXPECT_EQ(*cmpPeer[store2NodeId].state(), *newPeerSpec.state());
    EXPECT_LT(*cmpPeer[store2NodeId].stateElapsedTimeMs(), cmpElapsedTime);
    EXPECT_EQ(*cmpPeer[store2NodeId].flaps(), 0);

    EXPECT_TRUE(
        verifyKvStoreKeyVal(store1.get(), key2, thriftVal2, kTestingAreaName));
    EXPECT_TRUE(
        verifyKvStoreKeyVal(store1.get(), key2, thriftVal2, kTestingAreaName));

    evb.stop();
  });

  evb.run();

  //
  // Step3: Remove peers
  //
  EXPECT_TRUE(store1->delPeer(kTestingAreaName, store2->getNodeId()));
  EXPECT_TRUE(store2->delPeer(kTestingAreaName, store1->getNodeId()));
  EXPECT_EQ(0, store1->getPeers(kTestingAreaName).size());
  EXPECT_EQ(0, store2->getPeers(kTestingAreaName).size());
}

//
// Negative test case for initial full-sync over thrift
//
// 1) Start 2 kvStores and 2 corresponding thrift servers;
// 2) Jeopardize port number to mimick thrift exception;
// 3) Add peer to each other;
// 4) Make sure full-sync encountered expcetion and no
//    kvStore full-sync going through;
//
TEST_F(SimpleKvStoreThriftTestFixture, FullSyncWithException) {
  // create 2 nodes topology for thrift peers
  createSimpleThriftTestTopo();

  // build peerSpec for thrift client connection
  auto store1 = stores_.front();
  auto store2 = stores_.back();
  auto peerSpec1 = store1->getPeerSpec();
  auto peerSpec2 = store2->getPeerSpec();

  // create dummy port in purpose to mimick exception connecting thrift server
  // ATTN: explicitly make sure dummy port used will be different to thrift
  // server ports
  std::unordered_set<uint16_t> usedPorts{
      store1->getThriftPort(), store2->getThriftPort()};
  const uint16_t dummyPort1 = generateRandomDiffPort(usedPorts);
  const uint16_t dummyPort2 = generateRandomDiffPort(usedPorts);
  peerSpec1.ctrlPort() = dummyPort1;
  peerSpec2.ctrlPort() = dummyPort2;

  EXPECT_TRUE(
      store1->addPeer(kTestingAreaName, store2->getNodeId(), peerSpec2));
  EXPECT_TRUE(
      store2->addPeer(kTestingAreaName, store1->getNodeId(), peerSpec1));

  // verifying keys are NOT exchanged between peers
  EXPECT_FALSE(verifyKvStoreKeyVal(
      store1.get(), key2, thriftVal2, kTestingAreaName, waitTime_));
  EXPECT_FALSE(verifyKvStoreKeyVal(
      store2.get(), key1, thriftVal1, kTestingAreaName, waitTime_));

  // verify no initial sync event
  EXPECT_EQ(1, store1->dumpAll(kTestingAreaName).size());
  EXPECT_EQ(1, store2->dumpAll(kTestingAreaName).size());
}

TEST_F(KvStoreThriftTestFixture, KvStoreTermination) {
  const std::string node1{"node-1"};

  createKvStore(node1);
  auto store = stores_.back();
  EXPECT_FALSE(store->getKvStore()->getIsStopped(kTestingAreaName));

  // random areaId will throw exception
  EXPECT_THROW(
      store->getKvStore()->getIsStopped("random-areaId"), std::runtime_error);

  // trigger termination/stop
  store->stop();
  EXPECT_TRUE(store->getKvStore()->getIsStopped(kTestingAreaName));
}

class KvStoreThriftTestFixtureWithAreaParams
    : public KvStoreThriftTestFixture,
      public ::testing::WithParamInterface<
          std::pair<openr::AreaId, std::string>> {};

INSTANTIATE_TEST_SUITE_P(
    publishPeerStateCountersTest,
    KvStoreThriftTestFixtureWithAreaParams,
    ::testing::Values(
        std::make_pair(openr::AreaId{"test_area_name"}, "UNKNOWN"),
        std::make_pair(openr::AreaId{"snc1.fa007"}, "HGRID"),
        std::make_pair(openr::AreaId{"snc.slice001"}, "SLICE"),
        std::make_pair(openr::AreaId{"snc1.f02.s001"}, "SPINE"),
        std::make_pair(openr::AreaId{"snc1.f02.p004"}, "POD")));

// +-------------+-------+-------------+---------------------------+
// | total_peers | IDLE  | INITIALIZED | all_peers_not_initialized |
// +-------------+-------+-------------+---------------------------+
// |             |   0   |      2      |             0             |
// |      2      |   1   |      1      |             0             |
// |             |   2   |      0      |             1             |
// +-------------+-------+-------------+---------------------------+
TEST_P(KvStoreThriftTestFixtureWithAreaParams, PublishPeerStateCountersTest) {
  fb303::fbData->resetAllData();

  const std::string node1{"node-1"};
  const std::string node2{"node-2"};
  const std::string node3{"node-3"};

  const openr::AreaId testAreaId =
      std::get<0>(KvStoreThriftTestFixtureWithAreaParams::GetParam());

  createKvStore(node1, testAreaId);
  createKvStore(node2, testAreaId);
  createKvStore(node3, testAreaId);
  auto store1 = stores_[0];
  auto store2 = stores_[1];
  auto store3 = stores_[2];

  // add 2 peers to store1
  store1->addPeer(testAreaId, store2->getNodeId(), store2->getPeerSpec());
  store1->addPeer(testAreaId, store3->getNodeId(), store3->getPeerSpec());

  const std::string& testAreaName = testAreaId;
  std::string areaTypeStr = getAreaTypeByAreaName(testAreaName);
  ASSERT_EQ(
      std::get<1>(KvStoreThriftTestFixtureWithAreaParams::GetParam()),
      areaTypeStr);

  //
  // Construct the counters' keys string
  //
  // kvstore.num_peers.<UNKNOWN/HGRID/SLICE/SPINE/POD>.IDLE
  const std::string idleKeyStr = fmt::format(
      Constants::kKvStoreNumPeerByStateCounter, areaTypeStr, "IDLE");
  // kvstore.num_peers.<UNKNOWN/HGRID/SLICE/SPINE/POD>.INITIALIZED
  const std::string initializedKeyStr = fmt::format(
      Constants::kKvStoreNumPeerByStateCounter, areaTypeStr, "INITIALIZED");
  // kvstore.all_peers_not_initialized.<UNKNOWN/HGRID/SLICE/SPINE/POD>
  const std::string allPeerNotInitializedKeyStr =
      fmt::format(Constants::kKvStoreAllPeerNotInitialized, areaTypeStr);

  // eventbase to schedule callbacks at certain time spot
  OpenrEventBase evb;

  evb.scheduleTimeout(std::chrono::milliseconds(200), [&]() noexcept {
    auto allCounters = fb303::fbData->getCounters();

    // Get number of IDLE peers
    auto numIdlePeer = allCounters[idleKeyStr];
    // Get number of INITIALIZED peers
    auto numInitializedPeer = allCounters[initializedKeyStr];
    // Get boolean counter whether no peer is initialized
    auto allPeerNotInitialized = allCounters[allPeerNotInitializedKeyStr];

    EXPECT_EQ(0, numIdlePeer);
    EXPECT_EQ(2, numInitializedPeer);
    // If there is at least 1 peer is initialized state, this
    // kvstore.all_peers_not_initialized.<UNKNOWN/HGRID/SLICE/SPINE/POD> will be
    // 0
    EXPECT_EQ(0, allPeerNotInitialized);

    // inject thrift failure for this peer `node-2`, processing time is 500ms
    // the peer from INITIALIZED state will be changed to IDLE state
    // the next evb scheduled task will verify the number of IDLE and
    // INITIALIZED peers
    store1->injectThriftFailure(testAreaId, store2->getNodeId());
  });

  evb.scheduleTimeout(std::chrono::milliseconds(1000), [&]() noexcept {
    auto allCounters = fb303::fbData->getCounters();

    // Get number of IDLE peers
    auto numIdlePeer = allCounters[idleKeyStr];
    // Get number of INITIALIZED peers
    auto numInitializedPeer = allCounters[initializedKeyStr];
    // Verify if no peers is in INITIALIZED state
    auto allPeerNotInitialized = allCounters[allPeerNotInitializedKeyStr];

    // verify the number of IDLE and INITIALIZED peers
    //
    // IDLE peer should go from 0 to 1 after the injected thriftfailure
    // INITIALIZED peer should go from 2 to 1 after the injected
    // thriftfailure
    EXPECT_EQ(1, numIdlePeer);
    EXPECT_EQ(1, numInitializedPeer);
    // If there is at least 1 peer is initialized state, this
    // kvstore.all_peers_not_initialized.<UNKNOWN/HGRID/SLICE/SPINE/POD> will be
    // 0
    EXPECT_EQ(0, allPeerNotInitialized);

    // inject thrift failure for this peer `node-3`, processing time is 500ms
    // the peer from INITIALIZED state will be changed to IDLE state
    // the next evb scheduled task will verify the number of IDLE and
    // INITIALIZED peers
    store1->injectThriftFailure(testAreaId, store3->getNodeId());
  });

  evb.scheduleTimeout(std::chrono::milliseconds(2000), [&]() noexcept {
    auto allCounters = fb303::fbData->getCounters();

    // Get number of IDLE peers
    auto numIdlePeer = allCounters[idleKeyStr];
    // Get number of INITIALIZED peers
    auto numInitializedPeer = allCounters[initializedKeyStr];
    // Verify if no peers is in INITIALIZED state
    auto allPeerNotInitialized = allCounters[allPeerNotInitializedKeyStr];

    // verify the number of IDLE and INITIALIZED peers
    //
    // IDLE peer should go from 1 to 2 after another injected thriftfailure
    // INITIALIZED peer should go from 1 to 0 after another injected
    // thriftfailure
    EXPECT_EQ(2, numIdlePeer);
    EXPECT_EQ(0, numInitializedPeer);
    // If all peers are NOT in initialized state, this
    // kvstore.all_peers_not_initialized.<UNKNOWN/HGRID/SLICE/SPINE/POD> will be
    // 1
    EXPECT_EQ(1, allPeerNotInitialized);

    // stop the eventbase
    evb.stop();
  });

  // start the eventbase
  evb.run();
}

//
// Test case to verify correctness of 3-way full-sync
// Tuple => (key, version, value)
//
// store1 has (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 1, a)
// store2 has             (k1, 1, a), (k2, 1, b), (k3, 9, b), (k4, 6, b)
//
// After store1 did a full-sync with store2, we expect both have:
//
// (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 9, b), (k4, 6, b)
//
TEST_F(KvStoreThriftTestFixture, UnidirectionThriftFullSync) {
  // Reset fb303 data for every test to make sure clean startup
  facebook::fb303::fbData->resetAllData();

  // spin up 2 kvStore instances and thriftServers
  const std::string node1{"node-1"};
  const std::string node2{"node-2"};
  const std::string value1{"value-1"};
  const std::string value2{"value-2"};

  createKvStore(node1);
  createKvStore(node2);
  auto store1 = stores_.front();
  auto store2 = stores_.back();

  // inject keys in store1 and store2
  const std::string k0{"key0"};
  const std::string k1{"key1"};
  const std::string k2{"key2"};
  const std::string k3{"key3"};
  const std::string k4{"key4"};
  std::vector<std::string> allKeys = {k0, k1, k2, k3, k4};
  std::vector<std::pair<std::string, int>> keyVersionAs = {
      {k0, 5}, {k1, 1}, {k2, 9}, {k3, 1}};
  std::vector<std::pair<std::string, int>> keyVersionBs = {
      {k1, 1}, {k2, 1}, {k3, 9}, {k4, 6}};

  // eventbase to schedule callbacks at certain time spot
  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    for (const auto& keyVer : keyVersionAs) {
      auto val = createThriftValue(
          keyVer.second /* version */,
          node1 /* originatorId */,
          value1 /* value */);
      EXPECT_TRUE(store1->setKey(kTestingAreaName, keyVer.first, val));
    }

    for (const auto& keyVer : keyVersionBs) {
      auto val = createThriftValue(
          keyVer.second /* version */,
          node1 /* originatorId */,
          value2 /* value */);
      if (keyVer.first == k1) {
        val.value() = value1; // set same value for k1
      }
      EXPECT_TRUE(store2->setKey(kTestingAreaName, keyVer.first, val));
    }

    // Add peer ONLY for uni-direction
    EXPECT_TRUE(store1->addPeer(
        kTestingAreaName, store2->getNodeId(), store2->getPeerSpec()));
  });

  // after 3-way full-sync, we expect both A and B have:
  // (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 9, b), (k4, 6, b)
  evb.scheduleTimeout(std::chrono::milliseconds(1000), [&]() noexcept {
    for (const auto& key : allKeys) {
      auto val1 = store1->getKey(kTestingAreaName, key);
      auto val2 = store2->getKey(kTestingAreaName, key);
      EXPECT_TRUE(val1.has_value());
      EXPECT_TRUE(val2.has_value());
      EXPECT_EQ(val1->value().value(), val2->value().value());
      EXPECT_EQ(*val1->version(), *val2->version());
    }
  });

  evb.scheduleTimeout(
      std::chrono::milliseconds(1000) +
          std::chrono::milliseconds(Constants::kCounterSubmitInterval),
      [&]() noexcept {
        auto counters = facebook::fb303::fbData->getCounters();

        // check key existence
        ASSERT_EQ(1, counters.count("kvstore.thrift.num_full_sync.count"));
        ASSERT_EQ(
            1, counters.count("kvstore.thrift.num_full_sync_success.count"));
        ASSERT_EQ(
            1, counters.count("kvstore.thrift.num_full_sync_failure.count"));
        ASSERT_EQ(1, counters.count("kvstore.thrift.num_finalized_sync.count"));
        ASSERT_EQ(
            1,
            counters.count("kvstore.thrift.num_finalized_sync_success.count"));
        ASSERT_EQ(
            1,
            counters.count("kvstore.thrift.num_finalized_sync_failure.count"));

        // check key value
        EXPECT_EQ(1, counters.at("kvstore.thrift.num_full_sync.count"));
        EXPECT_EQ(1, counters.at("kvstore.thrift.num_full_sync_success.count"));
        EXPECT_EQ(0, counters.at("kvstore.thrift.num_full_sync_failure.count"));
        EXPECT_EQ(1, counters.at("kvstore.thrift.num_finalized_sync.count"));
        EXPECT_EQ(
            1, counters.at("kvstore.thrift.num_finalized_sync_success.count"));
        EXPECT_EQ(
            0, counters.at("kvstore.thrift.num_finalized_sync_failure.count"));

        evb.stop();
      });
  evb.run();

  // verify 5 keys from both stores
  EXPECT_EQ(5, store1->dumpAll(kTestingAreaName).size());
  EXPECT_EQ(5, store2->dumpAll(kTestingAreaName).size());

  auto v0 = store1->getKey(kTestingAreaName, k0);
  EXPECT_EQ(*v0->version(), 5);
  EXPECT_EQ(v0->value().value(), value1);
  auto v1 = store1->getKey(kTestingAreaName, k1);
  EXPECT_EQ(*v1->version(), 1);
  EXPECT_EQ(v1->value().value(), value1);
  auto v2 = store1->getKey(kTestingAreaName, k2);
  EXPECT_EQ(*v2->version(), 9);
  EXPECT_EQ(v2->value().value(), value1);
  auto v3 = store1->getKey(kTestingAreaName, k3);
  EXPECT_EQ(*v3->version(), 9);
  EXPECT_EQ(v3->value().value(), value2);
  auto v4 = store1->getKey(kTestingAreaName, k4);
  EXPECT_EQ(*v4->version(), 6);
  EXPECT_EQ(v4->value().value(), value2);
}

//
// Test case for flooding publication over thrift.
//
// Simple Topology:
//
// node1 <---> node2
//
// A ---> B indicates: A has B as its thrift peer
//
TEST_F(SimpleKvStoreThriftTestFixture, BasicFloodingOverThrift) {
  // create 2 nodes topology for thrift peers
  createSimpleThriftTestTopo();

  auto store1 = stores_.front();
  auto store2 = stores_.back();

  //
  // Step1: Add peer to each other's KvStore instances
  //        Expect full-sync request exchanged;
  //
  EXPECT_TRUE(store1->addPeer(
      kTestingAreaName, store2->getNodeId(), store2->getPeerSpec()));
  EXPECT_TRUE(store2->addPeer(
      kTestingAreaName, store1->getNodeId(), store1->getPeerSpec()));

  // verifying keys are exchanged between peers
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store1.get(), key2, thriftVal2, kTestingAreaName));
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store2.get(), key1, thriftVal1, kTestingAreaName));

  //
  // Step2: Inject a new key in one of the store. Make sure flooding happens
  //        and the other store have the key;
  //
  const std::string key3{"key3"};
  auto thriftVal3 =
      createThriftValue(3, store2->getNodeId(), std::string("value3"));
  EXPECT_TRUE(store2->setKey(kTestingAreaName, key3, thriftVal3));
  while (not verifyKvStoreKeyVal(
      store1.get(), key3, thriftVal3, kTestingAreaName)) {
    std::this_thread::yield();
  }

  // 3 keys from both stores
  EXPECT_EQ(3, store1->dumpAll(kTestingAreaName).size());
  EXPECT_EQ(3, store2->dumpAll(kTestingAreaName).size());
}

//
// Test case for flooding publication over thrift.
//
// Ring Topology:
//
// node1 ---> node2 ---> node3
//   ^                    |
//   |                    |
//   ----------------------
//
// 1) Inject key1 in node1;
// 2) Inject key2 in node2;
// 3) Inject key3 in node3;
// 4) Ring topology will make sure flooding is happening one-way
//    but reach global consistensy;
//
// NOTE: A ---> B indicates A has B as its thrift peer
//
TEST_F(KvStoreThriftTestFixture, RingTopoFloodingOverThrift) {
  // spin up 3 kvStore instances and thriftServers
  const std::string node1{"node-1"};
  const std::string node2{"node-2"};
  const std::string node3{"node-3"};
  const std::string key1{"key-1"};
  const std::string key2{"key-2"};
  const std::string key3{"key-3"};

  createKvStore(node1);
  auto store1 = stores_.front();

  createKvStore(node2);
  auto store2 = stores_.back();

  createKvStore(node3);
  auto store3 = stores_.back();

  // add peers
  EXPECT_TRUE(store1->addPeer(
      kTestingAreaName, store2->getNodeId(), store2->getPeerSpec()));
  EXPECT_TRUE(store2->addPeer(
      kTestingAreaName, store3->getNodeId(), store3->getPeerSpec()));
  EXPECT_TRUE(store3->addPeer(
      kTestingAreaName, store1->getNodeId(), store1->getPeerSpec()));

  LOG(INFO) << "Verify initial full-sync happening...";
  EXPECT_TRUE(verifyKvStorePeerState(
      store1.get(),
      store2->getNodeId(),
      thrift::KvStorePeerState::INITIALIZED,
      kTestingAreaName));
  EXPECT_TRUE(verifyKvStorePeerState(
      store2.get(),
      store3->getNodeId(),
      thrift::KvStorePeerState::INITIALIZED,
      kTestingAreaName));
  EXPECT_TRUE(verifyKvStorePeerState(
      store3.get(),
      store1->getNodeId(),
      thrift::KvStorePeerState::INITIALIZED,
      kTestingAreaName));
  EXPECT_EQ(0, store1->dumpAll(kTestingAreaName).size());
  EXPECT_EQ(0, store2->dumpAll(kTestingAreaName).size());
  EXPECT_EQ(0, store3->dumpAll(kTestingAreaName).size());

  LOG(INFO) << "Inject diff keys into individual store instances...";
  auto thriftVal1 =
      createThriftValue(1, store1->getNodeId(), std::string("value1"));
  auto thriftVal2 =
      createThriftValue(2, store2->getNodeId(), std::string("value2"));
  auto thriftVal3 =
      createThriftValue(3, store3->getNodeId(), std::string("value3"));
  EXPECT_TRUE(store1->setKey(kTestingAreaName, key1, thriftVal1));
  EXPECT_TRUE(store2->setKey(kTestingAreaName, key2, thriftVal2));
  EXPECT_TRUE(store3->setKey(kTestingAreaName, key3, thriftVal3));

  LOG(INFO) << "Verifying keys are exchanged between peers...";
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store1.get(), key2, thriftVal2, kTestingAreaName));
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store1.get(), key3, thriftVal3, kTestingAreaName));
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store2.get(), key1, thriftVal1, kTestingAreaName));
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store2.get(), key3, thriftVal3, kTestingAreaName));
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store3.get(), key1, thriftVal1, kTestingAreaName));
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store3.get(), key2, thriftVal2, kTestingAreaName));

  EXPECT_EQ(3, store1->dumpAll(kTestingAreaName).size());
  EXPECT_EQ(3, store2->dumpAll(kTestingAreaName).size());
  EXPECT_EQ(3, store3->dumpAll(kTestingAreaName).size());
}

TEST(KvStore, StateTransitionTest) {
  {
    auto oldState = thrift::KvStorePeerState::IDLE;

    // IDLE => SYNCING
    auto event1 = KvStorePeerEvent::PEER_ADD;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event1),
        thrift::KvStorePeerState::SYNCING);

    // IDLE => IDLE
    auto event2 = KvStorePeerEvent::THRIFT_API_ERROR;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event2),
        thrift::KvStorePeerState::IDLE);

    // IDLE => IDLE
    auto event3 = KvStorePeerEvent::INCONSISTENCY_DETECTED;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event3),
        thrift::KvStorePeerState::IDLE);
  }

  {
    auto oldState = thrift::KvStorePeerState::SYNCING;

    // SYNCING => INITIALIZED
    auto event1 = KvStorePeerEvent::SYNC_RESP_RCVD;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event1),
        thrift::KvStorePeerState::INITIALIZED);

    // SYNCING => IDLE
    auto event2 = KvStorePeerEvent::THRIFT_API_ERROR;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event2),
        thrift::KvStorePeerState::IDLE);

    // SYNCING => IDLE
    auto event3 = KvStorePeerEvent::INCONSISTENCY_DETECTED;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event3),
        thrift::KvStorePeerState::IDLE);
  }

  {
    auto oldState = thrift::KvStorePeerState::INITIALIZED;

    // INITIALIZED => INITIIALIZED
    auto event1 = KvStorePeerEvent::SYNC_RESP_RCVD;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event1),
        thrift::KvStorePeerState::INITIALIZED);

    // INITIALIZED => IDLE
    auto event2 = KvStorePeerEvent::THRIFT_API_ERROR;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event2),
        thrift::KvStorePeerState::IDLE);

    // INITIALIZED => IDLE
    auto event3 = KvStorePeerEvent::INCONSISTENCY_DETECTED;
    EXPECT_EQ(
        KvStoreDb<thrift::KvStoreServiceAsyncClient>::getNextState(
            oldState, event3),
        thrift::KvStorePeerState::IDLE);
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
