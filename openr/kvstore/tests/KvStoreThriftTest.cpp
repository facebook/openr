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

  std::shared_ptr<KvStoreWrapper>
  createKvStore(const std::string& nodeId) {
    auto tConfig = getBasicOpenrConfig(nodeId);
    stores_.emplace_back(std::make_shared<KvStoreWrapper>(
        context_,
        std::make_shared<Config>(tConfig),
        std::unordered_map<std::string, thrift::PeerSpec>{},
        std::nullopt,
        true /* enable_kvstore_thrift */));
    return stores_.back();
  }

  std::shared_ptr<OpenrThriftServerWrapper>
  createThriftServer(
      const std::string& nodeId, std::shared_ptr<KvStoreWrapper> store) {
    thriftServers_.emplace_back(std::make_shared<OpenrThriftServerWrapper>(
        nodeId,
        nullptr, // decision
        nullptr, // fib
        store->getKvStore(), // kvstore
        nullptr, // link-monitor
        nullptr, // config-store
        nullptr, // prefixManager
        nullptr, // config
        MonitorSubmitUrl{"inproc://monitor_submit"},
        context_));
    return thriftServers_.back();
  }

  // zmqContext
  fbzmq::Context context_;

  // local address
  const std::string localhost_{"::1"};

  // vector of KvStores created
  std::vector<std::shared_ptr<KvStoreWrapper>> stores_{};

  // vector of ThriftServers created
  std::vector<std::shared_ptr<OpenrThriftServerWrapper>> thriftServers_{};
};

TEST_F(KvStoreThriftTestFixture, thriftClientConnection) {
  const std::string node1{"node-1"};
  const std::string node2{"node-2"};

  // spin up kvStore instances through kvStoreWrapper
  auto store1 = createKvStore(node1);
  auto store2 = createKvStore(node2);
  store1->run();
  store2->run();

  // spin up OpenrThriftServer
  auto thriftServer1 = createThriftServer(node1, store1);
  auto thriftServer2 = createThriftServer(node2, store2);
  thriftServer1->run();
  thriftServer2->run();

  // retrieve port from thriftServer for peer connection
  const uint16_t port1 = thriftServer1->getOpenrCtrlThriftPort();
  const uint16_t port2 = thriftServer2->getOpenrCtrlThriftPort();

  // build peerSpec for thrift client connection
  auto peerSpec1 = createPeerSpec("inproc://dummy-spec-1", localhost_, port2);
  auto peerSpec2 = createPeerSpec("inproc://dummy-spec-2", localhost_, port1);

  // add peers for both stores respectively
  EXPECT_TRUE(store1->addPeer(store2->nodeId, peerSpec1));
  EXPECT_TRUE(store2->addPeer(store1->nodeId, peerSpec2));

  // dump peers to make sure they are aware of each other
  std::unordered_map<std::string, thrift::PeerSpec> expPeer1_1 = {
      {store2->nodeId, peerSpec1}};
  std::unordered_map<std::string, thrift::PeerSpec> expPeer2_1 = {
      {store1->nodeId, peerSpec2}};
  EXPECT_EQ(expPeer1_1, store1->getPeers());
  EXPECT_EQ(expPeer2_1, store2->getPeers());

  // remove peers
  EXPECT_TRUE(store1->delPeer(store2->nodeId));
  EXPECT_TRUE(store2->delPeer(store1->nodeId));

  std::unordered_map<std::string, thrift::PeerSpec> expPeer1_2{};
  std::unordered_map<std::string, thrift::PeerSpec> expPeer2_2{};
  EXPECT_EQ(expPeer1_2, store1->getPeers());
  EXPECT_EQ(expPeer2_2, store2->getPeers());
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
