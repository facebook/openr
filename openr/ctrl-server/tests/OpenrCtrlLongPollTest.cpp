/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;

class LongPollFixture : public ::testing::Test {
  void
  SetUp() override {
    // create config
    auto tConfig = getBasicOpenrConfig(nodeName_);
    config_ = std::make_shared<Config>(tConfig);

    // Create KvStore module
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(context_, config_);
    kvStoreWrapper_->run();

    // initialize openrCtrlClient talking to server
    client1_ = getOpenrCtrlPlainTextClient<apache::thrift::HeaderClientChannel>(
        evb_,
        folly::IPAddress(Constants::kPlatformHost.toString()),
        kvStoreWrapper_->getThriftPort());

    // Create client to have client-side timeout longer than OpenrCtrlServer
    // side default. This mimick we are NOT timeout but receive "false"
    // indicating no changes.
    client2_ = getOpenrCtrlPlainTextClient<apache::thrift::HeaderClientChannel>(
        evb_,
        folly::IPAddress(Constants::kPlatformHost.toString()),
        kvStoreWrapper_->getThriftPort(),
        Constants::kServiceConnTimeout,
        Constants::kLongPollReqHoldTime + std::chrono::milliseconds(10000));
  }

  void
  TearDown() override {
    kvStoreWrapper_->closeQueue();
    client1_.reset();
    client2_.reset();
    kvStoreWrapper_->stop();
    kvStoreWrapper_.reset();
  }

 private:
  fbzmq::Context context_;
  folly::EventBase evb_;

 public:
  const std::string nodeName_{"Valar-Morghulis"};
  const std::string adjKey_ = folly::sformat("adj:{}", nodeName_);
  const std::string prefixKey_ = folly::sformat("prefix:{}", nodeName_);

  openr::OpenrEventBase testEvb_;
  std::shared_ptr<Config> config_{nullptr};
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_;
  std::unique_ptr<openr::thrift::OpenrCtrlCppAsyncClient> client1_{nullptr};
  std::unique_ptr<openr::thrift::OpenrCtrlCppAsyncClient> client2_{nullptr};
};

TEST_F(LongPollFixture, LongPollAdjAdded) {
  //
  // This UT mimicks the basic functionality of long poll API to make sure
  // server will return to client if there is "adj:" key change received.
  //
  bool isAdjChanged = false;
  bool isTimeout = false;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  // mimick there is a new publication from kvstore
  testEvb_.scheduleTimeout(std::chrono::milliseconds(5000), [&]() noexcept {
    LOG(INFO) << "AdjKey set...";
    // catch  up the time
    startTime = std::chrono::steady_clock::now();
    kvStoreWrapper_->setKey(
        kTestingAreaName,
        adjKey_,
        createThriftValue(1, nodeName_, std::string("value1")));

    // stop the evl
    testEvb_.stop();
  });

  // start eventloop
  std::thread evlThread([&]() { testEvb_.run(); });
  testEvb_.waitUntilRunning();

  // client starts to do long-poll
  try {
    // By default, the processing timeout value for client is 10s.
    LOG(INFO) << "Start long poll...";
    thrift::KeyVals snapshot;
    isAdjChanged =
        client1_->sync_longPollKvStoreAdjArea(kTestingAreaName, snapshot);
    endTime = std::chrono::steady_clock::now();
    LOG(INFO) << "Finished long poll...";
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  // make sure we are receiving update and NOT timed out
  ASSERT_FALSE(isTimeout);
  // make sure when there is publication, processing delay is less than 450ms
  ASSERT_LE(endTime - startTime, std::chrono::milliseconds(450));
  ASSERT_TRUE(isAdjChanged);

  // wait for evl before cleanup
  testEvb_.waitUntilStopped();
  evlThread.join();
}

TEST_F(LongPollFixture, LongPollTimeout) {
  //
  // This UT mimicks the scenario there is a client side timeout since
  // there is NO "adj:" key change.
  //
  bool isTimeout = false;
  bool isAdjChanged = false;

  // mimick there is a new publication from kvstore
  testEvb_.scheduleTimeout(std::chrono::milliseconds(5000), [&]() noexcept {
    LOG(INFO) << "Prefix key set...";
    kvStoreWrapper_->setKey(
        kTestingAreaName,
        prefixKey_,
        createThriftValue(1, nodeName_, std::string("value1")));

    // stop the evl
    testEvb_.stop();
  });

  // start eventloop
  std::thread evlThread([&]() { testEvb_.run(); });
  testEvb_.waitUntilRunning();

  // client starts to do long-poll
  try {
    // By default, the processing timeout value for client is 10s.
    LOG(INFO) << "Start long poll...";
    thrift::KeyVals snapshot;
    isAdjChanged =
        client1_->sync_longPollKvStoreAdjArea(kTestingAreaName, snapshot);
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  // Client timeout and nothing received
  ASSERT_TRUE(isTimeout);
  ASSERT_FALSE(isAdjChanged);

  // Explicitly cleanup pending longPollReq
  kvStoreWrapper_->getThriftServerCtrlHandler()->cleanupPendingLongPollReqs();

  // wait for evl before cleanup
  testEvb_.waitUntilStopped();
  evlThread.join();
}

TEST_F(LongPollFixture, LongPollAdjModified) {
  //
  // This UT mimicks the scenario that before client send req.
  // There is already "adj:" key published before client subscribe.
  // Should push immediately.
  //
  bool isTimeout = false;
  bool isAdjChanged = false;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  // inject key to kvstore and openrCtrlThriftServer should have adj key
  kvStoreWrapper_->setKey(
      kTestingAreaName,
      adjKey_,
      createThriftValue(2, nodeName_, std::string("value1")));

  try {
    // mimicking scenario that server has different value for the same key
    thrift::KeyVals snapshot;
    snapshot.emplace(
        adjKey_, createThriftValue(1, "Valar-Dohaeris", std::string("value1")));

    // By default, the processing timeout value for client is 10s.
    LOG(INFO) << "Start long poll...";
    startTime = std::chrono::steady_clock::now();
    isAdjChanged =
        client1_->sync_longPollKvStoreAdjArea(kTestingAreaName, snapshot);
    endTime = std::chrono::steady_clock::now();
    LOG(INFO) << "Finished long poll...";
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  ASSERT_FALSE(isTimeout);
  // make sure when there is publication, processing delay is less than 50ms
  ASSERT_LE(endTime - startTime, std::chrono::milliseconds(50));
  ASSERT_TRUE(isAdjChanged);
}

TEST_F(LongPollFixture, LongPollAdjUnchanged) {
  //
  // This UT mimicks the scenario that client already hold the same adj key.
  // Server will NOT push notification since there is no diff.
  //
  bool isTimeout = false;
  bool isAdjChanged = true;

  // inject key to kvstore and openrCtrlThriftServer should have adj key
  kvStoreWrapper_->setKey(
      kTestingAreaName,
      adjKey_,
      createThriftValue(1, nodeName_, std::string("value1")));

  // mimick there is a new publication from kvstore.
  // This publication should clean up pending req.
  testEvb_.scheduleTimeout(
      Constants::kLongPollReqHoldTime + std::chrono::milliseconds(5000),
      [&]() noexcept {
        LOG(INFO) << "Prefix key set...";
        kvStoreWrapper_->setKey(
            kTestingAreaName,
            prefixKey_,
            createThriftValue(1, nodeName_, std::string("value1")));

        // stop the evl
        testEvb_.stop();
      });

  // start eventloop
  std::thread evlThread([&]() { testEvb_.run(); });
  testEvb_.waitUntilRunning();

  try {
    thrift::KeyVals snapshot;
    snapshot.emplace(
        adjKey_, createThriftValue(1, nodeName_, std::string("value1")));

    LOG(INFO) << "Start long poll...";
    isAdjChanged =
        client2_->sync_longPollKvStoreAdjArea(kTestingAreaName, snapshot);
    LOG(INFO) << "Finished long poll...";
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  ASSERT_FALSE(isAdjChanged);
  ASSERT_FALSE(isTimeout);

  // wait for evl before cleanup
  testEvb_.waitUntilStopped();
  evlThread.join();
}

TEST_F(LongPollFixture, LongPollAdjExpired) {
  //
  // This UT mimicks the scenario that client hold adj key,
  // but server doesn't have this adj key( e.g. adj key expiration )
  //
  bool isTimeout = false;
  bool isAdjChanged = false;

  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  try {
    // mimicking scenario that server has different value for the same key
    thrift::KeyVals snapshot;
    snapshot.emplace(
        adjKey_, createThriftValue(1, "Valar-Dohaeris", std::string("value1")));

    // By default, the processing timeout value for client is 10s.
    LOG(INFO) << "Start long poll...";
    startTime = std::chrono::steady_clock::now();
    isAdjChanged =
        client1_->sync_longPollKvStoreAdjArea(kTestingAreaName, snapshot);
    endTime = std::chrono::steady_clock::now();
    LOG(INFO) << "Finished long poll...";
  } catch (std::exception& ex) {
    LOG(INFO) << "Exception happened: " << folly::exceptionStr(ex);
    isTimeout = true;
  }

  ASSERT_FALSE(isTimeout);
  // make sure when there is publication, processing delay is less than 50ms
  ASSERT_LE(endTime - startTime, std::chrono::milliseconds(50));
  ASSERT_TRUE(isAdjChanged);
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
