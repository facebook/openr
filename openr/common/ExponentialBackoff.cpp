/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/ExponentialBackoff.h>

#include <folly/logging/xlog.h>

namespace openr {

template <typename Duration>
ExponentialBackoff<Duration>::ExponentialBackoff()
    : ExponentialBackoff(Duration(1), Duration(2), false) {}

template <typename Duration>
ExponentialBackoff<Duration>::ExponentialBackoff(
    Duration initialBackoff, Duration maxBackoff, bool isAbortAtMax)
    : initialBackoff_(initialBackoff),
      maxBackoff_(maxBackoff),
      currentBackoff_(0),
      isAbortAtMax_(isAbortAtMax) {
  XCHECK_GT(initialBackoff.count(), Duration(0).count())
      << "Backoff must be positive value";
  XCHECK_LT(initialBackoff.count(), maxBackoff.count())
      << "Max backoff must be greater than initial backoff.";
}

template <typename Duration>
bool
ExponentialBackoff<Duration>::canTryNow() const {
  return getTimeRemainingUntilRetry() == Duration(0);
}

template <typename Duration>
void
ExponentialBackoff<Duration>::reportSuccess() {
  // Set error time to clock's epoch
  lastErrorTime_ = std::chrono::steady_clock::time_point();
  currentBackoff_ = Duration(0);
}

template <typename Duration>
void
ExponentialBackoff<Duration>::reportError() {
  lastErrorTime_ = std::chrono::steady_clock::now();
  if (currentBackoff_ >= maxBackoff_ && isAbortAtMax_) {
    XLOG(ERR) << "Max back-off reached, isAbortAtMax true! Abort! Abort!";
    ::abort();
  }
  if (currentBackoff_ == Duration(0)) {
    currentBackoff_ = initialBackoff_;
  } else {
    currentBackoff_ = std::min(maxBackoff_, 2 * currentBackoff_);
  }
}

template <typename Duration>
bool
ExponentialBackoff<Duration>::atMaxBackoff() const {
  return currentBackoff_ >= maxBackoff_;
}

template <typename Duration>
Duration
ExponentialBackoff<Duration>::getTimeRemainingUntilRetry() const {
  auto res = std::chrono::duration_cast<Duration>(
      (lastErrorTime_ + currentBackoff_) - std::chrono::steady_clock::now());
  return (res < Duration(0)) ? Duration(0) : res;
}

template <typename Duration>
std::chrono::steady_clock::time_point
ExponentialBackoff<Duration>::getLastErrorTime() const {
  return lastErrorTime_;
}

template <typename Duration>
Duration
ExponentialBackoff<Duration>::getInitialBackoff() const {
  return initialBackoff_;
}

template <typename Duration>
Duration
ExponentialBackoff<Duration>::getMaxBackoff() const {
  return maxBackoff_;
}

template <typename Duration>
bool
ExponentialBackoff<Duration>::getIsAbortAtMax() const {
  return isAbortAtMax_;
}

// define template instance for some common usecases
template class ExponentialBackoff<std::chrono::microseconds>;
template class ExponentialBackoff<std::chrono::milliseconds>;
template class ExponentialBackoff<std::chrono::seconds>;
} // namespace openr
