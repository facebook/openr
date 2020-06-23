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

namespace {
// ttl used in test for (K,V) pair
const std::chrono::milliseconds kTtl{1000};
} // namespace

class MultipleKvStoreTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // define/start eventbase thread
    evbThread = std::thread([&]() { evb.run(); });

    auto makeStoreWrapper = [this](std::string nodeId) {
      auto tConfig = getBasicOpenrConfig(nodeId);
      config_ = std::make_shared<Config>(tConfig);
      return std::make_shared<KvStoreWrapper>(context_, config_);
    };

    auto makeThriftServerWrapper =
        [this](std::string nodeId, std::shared_ptr<KvStoreWrapper> store) {
          return std::make_shared<OpenrThriftServerWrapper>(
              nodeId,
              nullptr /* decision */,
              nullptr /* fib */,
              store->getKvStore() /* kvStore */,
              nullptr /* linkMonitor */,
              nullptr /* configStore */,
              nullptr /* prefixManager */,
              nullptr /* config */,
              MonitorSubmitUrl{"inproc://monitor_submit"},
              context_);
        };

    // spin up kvStore through kvStoreWrapper
    kvStoreWrapper1_ = makeStoreWrapper(nodeId1_);
    kvStoreWrapper2_ = makeStoreWrapper(nodeId2_);

    kvStoreWrapper1_->run();
    kvStoreWrapper2_->run();

    // spin up OpenrThriftServerWrapper
    thriftServer1_ = makeThriftServerWrapper(nodeId1_, kvStoreWrapper1_);
    thriftServer2_ = makeThriftServerWrapper(nodeId2_, kvStoreWrapper2_);

    thriftServer1_->run();
    thriftServer2_->run();
  }

  void
  TearDown() override {
    if (thriftServer1_) {
      thriftServer1_->stop();
      thriftServer1_.reset();
    }
    if (thriftServer2_) {
      thriftServer2_->stop();
      thriftServer2_.reset();
    }

    // ATTN: kvStoreUpdatesQueue must be closed before destructing
    //       KvStoreClientInternal as fiber future is depending on RQueue
    kvStoreWrapper1_->stop();
    kvStoreWrapper1_.reset();
    kvStoreWrapper2_->stop();
    kvStoreWrapper2_.reset();

    // ATTN: Destroy client before destroy evb. Otherwise, destructor will
    //       FOREVER waiting fiber future to be fulfilled.
    client1.reset();
    client2.reset();

    evb.stop();
    evb.waitUntilStopped();
    evbThread.join();
  }

  // var used to conmmunicate to kvStore through openrCtrl thrift server
  const std::string nodeId1_{"test_1"};
  const std::string nodeId2_{"test_2"};
  const std::string localhost_{"::1"};

  fbzmq::Context context_{};
  apache::thrift::CompactSerializer serializer;

  OpenrEventBase evb;
  std::thread evbThread;

  std::shared_ptr<Config> config_{nullptr};
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper1_{nullptr},
      kvStoreWrapper2_{nullptr};
  std::shared_ptr<OpenrThriftServerWrapper> thriftServer1_{nullptr},
      thriftServer2_{nullptr};
  std::shared_ptr<KvStoreClientInternal> client1{nullptr}, client2{nullptr};
};

TEST_F(MultipleKvStoreTestFixture, dumpAllTest) {
  const std::string key1{"test_key1"};
  const std::string key2{"test_key2"};
  const std::string prefix = "";
  const uint16_t port1 = thriftServer1_->getOpenrCtrlThriftPort();
  const uint16_t port2 = thriftServer2_->getOpenrCtrlThriftPort();

  std::vector<folly::SocketAddress> sockAddrs;
  sockAddrs.push_back(folly::SocketAddress{localhost_, port1});
  sockAddrs.push_back(folly::SocketAddress{localhost_, port2});

  // Step1: verify there is NOTHING inside kvStore instances
  auto preDb = dumpAllWithThriftClientFromMultiple(sockAddrs, prefix);
  EXPECT_TRUE(preDb.first.has_value());
  EXPECT_TRUE(preDb.first.value().empty());
  EXPECT_TRUE(preDb.second.empty());

  evb.getEvb()->runInEventBaseThreadAndWait([&]() noexcept {
    // Step2: initilize kvStoreClient connecting to different thriftServers
    client1 = std::make_shared<KvStoreClientInternal>(
        &evb, nodeId1_, kvStoreWrapper1_->getKvStore());
    client2 = std::make_shared<KvStoreClientInternal>(
        &evb, nodeId2_, kvStoreWrapper2_->getKvStore());
    EXPECT_TRUE(nullptr != client1);
    EXPECT_TRUE(nullptr != client2);

    // Step3: insert (k1, v1) and (k2, v2) to different openrCtrlWrapper server
    thrift::Value value;
    value.version = 1;
    {
      value.value_ref() = "test_value1";
      EXPECT_TRUE(client1->setKey(
          key1, fbzmq::util::writeThriftObjStr(value, serializer), 100));
    }
    {
      value.value_ref() = "test_value2";
      EXPECT_TRUE(client2->setKey(
          key2, fbzmq::util::writeThriftObjStr(value, serializer), 200));
    }
  });

  // Step4: verify we can fetch 2 keys from different servers as aggregation
  // result
  {
    auto postDb = dumpAllWithThriftClientFromMultiple(sockAddrs, prefix);
    ASSERT_TRUE(postDb.first.has_value());
    auto pub = postDb.first.value();
    EXPECT_TRUE(pub.size() == 2);
    EXPECT_TRUE(pub.count(key1));
    EXPECT_TRUE(pub.count(key2));
  }

  // Step5: verify dumpAllWithPrefixMultipleAndParse API
  {
    auto maybe =
        dumpAllWithPrefixMultipleAndParse<thrift::Value>(sockAddrs, "test_");
    ASSERT_TRUE(maybe.first.has_value());
    auto pub = maybe.first.value();
    EXPECT_EQ(2, pub.size());
    EXPECT_EQ("test_value1", pub[key1].value_ref());
    EXPECT_EQ("test_value2", pub[key2].value_ref());
  }

  // Step6: shutdown both thriftSevers and verify
  // dumpAllWithThriftClientFromMultiple() will get nothing.
  {
    // ATTN: kvStoreUpdatesQueue must be closed before destructing
    //       KvStoreClientInternal as fiber future is depending on RQueue
    kvStoreWrapper1_->closeQueue();
    kvStoreWrapper2_->closeQueue();

    thriftServer1_->stop();
    thriftServer1_.reset();
    thriftServer2_->stop();
    thriftServer2_.reset();

    auto db = dumpAllWithThriftClientFromMultiple(sockAddrs, prefix);
    ASSERT_TRUE(db.first.has_value());
    ASSERT_TRUE(db.first.value().empty());
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
