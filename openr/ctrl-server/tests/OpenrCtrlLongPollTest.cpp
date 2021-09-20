/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

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

    // initialize OpenrCtrlHandler for testing usage
    handler_ = kvStoreWrapper_->getThriftServerCtrlHandler();
  }

  void
  TearDown() override {
    kvStoreWrapper_->closeQueue();
    handler_.reset();
    kvStoreWrapper_->stop();
    kvStoreWrapper_.reset();
  }

 private:
  fbzmq::Context context_;
  folly::EventBase evb_;

 public:
  const std::string nodeName_{"Valar-Morghulis"};
  const std::string adjKey_ = fmt::format("adj:{}", nodeName_);
  const std::string prefixKey_ = fmt::format("prefix:{}", nodeName_);

  openr::OpenrEventBase testEvb_;
  std::shared_ptr<Config> config_{nullptr};
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_;
  std::shared_ptr<OpenrCtrlHandler> handler_{nullptr};
};

/*
 * This UT mimicks the basic functionality of long poll API to make sure
 * server will return to client if there is "adj:" key change received.
 */
TEST_F(LongPollFixture, LongPollAdjAdded) {
  bool isAdjChanged = false;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  // mimick there is a new publication from kvstore
  testEvb_.scheduleTimeout(std::chrono::milliseconds(1000), [&]() noexcept {
    // record the time
    startTime = std::chrono::steady_clock::now();
    kvStoreWrapper_->setKey(
        kTestingAreaName,
        adjKey_,
        createThriftValue(1, nodeName_, std::string("value1")));

    // stop the evb
    testEvb_.stop();
  });

  // start eventloop
  std::thread evbThread([&]() { testEvb_.run(); });
  testEvb_.waitUntilRunning();

  // start long-poll
  LOG(INFO) << "Start long poll...";
  isAdjChanged = handler_
                     ->semifuture_longPollKvStoreAdjArea(
                         std::make_unique<std::string>(kTestingAreaName),
                         std::make_unique<thrift::KeyVals>())
                     .get();
  endTime = std::chrono::steady_clock::now();
  LOG(INFO) << "Finished long poll...";

  // make sure when there is publication, processing delay is less than 250ms
  ASSERT_LE(endTime - startTime, std::chrono::milliseconds(250));
  ASSERT_TRUE(isAdjChanged);

  // wait for evl before cleanup
  testEvb_.waitUntilStopped();
  evbThread.join();
}

/*
 * This UT mimicks the scenario there is a client side timeout since
 * there is NO "adj:" key change.
 */
TEST_F(LongPollFixture, LongPollTimeout) {
  bool isAdjChanged = false;

  // mimick there is a new publication from kvstore
  testEvb_.scheduleTimeout(
      Constants::kLongPollReqHoldTime + std::chrono::milliseconds(1000),
      [&]() noexcept {
        kvStoreWrapper_->setKey(
            kTestingAreaName,
            prefixKey_,
            createThriftValue(1, nodeName_, std::string("value1")));

        // stop the evl
        testEvb_.stop();
      });

  // start eventloop
  std::thread evbThread([&]() { testEvb_.run(); });
  testEvb_.waitUntilRunning();

  // start long-poll
  LOG(INFO) << "Start long poll...";
  isAdjChanged = handler_
                     ->semifuture_longPollKvStoreAdjArea(
                         std::make_unique<std::string>(kTestingAreaName),
                         std::make_unique<thrift::KeyVals>())
                     .get();
  LOG(INFO) << "Finished long poll...";

  // Server side timeout with no adj change
  ASSERT_FALSE(isAdjChanged);

  // Explicitly cleanup pending longPollReq
  handler_->cleanupPendingLongPollReqs();

  // wait for evl before cleanup
  testEvb_.waitUntilStopped();
  evbThread.join();
}

/*
 * This UT mimicks the scenario that before client send req.
 * There is already "adj:" key published before client subscribe.
 * Should push immediately.
 */
TEST_F(LongPollFixture, LongPollAdjModified) {
  bool isAdjChanged = false;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  // inject key to kvstore and openrCtrlThriftServer should have adj key
  kvStoreWrapper_->setKey(
      kTestingAreaName,
      adjKey_,
      createThriftValue(2, nodeName_, std::string("value1")));

  // mimicking scenario that server has different value for the same key
  thrift::KeyVals snapshot;
  snapshot.emplace(
      adjKey_, createThriftValue(1, "Valar-Dohaeris", std::string("value1")));

  // By default, the processing timeout value for client is 10s.
  LOG(INFO) << "Start long poll...";
  startTime = std::chrono::steady_clock::now();
  isAdjChanged = handler_
                     ->semifuture_longPollKvStoreAdjArea(
                         std::make_unique<std::string>(kTestingAreaName),
                         std::make_unique<thrift::KeyVals>(std::move(snapshot)))
                     .get();
  endTime = std::chrono::steady_clock::now();
  LOG(INFO) << "Finished long poll...";

  // make sure when there is publication, processing delay is less than 50ms
  ASSERT_LE(endTime - startTime, std::chrono::milliseconds(50));
  ASSERT_TRUE(isAdjChanged);
}

/*
 * This UT mimicks the scenario that client already hold the same adj key.
 * Server will NOT push notification since there is no delta generated.
 */
TEST_F(LongPollFixture, LongPollAdjUnchanged) {
  bool isAdjChanged = true;

  // inject key to kvstore and openrCtrlThriftServer should have adj key
  kvStoreWrapper_->setKey(
      kTestingAreaName,
      adjKey_,
      createThriftValue(1, nodeName_, std::string("value1")));

  // mimick there is a new publication from kvstore.
  // This publication should clean up pending req.
  testEvb_.scheduleTimeout(
      Constants::kLongPollReqHoldTime + std::chrono::milliseconds(1000),
      [&]() noexcept {
        kvStoreWrapper_->setKey(
            kTestingAreaName,
            prefixKey_,
            createThriftValue(1, nodeName_, std::string("value1")));

        // stop the evl
        testEvb_.stop();
      });

  // start eventloop
  std::thread evbThread([&]() { testEvb_.run(); });
  testEvb_.waitUntilRunning();

  thrift::KeyVals snapshot;
  snapshot.emplace(
      adjKey_, createThriftValue(1, nodeName_, std::string("value1")));

  LOG(INFO) << "Start long poll...";
  isAdjChanged = handler_
                     ->semifuture_longPollKvStoreAdjArea(
                         std::make_unique<std::string>(kTestingAreaName),
                         std::make_unique<thrift::KeyVals>(std::move(snapshot)))
                     .get();
  LOG(INFO) << "Finished long poll...";

  ASSERT_FALSE(isAdjChanged);

  // wait for evl before cleanup
  testEvb_.waitUntilStopped();
  evbThread.join();
}

/*
 * This UT mimicks the scenario that client hold adj key,
 * but server doesn't have this adj key(e.g. adj key expiration)
 */
TEST_F(LongPollFixture, LongPollAdjExpired) {
  bool isAdjChanged = false;

  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point endTime;

  // mimicking scenario that server has different value for the same key
  thrift::KeyVals snapshot;
  snapshot.emplace(
      adjKey_, createThriftValue(1, "Valar-Dohaeris", std::string("value1")));

  LOG(INFO) << "Start long poll...";
  startTime = std::chrono::steady_clock::now();
  isAdjChanged = handler_
                     ->semifuture_longPollKvStoreAdjArea(
                         std::make_unique<std::string>(kTestingAreaName),
                         std::make_unique<thrift::KeyVals>(std::move(snapshot)))
                     .get();
  endTime = std::chrono::steady_clock::now();
  LOG(INFO) << "Finished long poll...";

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
