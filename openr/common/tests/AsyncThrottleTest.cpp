/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <folly/io/async/EventBase.h>
#include <openr/common/AsyncThrottle.h>

namespace chrono = std::chrono;

namespace openr {

TEST(AsyncThrottleTest, ThrottleTest) {
  folly::EventBase evb;

  int count = 0;
  AsyncThrottle throttledFn(
      &evb, chrono::milliseconds(100), [&count]() noexcept {
        count++;
        LOG(INFO) << "Incremented counter. New value: " << count;
      });

  auto timer1 = folly::AsyncTimeout::make(evb, [&]() noexcept {
    LOG(INFO) << "timer1 callback ... " << count;
    EXPECT_FALSE(throttledFn.isActive());
    EXPECT_EQ(0, count);
    for (int i = 0; i < 100; i++) {
      throttledFn();
      EXPECT_EQ(0, count);
      EXPECT_TRUE(throttledFn.isActive());
    }
  });

  auto callbackFn = [&]() noexcept {
    EXPECT_EQ(1, count);
    throttledFn();
    EXPECT_TRUE(throttledFn.isActive());
  };
  auto callbackTimer = folly::AsyncTimeout::make(evb, callbackFn);

  auto timer2 = folly::AsyncTimeout::make(evb, [&]() noexcept {
    LOG(INFO) << "timer2 callback ... " << count;
    EXPECT_EQ(1, count);

    // All the below ones should lead to only one increment
    EXPECT_FALSE(throttledFn.isActive());
    for (int i = 10; i <= 90; i = i + 10) {
      callbackTimer->scheduleTimeout(chrono::milliseconds(i));
    }
    EXPECT_FALSE(throttledFn.isActive());

    // Validate count
    folly::AsyncTimeout::schedule(
        chrono::milliseconds(200), evb, [&]() noexcept {
          EXPECT_EQ(2, count); // Count must be 2
        });

    // Below shouldn't lead to any increment
    folly::AsyncTimeout::schedule(
        chrono::milliseconds(210), evb, [&]() noexcept {
          EXPECT_FALSE(throttledFn.isActive());
          throttledFn();
          EXPECT_TRUE(throttledFn.isActive());
        });

    folly::AsyncTimeout::schedule(
        chrono::milliseconds(200), evb, [&]() noexcept {
          EXPECT_TRUE(throttledFn.isActive());
          throttledFn.cancel();
          EXPECT_FALSE(throttledFn.isActive());
        });

    // Schedule stop and validate final counter
    folly::AsyncTimeout::schedule(
        chrono::milliseconds(200), evb, [&]() noexcept {
          EXPECT_EQ(2, count); // Count must be 2
          evb.terminateLoopSoon();
        });
  });

  timer1->scheduleTimeout(chrono::milliseconds(0));
  timer2->scheduleTimeout(chrono::milliseconds(200));

  // Loop thread
  LOG(INFO) << "Starting event base.";
  EXPECT_EQ(0, count);
  evb.loop();
  EXPECT_EQ(2, count);
  LOG(INFO) << "Stopping event base.";
}

TEST(AsyncThrottleTest, ImmediateExecution) {
  folly::EventBase evb;

  int count = 0;
  AsyncThrottle throttledFn(&evb, chrono::milliseconds(0), [&count]() noexcept {
    count++;
    LOG(INFO) << "Incremented counter. New value: " << count;
  });

  evb.runInLoop([&]() noexcept {
    EXPECT_EQ(0, count);
    throttledFn();
    EXPECT_EQ(1, count);
    evb.terminateLoopSoon();
  });

  // Loop thread
  LOG(INFO) << "Starting event base.";
  EXPECT_EQ(0, count);
  evb.loop();
  EXPECT_EQ(1, count);
  LOG(INFO) << "Stopping event base.";
}

} // namespace openr

int
main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  return RUN_ALL_TESTS();
}
