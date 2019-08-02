/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <signal.h>

#include <fbzmq/async/ZmqEventLoop.h>
#include <folly/io/async/AsyncSignalHandler.h>

namespace openr {
namespace fbmeshd {

/**
 * A signal handler for auxiliary event loops using folly::EventBase which stops
 * the underlying zmq event loop upon signal catching for graceful exit.
 */
class FollySignalHandler final : public folly::AsyncSignalHandler {
  // This class should never be copied; remove default copy/move
  FollySignalHandler() = delete;
  FollySignalHandler(const FollySignalHandler&) = delete;
  FollySignalHandler(FollySignalHandler&&) = delete;
  FollySignalHandler& operator=(const FollySignalHandler&) = delete;
  FollySignalHandler& operator=(FollySignalHandler&&) = delete;

 public:
  explicit FollySignalHandler(folly::EventBase& evb, fbzmq::ZmqEventLoop& evl)
      : folly::AsyncSignalHandler(&evb), evl_{evl} {}

  void
  signalReceived(int sig) noexcept override {
    LOG(INFO) << "EventBase received signal: " << sig;
    switch (sig) {
    case SIGABRT:
    case SIGINT:
    case SIGKILL:
    case SIGTERM: {
      LOG(INFO) << "Stopping ZmqEventLoop...";
      evl_.runImmediatelyOrInEventLoop([&]() { evl_.stop(); });
      break;
    }
    }
  }

 private:
  fbzmq::ZmqEventLoop& evl_;
};

} // namespace fbmeshd
} // namespace openr
