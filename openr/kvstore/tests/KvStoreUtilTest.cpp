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
    // spin up a kvStore through kvStoreWrapper
    kvStoreWrapper1_ = std::make_shared<KvStoreWrapper>(
        context_,
        nodeId1_,
        std::chrono::seconds(60), // db sync interval
        std::chrono::seconds(600), // counter submit interval,
        std::unordered_map<std::string, thrift::PeerSpec>{});
    kvStoreWrapper1_->run();

    // spin up an OpenrThriftServerWrapper
    openrThriftServerWrapper1_ = std::make_shared<OpenrThriftServerWrapper>(
        nodeId1_,
        nullptr /* decision */,
        nullptr /* fib */,
        kvStoreWrapper1_->getKvStore() /* kvStore */,
        nullptr /* linkMonitor */,
        nullptr /* configStore */,
        nullptr /* prefixManager */,
        MonitorSubmitUrl{"inproc://monitor_submit"},
        KvStoreLocalPubUrl{kvStoreWrapper1_->localPubUrl},
        context_);
    openrThriftServerWrapper1_->run();

    // spin up another kvStore through kvStoreWrapper
    kvStoreWrapper2_ = std::make_shared<KvStoreWrapper>(
        context_,
        nodeId2_,
        std::chrono::seconds(60), // db sync interval
        std::chrono::seconds(600), // counter submit interval,
        std::unordered_map<std::string, thrift::PeerSpec>{});
    kvStoreWrapper2_->run();

    // spin up another OpenrThriftServerWrapper
    openrThriftServerWrapper2_ = std::make_shared<OpenrThriftServerWrapper>(
        nodeId2_,
        nullptr /* decision */,
        nullptr /* fib */,
        kvStoreWrapper2_->getKvStore() /* kvStore */,
        nullptr /* linkMonitor */,
        nullptr /* configStore */,
        nullptr /* prefixManager */,
        MonitorSubmitUrl{"inproc://monitor_submit"},
        KvStoreLocalPubUrl{kvStoreWrapper2_->localPubUrl},
        context_);
    openrThriftServerWrapper2_->run();
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping openrCtrl thrift server thread";
    openrThriftServerWrapper1_->stop();
    openrThriftServerWrapper2_->stop();
    LOG(INFO) << "OpenrCtrl thrift server thread got stopped";

    LOG(INFO) << "Stopping KvStoreWrapper thread";
    kvStoreWrapper1_->stop();
    kvStoreWrapper2_->stop();
    LOG(INFO) << "KvStoreWrapper thread got stopped";
  }
  // var used to conmmunicate to kvStore through openrCtrl thrift server
  const std::string nodeId1_{"test_1"};
  const std::string nodeId2_{"test_2"};
  const std::string localhost_{"::1"};

  fbzmq::Context context_{};
  apache::thrift::CompactSerializer serializer;
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper1_;
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper2_;
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper1_;
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper2_;
};

TEST_F(MultipleKvStoreTestFixture, dumpAllTest) {
  const std::string key1{"test_key1"};
  const std::string key2{"test_key2"};

  std::vector<folly::SocketAddress> sockAddrs;
  const std::string prefix = "";
  const uint16_t port1 = openrThriftServerWrapper1_->getOpenrCtrlThriftPort();
  const uint16_t port2 = openrThriftServerWrapper2_->getOpenrCtrlThriftPort();
  sockAddrs.push_back(folly::SocketAddress{localhost_, port1});
  sockAddrs.push_back(folly::SocketAddress{localhost_, port2});

  // Step1: verify there is NOTHING inside kvStore instances
  auto preDb = dumpAllWithThriftClientFromMultiple(sockAddrs, prefix);
  EXPECT_TRUE(preDb.first.has_value());
  EXPECT_TRUE(preDb.first.value().empty());
  EXPECT_TRUE(preDb.second.empty());

  // Step2: initilize kvStoreClient connecting to different thriftServers
  OpenrEventBase evb;
  auto client1 = std::make_shared<KvStoreClientInternal>(
      context_,
      &evb,
      nodeId1_,
      kvStoreWrapper1_->localPubUrl,
      kvStoreWrapper1_->getKvStore());
  auto client2 = std::make_shared<KvStoreClientInternal>(
      context_,
      &evb,
      nodeId2_,
      kvStoreWrapper2_->localPubUrl,
      kvStoreWrapper2_->getKvStore());
  EXPECT_TRUE(nullptr != client1);
  EXPECT_TRUE(nullptr != client2);

  // Step3: insert (k1, v1) and (k2, v2) to different openrCtrlWrapper server
  evb.runInEventBaseThread([&]() noexcept {
    thrift::Value value;
    value.version = 1;
    {
      value.value = "test_value1";
      EXPECT_TRUE(client1->setKey(
          key1, fbzmq::util::writeThriftObjStr(value, serializer), 100));
    }
    {
      value.value = "test_value2";
      client2->setKey(
          key2, fbzmq::util::writeThriftObjStr(value, serializer), 200);
    }

    evb.stop();
  });

  evb.run();

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
    EXPECT_EQ("test_value1", pub[key1].value);
    EXPECT_EQ("test_value2", pub[key2].value);
  }

  // Step6: shutdown both thriftSevers and verify
  // dumpAllWithThriftClientFromMultiple() will get nothing.
  {
    openrThriftServerWrapper1_->stop();
    openrThriftServerWrapper2_->stop();

    auto db = dumpAllWithThriftClientFromMultiple(sockAddrs, prefix);
    ASSERT_TRUE(db.first.has_value());
    ASSERT_TRUE(db.first.value().empty());

    // start thriftSever to make sure TearDown method won't stop non-existing
    // server
    openrThriftServerWrapper1_->run();
    openrThriftServerWrapper2_->run();
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
