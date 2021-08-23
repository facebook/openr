/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <openr/common/ExponentialBackoff.h>

namespace openr {

/**
 * This class provides you the capability to rate-limit certain events with an
 * exponential backoff which might happen rapidly in the system when processing
 * of the event is expensive.
 *
 * It is similar to AsyncThrottle except each time invoked we double the amount
 * of wait time until execution from minBackoff to maxBackoff
 */
template <typename Duration>
class AsyncDebounce final : public folly::AsyncTimeout {
 public:
  using TimeoutCallback = folly::Function<void(void)>;

  AsyncDebounce(
      folly::EventBase* eventBase,
      Duration minBackOff,
      Duration maxBackOff,
      TimeoutCallback callback)
      : AsyncTimeout(eventBase),
        backoff_(minBackOff, maxBackOff),
        callback_(std::move(callback)) {}

  ~AsyncDebounce() override = default;

  /**
   * Overload function operator. This method exposes debounced version of
   * callback passed in.
   */
  void
  operator()() noexcept {
    if (!backoff_.atMaxBackoff()) {
      backoff_.reportError();
      // schedule or reschedule timeout
      scheduleTimeout(backoff_.getCurrentBackoff());
    }
    CHECK(isScheduled());
  }

 private:
  void
  timeoutExpired() noexcept override {
    backoff_.reportSuccess();
    callback_();
  }

  ExponentialBackoff<Duration> backoff_;
  TimeoutCallback callback_{nullptr};
};

} // namespace openr
