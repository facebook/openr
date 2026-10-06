/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <memory>
#include <stdexcept>

#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>

#include <openr/common/EvbUtils.h>

using namespace std::chrono_literals;

namespace openr {
namespace {

constexpr auto kOperationTimeout = 500ms;
constexpr auto kCallbackWaitTimeout = 2s;
constexpr int kExpectedValue = 42;

struct DelayedCallbackState {
  folly::Baton<> started;
  folly::Baton<> release;
  folly::Baton<> completed;
};

struct ThrowTestError {
  [[noreturn]] int
  operator()() const {
    throw std::runtime_error("test error");
  }
};

} // namespace

TEST(EvbUtilsTest, SuccessReturnsValue) {
  folly::ScopedEventBaseThread evbThread;

  EXPECT_EQ(
      kExpectedValue,
      runOnEvbWithTimeout(
          *evbThread.getEventBase(),
          []() { return kExpectedValue; },
          kCallbackWaitTimeout)
          .get());
}

TEST(EvbUtilsTest, VoidCallableCompletes) {
  folly::ScopedEventBaseThread evbThread;
  folly::Baton<> completed;

  runOnEvbWithTimeout(
      *evbThread.getEventBase(),
      [&completed]() { completed.post(); },
      kCallbackWaitTimeout)
      .get();

  EXPECT_TRUE(completed.ready());
}

TEST(EvbUtilsTest, FutureCallableIsFlattened) {
  folly::ScopedEventBaseThread evbThread;

  EXPECT_EQ(
      kExpectedValue,
      runOnEvbWithTimeout(
          *evbThread.getEventBase(),
          []() { return folly::makeFuture(kExpectedValue); },
          kCallbackWaitTimeout)
          .get());
}

TEST(EvbUtilsTest, SemiFutureCallableIsFlattened) {
  folly::ScopedEventBaseThread evbThread;

  EXPECT_EQ(
      kExpectedValue,
      runOnEvbWithTimeout(
          *evbThread.getEventBase(),
          []() { return folly::makeSemiFuture(kExpectedValue); },
          kCallbackWaitTimeout)
          .get());
}

TEST(EvbUtilsTest, ExceptionIsReturned) {
  folly::ScopedEventBaseThread evbThread;

  EXPECT_THROW(
      runOnEvbWithTimeout(
          *evbThread.getEventBase(), ThrowTestError{}, kCallbackWaitTimeout)
          .get(),
      std::runtime_error);
}

TEST(EvbUtilsTest, TimeoutAllowsCallbackToFinishSafely) {
  folly::ScopedEventBaseThread evbThread;
  auto state = std::make_shared<DelayedCallbackState>();

  EXPECT_THROW(
      runOnEvbWithTimeout(
          *evbThread.getEventBase(),
          [state]() {
            state->started.post();
            const auto released =
                state->release.try_wait_for(kCallbackWaitTimeout);
            state->completed.post();
            return released;
          },
          kOperationTimeout)
          .get(),
      folly::FutureTimeout);

  EXPECT_TRUE(state->started.ready());

  state->release.post();
  EXPECT_TRUE(state->completed.try_wait_for(kCallbackWaitTimeout));
}

TEST(EvbUtilsTest, TimeoutDoesNotDependOnTargetEventBase) {
  folly::ScopedEventBaseThread evbThread;
  auto blocker = std::make_shared<DelayedCallbackState>();
  auto callbackCompleted = std::make_shared<folly::Baton<>>();
  evbThread.getEventBase()->runInEventBaseThread([blocker]() {
    blocker->started.post();
    blocker->release.try_wait_for(kCallbackWaitTimeout);
    blocker->completed.post();
  });
  ASSERT_TRUE(blocker->started.try_wait_for(kCallbackWaitTimeout));

  EXPECT_THROW(
      runOnEvbWithTimeout(
          *evbThread.getEventBase(),
          [callbackCompleted]() {
            callbackCompleted->post();
            return true;
          },
          kOperationTimeout)
          .get(),
      folly::FutureTimeout);
  EXPECT_FALSE(callbackCompleted->ready());

  blocker->release.post();
  EXPECT_TRUE(blocker->completed.try_wait_for(kCallbackWaitTimeout));
  EXPECT_TRUE(callbackCompleted->try_wait_for(kCallbackWaitTimeout));
}

} // namespace openr

int
main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  const folly::Init init(&argc, &argv);
  return RUN_ALL_TESTS();
}
