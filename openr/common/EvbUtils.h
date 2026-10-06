/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <utility>

#include <folly/futures/Future.h>
#include <folly/io/async/EventBase.h>

namespace openr {

/**
 * Run a callable on a target EventBase with a timeout.
 *
 * Converting the EventBase future to a SemiFuture before applying within()
 * makes the timeout use the global Timekeeper instead of the target
 * EventBase's timer wheel. The timeout therefore remains effective when the
 * target EventBase is stalled. The EventBase queue retains the callable after
 * the waiter times out, so callers must ensure anything referenced by the
 * callable outlives any late execution.
 */
template <typename F, typename Rep, typename Period>
auto
runOnEvbWithTimeout(
    folly::EventBase& evb, F&& fn, std::chrono::duration<Rep, Period> timeout) {
  return folly::via(&evb, std::forward<F>(fn)).semi().within(timeout);
}

} // namespace openr
