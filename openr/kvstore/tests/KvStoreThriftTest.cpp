/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sodium.h>
#include <thread>

#include <fbzmq/zmq/Zmq.h>
#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

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

    // tear down thrift server
    for (auto& thriftServer : thriftServers_) {
      thriftServer->stop();
      thriftServer.reset();
    }

    // tear down kvStore instances
    for (auto& store : stores_) {
      store->stop();
      store.reset();
    }

    // clean up vector content
    thriftServers_.clear();
    stores_.clear();
  }

  void
  createKvStore(const std::string& nodeId) {
    auto tConfig = getBasicOpenrConfig(nodeId);
    stores_.emplace_back(std::make_shared<KvStoreWrapper>(
        context_,
        std::make_shared<Config>(tConfig),
        std::nullopt,
        true /* enable_kvstore_thrift */));
    stores_.back()->run();
  }

  void
  createThriftServer(
      const std::string& nodeId, std::shared_ptr<KvStoreWrapper> store) {
    thriftServers_.emplace_back(std::make_shared<OpenrThriftServerWrapper>(
        nodeId,
        nullptr, // decision
        nullptr, // fib
        store->getKvStore(), // kvStore
        nullptr, // link-monitor
        nullptr, // monitor
        nullptr, // config-store
        nullptr, // prefixManager
        nullptr, // spark
        nullptr // config
        ));
    thriftServers_.back()->run();
  }

  bool
  verifyKvStoreKeyVal(
      KvStoreWrapper* kvStore,
      const std::string& key,
      const thrift::Value& thriftVal,
      const AreaId& area,
      std::optional<std::chrono::milliseconds> processingTimeout =
          Constants::kPlatformRoutesProcTimeout) noexcept {
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
      KvStoreWrapper* kvStore,
      const std::string& peerName,
      KvStorePeerState expPeerState,
      const AreaId& area,
      std::optional<std::chrono::milliseconds> processingTimeout =
          Constants::kPlatformRoutesProcTimeout) noexcept {
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > processingTimeout.value()) {
        LOG(ERROR)
            << "Timeout verifying state: " << KvStoreDb::toStr(expPeerState)
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

  // zmqContext
  fbzmq::Context context_;

  // initialize maximum waiting time to check key-val:
  const std::chrono::milliseconds waitTime_{1000};

  // vector of KvStores created
  std::vector<std::shared_ptr<KvStoreWrapper>> stores_{};

  // vector of ThriftServers created
  std::vector<std::shared_ptr<OpenrThriftServerWrapper>> thriftServers_{};
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
    createThriftServer(node1, stores_.back());

    // spin up another kvStore instance and thriftServer
    createKvStore(node2);
    createThriftServer(node2, stores_.back());

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
  auto peerSpec1 = createPeerSpec(
      "inproc://dummy-spec-1", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      thriftServers_.back()->getOpenrCtrlThriftPort());
  auto peerSpec2 = createPeerSpec(
      "inproc://dummy-spec-2", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      thriftServers_.front()->getOpenrCtrlThriftPort());
  auto store1 = stores_.front();
  auto store2 = stores_.back();

  // eventbase to schedule callbacks at certain time spot
  OpenrEventBase evb;
  evb.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    //
    // Step1: Add peer to each other's KvStore instances
    //        Expect full-sync request exchanged;
    //
    EXPECT_TRUE(
        store1->addPeer(kTestingAreaName, store2->getNodeId(), peerSpec1));
    EXPECT_TRUE(
        store2->addPeer(kTestingAreaName, store1->getNodeId(), peerSpec2));

    // dump peers to make sure they are aware of each other
    std::unordered_map<std::string, thrift::PeerSpec> expPeer1_1 = {
        {store2->getNodeId(), peerSpec1}};
    std::unordered_map<std::string, thrift::PeerSpec> expPeer2_1 = {
        {store1->getNodeId(), peerSpec2}};
    EXPECT_EQ(expPeer1_1, store1->getPeers(kTestingAreaName));
    EXPECT_EQ(expPeer2_1, store2->getPeers(kTestingAreaName));

    // verifying keys are exchanged between peers
    EXPECT_TRUE(verifyKvStorePeerState(
        store1.get(),
        store2->getNodeId(),
        KvStorePeerState::INITIALIZED,
        kTestingAreaName));
    EXPECT_TRUE(verifyKvStorePeerState(
        store2.get(),
        store1->getNodeId(),
        KvStorePeerState::INITIALIZED,
        kTestingAreaName));

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
    thriftServers_.back()->stop();
    thriftServers_.back().reset();
    thriftServers_.pop_back();
    stores_.back()->stop();
    stores_.back().reset();
    stores_.pop_back();
  });

  // ATTN: give 1000ms margin before recreate kvstore instance to avoid
  //       situation of INPROC_URL of ZMQ still being occupied
  evb.scheduleTimeout(std::chrono::milliseconds(1000), [&]() noexcept {
    // recreate store2 and corresponding thriftServer
    createKvStore(node2);
    createThriftServer(node2, stores_.back());

    store2 = stores_.back();
    auto newPeerSpec = createPeerSpec(
        "inproc://dummy-spec-2", // TODO: remove dummy url once zmq deprecated
        Constants::kPlatformHost.toString(),
        thriftServers_.back()->getOpenrCtrlThriftPort());
    std::unordered_map<std::string, thrift::PeerSpec> newExpPeer = {
        {store2->getNodeId(), newPeerSpec}};

    // TODO: add counter verification for state change to IDLE
    EXPECT_TRUE(
        store1->addPeer(kTestingAreaName, store2->getNodeId(), newPeerSpec));
    EXPECT_EQ(newExpPeer, store1->getPeers(kTestingAreaName));

    // verify another full-sync request being sent
    EXPECT_TRUE(verifyKvStorePeerState(
        store1.get(),
        store2->getNodeId(),
        KvStorePeerState::INITIALIZED,
        kTestingAreaName));
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

  // create dummy port in purpose to mimick exception connecting thrift server
  // ATTN: explicitly make sure dummy port used will be different to thrift
  // server ports
  std::unordered_set<uint16_t> usedPorts{
      thriftServers_.front()->getOpenrCtrlThriftPort(),
      thriftServers_.back()->getOpenrCtrlThriftPort()};
  const uint16_t dummyPort1 = generateRandomDiffPort(usedPorts);
  const uint16_t dummyPort2 = generateRandomDiffPort(usedPorts);

  // build peerSpec for thrift client connection
  auto peerSpec1 = createPeerSpec(
      "inproc://dummy-spec-1", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      dummyPort1);
  auto peerSpec2 = createPeerSpec(
      "inproc://dummy-spec-2", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      dummyPort2);
  auto store1 = stores_.front();
  auto store2 = stores_.back();

  EXPECT_TRUE(
      store1->addPeer(kTestingAreaName, store2->getNodeId(), peerSpec1));
  EXPECT_TRUE(
      store2->addPeer(kTestingAreaName, store1->getNodeId(), peerSpec2));

  // verifying keys are NOT exchanged between peers
  EXPECT_FALSE(verifyKvStoreKeyVal(
      store1.get(), key2, thriftVal2, kTestingAreaName, waitTime_));
  EXPECT_FALSE(verifyKvStoreKeyVal(
      store2.get(), key1, thriftVal1, kTestingAreaName, waitTime_));

  // verify no initial sync event
  EXPECT_EQ(0, store1->getInitialSyncEventsReader().size());
  EXPECT_EQ(0, store2->getInitialSyncEventsReader().size());
  EXPECT_EQ(1, store1->dumpAll(kTestingAreaName).size());
  EXPECT_EQ(1, store2->dumpAll(kTestingAreaName).size());
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
  auto store1 = stores_.back();
  createThriftServer(node1, store1);

  createKvStore(node2);
  auto store2 = stores_.back();
  createThriftServer(node2, store2);

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
      // keyValMapA.emplace(keyVer.first, val);
    }

    for (const auto& keyVer : keyVersionBs) {
      auto val = createThriftValue(
          keyVer.second /* version */,
          node1 /* originatorId */,
          value2 /* value */);
      if (keyVer.first == k1) {
        val.value_ref() = value1; // set same value for k1
      }
      EXPECT_TRUE(store2->setKey(kTestingAreaName, keyVer.first, val));
      // keyValMapB.emplace(keyVer.first, val);
    }

    // Add peer ONLY for uni-direction
    auto peerSpec1 = createPeerSpec(
        "inproc://dummy-spec-1", // TODO: remove dummy url once zmq deprecated
        Constants::kPlatformHost.toString(),
        thriftServers_.back()->getOpenrCtrlThriftPort());
    EXPECT_TRUE(
        store1->addPeer(kTestingAreaName, store2->getNodeId(), peerSpec1));
  });

  // after 3-way full-sync, we expect both A and B have:
  // (k0, 5, a), (k1, 1, a), (k2, 9, a), (k3, 9, b), (k4, 6, b)
  evb.scheduleTimeout(std::chrono::milliseconds(1000), [&]() noexcept {
    for (const auto& key : allKeys) {
      auto val1 = store1->getKey(kTestingAreaName, key);
      auto val2 = store2->getKey(kTestingAreaName, key);
      EXPECT_TRUE(val1.has_value());
      EXPECT_TRUE(val2.has_value());
      EXPECT_EQ(val1->value_ref().value(), val2->value_ref().value());
      EXPECT_EQ(*val1->version_ref(), *val2->version_ref());
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
  EXPECT_EQ(*v0->version_ref(), 5);
  EXPECT_EQ(v0->value_ref().value(), value1);
  auto v1 = store1->getKey(kTestingAreaName, k1);
  EXPECT_EQ(*v1->version_ref(), 1);
  EXPECT_EQ(v1->value_ref().value(), value1);
  auto v2 = store1->getKey(kTestingAreaName, k2);
  EXPECT_EQ(*v2->version_ref(), 9);
  EXPECT_EQ(v2->value_ref().value(), value1);
  auto v3 = store1->getKey(kTestingAreaName, k3);
  EXPECT_EQ(*v3->version_ref(), 9);
  EXPECT_EQ(v3->value_ref().value(), value2);
  auto v4 = store1->getKey(kTestingAreaName, k4);
  EXPECT_EQ(*v4->version_ref(), 6);
  EXPECT_EQ(v4->value_ref().value(), value2);
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

  // build peerSpec for thrift peer connection
  auto peerSpec1 = createPeerSpec(
      "inproc://dummy-spec-1", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      thriftServers_.back()->getOpenrCtrlThriftPort());
  auto peerSpec2 = createPeerSpec(
      "inproc://dummy-spec-2", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      thriftServers_.front()->getOpenrCtrlThriftPort());
  auto store1 = stores_.front();
  auto store2 = stores_.back();

  //
  // Step1: Add peer to each other's KvStore instances
  //        Expect full-sync request exchanged;
  //
  EXPECT_TRUE(
      store1->addPeer(kTestingAreaName, store2->getNodeId(), peerSpec1));
  EXPECT_TRUE(
      store2->addPeer(kTestingAreaName, store1->getNodeId(), peerSpec2));

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
  EXPECT_TRUE(
      verifyKvStoreKeyVal(store1.get(), key3, thriftVal3, kTestingAreaName));

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
// A ---> B indicates: A has B as its thrift peer
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
  auto store1 = stores_.back();
  createThriftServer(node1, store1);
  auto thriftServer1 = thriftServers_.back();

  createKvStore(node2);
  auto store2 = stores_.back();
  createThriftServer(node2, store2);
  auto thriftServer2 = thriftServers_.back();

  createKvStore(node3);
  auto store3 = stores_.back();
  createThriftServer(node3, store3);
  auto thriftServer3 = thriftServers_.back();

  // add peers
  auto peerSpec1 = createPeerSpec(
      "inproc://dummy-spec-1", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      thriftServer2->getOpenrCtrlThriftPort());
  auto peerSpec2 = createPeerSpec(
      "inproc://dummy-spec-2", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      thriftServer3->getOpenrCtrlThriftPort());
  auto peerSpec3 = createPeerSpec(
      "inproc://dummy-spec-3", // TODO: remove dummy url once zmq deprecated
      Constants::kPlatformHost.toString(),
      thriftServer1->getOpenrCtrlThriftPort());

  LOG(INFO) << "KvStore instances add thrift peers...";
  EXPECT_TRUE(
      store1->addPeer(kTestingAreaName, store2->getNodeId(), peerSpec1));
  EXPECT_TRUE(
      store2->addPeer(kTestingAreaName, store3->getNodeId(), peerSpec2));
  EXPECT_TRUE(
      store3->addPeer(kTestingAreaName, store1->getNodeId(), peerSpec3));

  LOG(INFO) << "Verify initial full-sync happening...";
  EXPECT_TRUE(verifyKvStorePeerState(
      store1.get(),
      store2->getNodeId(),
      KvStorePeerState::INITIALIZED,
      kTestingAreaName));
  EXPECT_TRUE(verifyKvStorePeerState(
      store2.get(),
      store3->getNodeId(),
      KvStorePeerState::INITIALIZED,
      kTestingAreaName));
  EXPECT_TRUE(verifyKvStorePeerState(
      store3.get(),
      store1->getNodeId(),
      KvStorePeerState::INITIALIZED,
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
    // IDLE => SYNCING
    auto oldState = KvStorePeerState::IDLE;
    auto event = KvStorePeerEvent::PEER_ADD;
    auto newState = KvStoreDb::getNextState(oldState, event);

    EXPECT_EQ(newState, KvStorePeerState::SYNCING);
  }

  {
    // SYNCING => INITIALIZED
    auto oldState = KvStorePeerState::SYNCING;
    auto event = KvStorePeerEvent::SYNC_RESP_RCVD;
    auto newState = KvStoreDb::getNextState(oldState, event);

    EXPECT_EQ(newState, KvStorePeerState::INITIALIZED);
  }

  {
    // SYNCING => IDLE
    auto oldState = KvStorePeerState::SYNCING;
    auto event = KvStorePeerEvent::THRIFT_API_ERROR;
    auto newState = KvStoreDb::getNextState(oldState, event);

    EXPECT_EQ(newState, KvStorePeerState::IDLE);
  }

  {
    // INITIALIZED => IDLE
    // INITIALIZED => INITIIALIZED
    auto oldState = KvStorePeerState::INITIALIZED;
    auto event1 = KvStorePeerEvent::SYNC_RESP_RCVD;
    auto newState1 = KvStoreDb::getNextState(oldState, event1);
    auto event2 = KvStorePeerEvent::THRIFT_API_ERROR;
    auto newState2 = KvStoreDb::getNextState(newState1, event2);

    EXPECT_EQ(newState1, KvStorePeerState::INITIALIZED);
    EXPECT_EQ(newState2, KvStorePeerState::IDLE);
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // Run the tests
  auto rc = RUN_ALL_TESTS();

  return rc;
}
