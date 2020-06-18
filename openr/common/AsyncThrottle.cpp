/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/AsyncThrottle.h"

namespace openr {

AsyncThrottle::AsyncThrottle(
    folly::EventBase* eventBase,
    std::chrono::milliseconds timeout,
    TimeoutCallback callback)
    : AsyncTimeout(eventBase),
      timeout_(timeout),
      callback_(std::move(callback)) {
  CHECK(callback_);
}

void
AsyncThrottle::operator()() noexcept {
  // Return immediately as callback is already scheduled.
  if (isScheduled()) {
    return;
  }

  // Special case to handle immediate timeouts
  if (timeout_ <= std::chrono::milliseconds(0)) {
    callback_();
    return;
  }

  scheduleTimeout(timeout_);
}

void
AsyncThrottle::timeoutExpired() noexcept {
  callback_();
}

} // namespace openr
