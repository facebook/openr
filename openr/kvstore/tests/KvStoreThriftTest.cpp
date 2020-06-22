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
        std::unordered_map<std::string, thrift::PeerSpec>{},
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
        nullptr, // config-store
        nullptr, // prefixManager
        nullptr, // config
        MonitorSubmitUrl{"inproc://monitor_submit"},
        context_));
    thriftServers_.back()->run();
  }

  bool
  verifyKvStoreKeyVal(
      KvStoreWrapper* kvStore,
      const std::string& key,
      const thrift::Value& thriftVal,
      const std::string& area = thrift::KvStore_constants::kDefaultArea(),
      std::optional<std::chrono::milliseconds> processingTimeout =
          Constants::kPlatformRoutesProcTimeout) noexcept {
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
      // check i it is over procTimeout
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > processingTimeout.value()) {
        LOG(ERROR) << "Timeout verifying key: " << key
                   << " inside KvStore: " << kvStore->getNodeId();
        break;
      }
      auto val = kvStore->getKey(key, area);
      if (val.has_value() and val.value() == thriftVal) {
        return true;
      }

      // yield to avoid hogging the process
      std::this_thread::yield();
    }
    return false;
  }

  // zmqContext
  fbzmq::Context context_;

  // local address
  const std::string localhost_{"::1"};

  // maximum waiting time to check key-val
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
    EXPECT_TRUE(stores_.front()->setKey(key1, thriftVal1));
    EXPECT_TRUE(stores_.back()->setKey(key2, thriftVal2));

    // check key ONLY exists in one store, not the other
    EXPECT_TRUE(stores_.front()->getKey(key1).has_value());
    EXPECT_FALSE(stores_.back()->getKey(key1).has_value());
    EXPECT_FALSE(stores_.front()->getKey(key2).has_value());
    EXPECT_TRUE(stores_.back()->getKey(key2).has_value());
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
      localhost_,
      thriftServers_.back()->getOpenrCtrlThriftPort());
  auto peerSpec2 = createPeerSpec(
      "inproc://dummy-spec-2", // TODO: remove dummy url once zmq deprecated
      localhost_,
      thriftServers_.front()->getOpenrCtrlThriftPort());
  auto store1 = stores_.front();
  auto store2 = stores_.back();

  EXPECT_TRUE(store1->addPeer(store2->getNodeId(), peerSpec1));
  EXPECT_TRUE(store2->addPeer(store1->getNodeId(), peerSpec2));

  // dump peers to make sure they are aware of each other
  std::unordered_map<std::string, thrift::PeerSpec> expPeer1_1 = {
      {store2->getNodeId(), peerSpec1}};
  std::unordered_map<std::string, thrift::PeerSpec> expPeer2_1 = {
      {store1->getNodeId(), peerSpec2}};
  EXPECT_EQ(expPeer1_1, store1->getPeers());
  EXPECT_EQ(expPeer2_1, store2->getPeers());

  // verifying keys are exchanged between peers
  EXPECT_TRUE(verifyKvStoreKeyVal(store1.get(), key2, thriftVal2));
  EXPECT_TRUE(verifyKvStoreKeyVal(store2.get(), key1, thriftVal1));

  EXPECT_EQ(2, store1->dumpAll().size());
  EXPECT_EQ(2, store2->dumpAll().size());

  // remove peers
  EXPECT_TRUE(store1->delPeer(store2->getNodeId()));
  EXPECT_TRUE(store2->delPeer(store1->getNodeId()));

  std::unordered_map<std::string, thrift::PeerSpec> expPeer1_2{};
  std::unordered_map<std::string, thrift::PeerSpec> expPeer2_2{};
  EXPECT_EQ(expPeer1_2, store1->getPeers());
  EXPECT_EQ(expPeer2_2, store2->getPeers());
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
      localhost_,
      dummyPort1);
  auto peerSpec2 = createPeerSpec(
      "inproc://dummy-spec-2", // TODO: remove dummy url once zmq deprecated
      localhost_,
      dummyPort2);
  auto store1 = stores_.front();
  auto store2 = stores_.back();

  EXPECT_TRUE(store1->addPeer(store2->getNodeId(), peerSpec1));
  EXPECT_TRUE(store2->addPeer(store1->getNodeId(), peerSpec2));

  // verifying keys are exchanged between peers
  EXPECT_FALSE(verifyKvStoreKeyVal(
      store1.get(),
      key2,
      thriftVal2,
      thrift::KvStore_constants::kDefaultArea(),
      waitTime_));
  EXPECT_FALSE(verifyKvStoreKeyVal(
      store2.get(),
      key1,
      thriftVal1,
      thrift::KvStore_constants::kDefaultArea(),
      waitTime_));

  EXPECT_EQ(1, store1->dumpAll().size());
  EXPECT_EQ(1, store2->dumpAll().size());
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
    auto event1 = KvStorePeerEvent::SYNC_TIMEOUT;
    auto newState1 = KvStoreDb::getNextState(oldState, event1);
    auto event2 = KvStorePeerEvent::THRIFT_API_ERROR;
    auto newState2 = KvStoreDb::getNextState(oldState, event2);

    EXPECT_EQ(newState1, KvStorePeerState::IDLE);
    EXPECT_EQ(newState2, KvStorePeerState::IDLE);
  }

  {
    // INITIALIZED => IDLE
    auto oldState = KvStorePeerState::INITIALIZED;
    auto event1 = KvStorePeerEvent::SYNC_TIMEOUT;
    auto newState1 = KvStoreDb::getNextState(oldState, event1);
    auto event2 = KvStorePeerEvent::THRIFT_API_ERROR;
    auto newState2 = KvStoreDb::getNextState(oldState, event2);
    auto event3 = KvStorePeerEvent::SYNC_RESP_RCVD;
    auto newState3 = KvStoreDb::getNextState(oldState, event3);

    EXPECT_EQ(newState1, KvStorePeerState::IDLE);
    EXPECT_EQ(newState2, KvStorePeerState::IDLE);
    EXPECT_EQ(newState3, KvStorePeerState::INITIALIZED);
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
