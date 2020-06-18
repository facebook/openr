/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>

namespace openr {

using TimeoutCallback = folly::Function<void(void)>;

/**
 * This class provides you capability to rate-limit certain events which might
 * happen rapidly in the system and processing of an event is expensive.
 *
 * For e.g. you want to `saveState()` on every `addKey` and `removeKey` but
 * saving state is expensive operation. You can do
 *
 *  auto throttledSaveState = AsyncThrottle(*evl, 1_s, [this] () noexcept {
 *    saveState();
 *  });
 *
 *  And then call `throttledSaveState()` on every `addKey` and `removeKey` but
 *  internally `saveState()` will be execute at max once per second.
 */
class AsyncThrottle final : private folly::AsyncTimeout {
 public:
  AsyncThrottle(
      folly::EventBase* eventBase,
      std::chrono::milliseconds timeout,
      TimeoutCallback callback);

  ~AsyncThrottle() override = default;

  /**
   * Overload function operator. This method exposes throttled version of
   * callback passed in.
   */
  void operator()() noexcept;

  /**
   * Tells you if this is currently active ?
   */
  bool
  isActive() const {
    return isScheduled();
  }

  /**
   * Cancel scheduled throttle
   */
  void
  cancel() {
    cancelTimeout();
  }

 private:
  /**
   * Overrides timeout callback
   */
  void timeoutExpired() noexcept override;

  const std::chrono::milliseconds timeout_{0};
  TimeoutCallback callback_{nullptr};
};

} // namespace openr
