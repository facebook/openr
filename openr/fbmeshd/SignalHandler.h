/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <signal.h>

#include <fbzmq/async/AsyncSignalHandler.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>

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
  explicit SignalHandler(fbzmq::ZmqEventLoop& evl, Nl80211Handler& nlHandler)
      : fbzmq::AsyncSignalHandler(&evl), nlHandler_(nlHandler) {}

 private:
  Nl80211Handler& nlHandler_;

  void
  signalReceived(int sig) noexcept override {
    LOG(INFO) << "Received signal: " << sig;
    switch (sig) {
    case SIGTERM:
    case SIGKILL:
    case SIGINT:
    case SIGABRT: {
      LOG(INFO) << ". Stopping event loop ...";
      auto evl = getZmqEventLoop();
      evl->stop();
      break;
    }
    }
  }
};

} // namespace fbmeshd
} // namespace openr
