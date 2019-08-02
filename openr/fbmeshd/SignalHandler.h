/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <signal.h>

#include <fbzmq/async/AsyncSignalHandler.h>

namespace openr {
namespace fbmeshd {

/**
 * A commom signal handler which stops the underlying zmq event loop upon signal
 * catching for graceful exit. Use AsyncSignalHandler directly if you intend to
 * do more.
 */
class SignalHandler final : public fbzmq::AsyncSignalHandler {
  // This class should never be copied; remove default copy/move
  SignalHandler() = delete;
  SignalHandler(const SignalHandler&) = delete;
  SignalHandler(SignalHandler&&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;
  SignalHandler& operator=(SignalHandler&&) = delete;

 public:
  explicit SignalHandler(fbzmq::ZmqEventLoop& evl)
      : fbzmq::AsyncSignalHandler(&evl) {}

  void
  signalReceived(int sig) noexcept override {
    LOG(INFO) << "ZmqEventLoop received signal: " << sig;
    switch (sig) {
    case SIGABRT:
    case SIGINT:
    case SIGKILL:
    case SIGTERM: {
      LOG(INFO) << "Stopping ZmqEventLoop...";
      auto evl = getZmqEventLoop();
      evl->stop();
      break;
    }
    }
  }
};

} // namespace fbmeshd
} // namespace openr
