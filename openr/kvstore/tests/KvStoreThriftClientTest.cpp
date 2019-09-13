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

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/OpenrThriftServerWrapper.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

using namespace openr;
using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;

namespace {
// ttl used in test for (K,V) pair
const std::chrono::milliseconds kTtl{1000};
} // namespace

class KvStoreThriftClientTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // spin up a kvStore through kvStoreWrapper
    kvStoreWrapper_ = std::make_shared<KvStoreWrapper>(
        context_,
        nodeId_,
        std::chrono::seconds(60) /* db sync interval */,
        std::chrono::seconds(600) /* counter submit interval */,
        std::unordered_map<std::string, thrift::PeerSpec>{});
    kvStoreWrapper_->run();

    // spin up an openrThriftServer
    openrThriftServerWrapper_ = std::make_shared<OpenrThriftServerWrapper>(
        nodeId_,
        MonitorSubmitUrl{"inproc://monitor_submit"},
        KvStoreLocalPubUrl{kvStoreWrapper_->localPubUrl},
        context_);
    openrThriftServerWrapper_->addModuleType(
        thrift::OpenrModuleType::KVSTORE, kvStoreWrapper_->getKvStore());
    openrThriftServerWrapper_->run();
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping openrCtrl thrift server thread";
    openrThriftServerWrapper_->stop();
    LOG(INFO) << "OpenrCtrl thrift server thread got stopped";

    LOG(INFO) << "Stopping KvStoreWrapper thread";
    kvStoreWrapper_->stop();
    LOG(INFO) << "KvStoreWrapper thread got stopped";
  }

  // var used to conmmunicate to kvStore through openrCtrl thrift server
  const std::string nodeId_{"test_kvstore_thrift"};
  const std::string localhost_{"::1"};

  fbzmq::Context context_{};
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper_{nullptr};
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper_{nullptr};
};

TEST_F(KvStoreThriftClientTestFixture, ApiTest) {
  // Create another ZmqEventLoop instance for looping clients
  fbzmq::ZmqEventLoop evl;

  const std::string key1{"test_key1"};
  const std::string val1{"test_value1"};
  const std::string key2{"test_key2"};
  const std::string val2{"test_value2"};
  const uint16_t port = openrThriftServerWrapper_->getOpenrCtrlThriftPort();

  // Create and initilize kvStoreThriftClient
  auto client1 = std::make_shared<KvStoreClient>(
      context_, &evl, nodeId_, folly::SocketAddress{localhost_, port});
  auto client2 = std::make_shared<KvStoreClient>(
      context_, &evl, nodeId_, folly::SocketAddress{localhost_, port});
  EXPECT_TRUE(nullptr != client1);
  EXPECT_TRUE(nullptr != client2);

  // Test1: test getKey()/setKey() with one client when there is no such key
  evl.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    // client1 get key, should be no key inside kvStore
    auto maybeValue1 = client1->getKey(key1);
    ASSERT_FALSE(maybeValue1.hasValue());

    // client1 set key-value
    EXPECT_TRUE(client1->setKey(key1, val1));

    auto maybeValue2 = client1->getKey(key1);
    ASSERT_TRUE(maybeValue2.hasValue());
    EXPECT_EQ(1, maybeValue2->version);
    EXPECT_EQ(val1, maybeValue2->value.value());
  });

  // Test2: test getKey()/setKey() with the other client. Verift version bump
  evl.scheduleTimeout(std::chrono::milliseconds(1), [&]() noexcept {
    // use client2 to get key, should see excatly the same as client1
    auto maybeValue3 = client2->getKey(key1);
    ASSERT_TRUE(maybeValue3.hasValue());
    EXPECT_EQ(1, maybeValue3->version);
    EXPECT_EQ(val1, maybeValue3->value.value());

    // use client2 to set key, should see version bumped up
    const std::string newValue{"test_value1_new"};
    EXPECT_TRUE(client2->setKey(key1, newValue));

    auto maybeValue4 = client2->getKey(key1);
    ASSERT_TRUE(maybeValue4.hasValue());
    EXPECT_EQ(2, maybeValue4->version);
    EXPECT_EQ(newValue, maybeValue4->value.value());
  });

  // Test3: inject keys with non-infinite TTL
  evl.scheduleTimeout(std::chrono::milliseconds(2), [&]() noexcept {
    EXPECT_TRUE(client1->setKey(key2, val2, 3, kTtl));
  });

  // Test4: key shall NOT expire even after TTL time due to continuous
  // refreshing
  evl.scheduleTimeout(std::chrono::milliseconds(3) + kTtl * 3, [&]() noexcept {
    // check key is NOT expiring
    auto maybeValue5 = client2->getKey(key2);
    ASSERT_TRUE(maybeValue5.hasValue());
    EXPECT_EQ(3, maybeValue5->version);
    EXPECT_EQ(val2, maybeValue5->value.value());
    EXPECT_LT(0, maybeValue5->ttlVersion);

    // nuke client to mimick scenario user process dies and no ttl update
    client1 = nullptr;
    client2 = nullptr;
  });

  evl.scheduleTimeout(std::chrono::milliseconds(4) + kTtl * 6, [&]() noexcept {
    // Verify key-value info
    const auto keyValResponse = kvStoreWrapper_->dumpAll();
    ASSERT_EQ(1, keyValResponse.size()); // (key2, val2) expired!!!

    auto const& value1 = keyValResponse.at(key1);
    EXPECT_EQ("test_value1_new", value1.value);
    EXPECT_EQ(2, value1.version);

    // stop the event loop
    evl.stop();
  });

  // Start event loop and wait until it finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "Starting main eventloop.";
    evl.run();
    LOG(INFO) << "Main eventloop terminated.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();
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
