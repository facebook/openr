/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

class LongPollFixture : public ::testing::Test {
  void
  SetUp() override {
    // create KvStoreConfig
    thrift::KvStoreConfig kvStoreConfig;
    kvStoreConfig.node_name() = nodeName_;
    const folly::F14FastSet<std::string> areaIds{kTestingAreaName.t};

    // Create KvStore module
    kvStoreWrapper_ =
        std::make_unique<KvStoreWrapper<thrift::OpenrCtrlCppAsyncClient>>(
            areaIds, kvStoreConfig);
    kvStoreWrapper_->run();

    // create Dispatcher module
    dispatcher_ = std::make_shared<Dispatcher>(
        kvStoreWrapper_->getReader(), kvStorePublicationsQueue_);

    dispatcherThread_ = std::thread([this]() { dispatcher_->run(); });
    dispatcher_->waitUntilRunning();

    // initialize OpenrCtrlHandler for testing usage
    handler_ = std::make_shared<OpenrCtrlHandler>(
        nodeName_,
        folly::F14FastSet<std::string>{},
        &ctrlEvb_,
        nullptr, /* decision raw ptr */
        nullptr, /* fib raw ptr */
        kvStoreWrapper_->getKvStore(), /* kvstore raw ptr */
        nullptr, /* link-monitor raw ptr */
        nullptr, /* monitor raw ptr */
        nullptr, /* persistent-store raw ptr */
        nullptr, /* prefix-mgr raw ptr */
        nullptr, /* spark raw ptr */
        nullptr /* config shared-ptr */,
        dispatcher_.get());

    // ATTN: ctrlEvb must running in separate thread to mimick receiving
    // adj update for long-poll
    ctrlEvbThread_ = std::thread([&]() { ctrlEvb_.run(); });
    ctrlEvb_.waitUntilRunning();
  }

  void
  TearDown() override {
    kvStorePublicationsQueue_.close();
    kvStoreWrapper_->closeQueue();
    handler_.reset();

    ctrlEvb_.stop();
    ctrlEvb_.waitUntilStopped();
    ctrlEvbThread_.join();

    dispatcher_->stop();
    dispatcherThread_.join();

    kvStoreWrapper_->stop();
    kvStoreWrapper_.reset();
  }

 private:
  OpenrEventBase ctrlEvb_;
  std::thread ctrlEvbThread_;
  std::shared_ptr<Dispatcher> dispatcher_{nullptr};
  std::thread dispatcherThread_;

  DispatcherQueue kvStorePublicationsQueue_;

 public:
  const std::string nodeName_{"Valar-Morghulis"};
  const std::string adjKey_ = fmt::format("adj:{}", nodeName_);
  const std::string prefixKey_ = fmt::format("prefix:{}", nodeName_);

  openr::OpenrEventBase testEvb_;
  std::unique_ptr<KvStoreWrapper<thrift::OpenrCtrlCppAsyncClient>>
      kvStoreWrapper_;
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

  kvStoreWrapper_->setKey(
      kTestingAreaName,
      adjKey_,
      createThriftValue(1, nodeName_, std::string("value1")));

  // start long-poll
  LOG(INFO) << "Start long poll...";
  startTime = std::chrono::steady_clock::now();
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
}

/*
 * This UT mimicks the scenario there is a client side timeout since
 * there is NO "adj:" key change.
 */
TEST_F(LongPollFixture, LongPollTimeout) {
  bool isAdjChanged = false;

  /**
   * mimick there is a new publication from kvstore
   * Publication from separate thread required for Longpoll to timeout
   */
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

  /**
   * mimick there is a new publication from kvstore.
   * This publication should clean up pending req.
   * Publication from separate thread required for Longpoll to timeout
   */
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
  const folly::Init init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
