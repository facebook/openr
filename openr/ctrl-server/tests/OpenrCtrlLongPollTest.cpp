/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

#include <fbzmq/zmq/Context.h>
#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/OpenrClient.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

using namespace openr;

class LongPollFixture : public ::testing::Test {
  void
  SetUp() override {
    // Create KvStore module
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(
        context_,
        nodeName_,
        std::chrono::seconds(60),
        std::chrono::seconds(600),
        std::unordered_map<std::string, thrift::PeerSpec>());
    kvStoreWrapper_->run();

    // spin up an openrThriftServer
    openrThriftServerWrapper_ = std::make_shared<OpenrThriftServerWrapper>(
        nodeName_,
        MonitorSubmitUrl{"inproc://monitor-submit-url"},
        KvStoreLocalPubUrl{kvStoreWrapper_->localPubUrl},
        context_);

    // add module into thriftServer module map
    openrThriftServerWrapper_->addModuleType(
        thrift::OpenrModuleType::KVSTORE, kvStoreWrapper_->getKvStore());
    openrThriftServerWrapper_->run();

    // initialize openrCtrlClient talking to server
    openrCtrlThriftClient_ =
        getOpenrCtrlPlainTextClient<apache::thrift::HeaderClientChannel>(
            evb_,
            folly::IPAddress("::1"),
            openrThriftServerWrapper_->getOpenrCtrlThriftPort());
  }

  void
  TearDown() override {
    openrCtrlThriftClient_.reset();
    openrThriftServerWrapper_->stop();
    kvStoreWrapper_->stop();
  }

 private:
  fbzmq::Context context_;
  folly::EventBase evb_;

 public:
  const std::string nodeName_{"Valar-Morghulis"};
  const std::string adjKey_ = folly::sformat("adj:{}", nodeName_);
  const std::string prefixKey_ = folly::sformat("prefix:{}", nodeName_);

  fbzmq::ZmqEventLoop evl_;
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_;
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper_{nullptr};
  std::unique_ptr<openr::thrift::OpenrCtrlCppAsyncClient>
      openrCtrlThriftClient_{nullptr};
};

TEST_F(LongPollFixture, LongPollSuccess) {
  //
  // This UT mimicks the basic functionality of long poll API to make sure
  // server will return to client if there is "adj:" key change received.
  //
  bool isAdjChanged = false;
  bool isTimeout = false;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  // mimick there is a new publication from kvstore
  evl_.scheduleTimeout(std::chrono::milliseconds(5000), [&]() noexcept {
    LOG(INFO) << "AdjKey set...";
    // catch  up the time
    startTime = std::chrono::steady_clock::now();
    kvStoreWrapper_->setKey(
        adjKey_, createThriftValue(1, nodeName_, std::string("value1")));

    // stop the evl
    evl_.stop();
  });

  // start eventloop
  std::thread evlThread([&]() { evl_.run(); });
  evl_.waitUntilRunning();

  // client starts to do long-poll
  try {
    // By default, the processing timeout value for client is 10s.
    LOG(INFO) << "Start long poll...";
    thrift::KeyVals snapshot;
    isAdjChanged = openrCtrlThriftClient_->sync_longPollKvStoreAdj(snapshot);
    endTime = std::chrono::steady_clock::now();
    LOG(INFO) << "Finished long poll...";
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  // make sure we are receiving update and NOT timed out
  ASSERT_FALSE(isTimeout);
  // make sure when there is publication, processing delay is less than 50ms
  ASSERT_LE(endTime - startTime, std::chrono::milliseconds(50));
  ASSERT_TRUE(isAdjChanged);

  // wait for evl before cleanup
  evl_.waitUntilStopped();
  evlThread.join();
}

TEST_F(LongPollFixture, LongPollTimeout) {
  //
  // This UT mimicks the scenario there is a client side timeout since
  // there is NOT "adj:" key change.
  //
  bool isTimeout = false;
  bool isAdjChanged = false;

  // mimick there is a new publication from kvstore
  evl_.scheduleTimeout(std::chrono::milliseconds(5000), [&]() noexcept {
    LOG(INFO) << "Prefix key set...";
    kvStoreWrapper_->setKey(
        prefixKey_, createThriftValue(1, nodeName_, std::string("value1")));

    // stop the evl
    evl_.stop();
  });

  // start eventloop
  std::thread evlThread([&]() { evl_.run(); });
  evl_.waitUntilRunning();

  // client starts to do long-poll
  try {
    // By default, the processing timeout value for client is 10s.
    LOG(INFO) << "Start long poll...";
    thrift::KeyVals snapshot;
    isAdjChanged = openrCtrlThriftClient_->sync_longPollKvStoreAdj(snapshot);
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  // Client timeout and nothing received
  ASSERT_TRUE(isTimeout);
  ASSERT_FALSE(isAdjChanged);

  // Explicitly cleanup pending longPollReq
  openrThriftServerWrapper_->getOpenrCtrlHandler()
      ->cleanupPendingLongPollReqs();

  // wait for evl before cleanup
  evl_.waitUntilStopped();
  evlThread.join();
}

TEST_F(LongPollFixture, LongPollPendingAdj) {
  //
  // Test1: mimicks the scenario that before client send req. There is already
  // "adj:" key published before client subscribe. Should push immediately.
  //
  bool isTimeout = false;
  bool isAdjChanged = false;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  // inject key to kvstore and openrCtrlThriftServer should have adj key
  kvStoreWrapper_->setKey(
      adjKey_, createThriftValue(1, nodeName_, std::string("value1")));

  try {
    // mimicking scenario that server has different value for the same key
    thrift::KeyVals snapshot;
    snapshot.emplace(
        adjKey_, createThriftValue(2, "Valar-Dohaeris", std::string("value1")));

    // By default, the processing timeout value for client is 10s.
    LOG(INFO) << "Start long poll...";
    startTime = std::chrono::steady_clock::now();
    isAdjChanged = openrCtrlThriftClient_->sync_longPollKvStoreAdj(snapshot);
    endTime = std::chrono::steady_clock::now();
    LOG(INFO) << "Finished long poll...";
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  ASSERT_TRUE(isAdjChanged);
  ASSERT_FALSE(isTimeout);

  //
  // Test2: mimicks the scenario that client already hold the same adj key.
  // Server will NOT push notification since there is no diff.
  //
  isTimeout = false;
  isAdjChanged = false;
  try {
    thrift::KeyVals snapshot;
    snapshot.emplace(
        adjKey_, createThriftValue(1, nodeName_, std::string("value1")));

    // By default, the processing timeout value for client is 10s.
    LOG(INFO) << "Start long poll...";
    startTime = std::chrono::steady_clock::now();
    isAdjChanged = openrCtrlThriftClient_->sync_longPollKvStoreAdj(snapshot);
    endTime = std::chrono::steady_clock::now();
    LOG(INFO) << "Finished long poll...";
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  ASSERT_FALSE(isAdjChanged);
  ASSERT_TRUE(isTimeout);

  // Explicitly cleanup pending longPollReq
  openrThriftServerWrapper_->getOpenrCtrlHandler()
      ->cleanupPendingLongPollReqs();
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
