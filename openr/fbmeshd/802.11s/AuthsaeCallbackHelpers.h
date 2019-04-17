/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

extern "C" {
#include <authsae/ampe.h>
#include <authsae/evl_ops.h>
#include <authsae/sae.h>
}

#include <fbzmq/async/ZmqEventLoop.h>

// Creates the function callback tables for AMPE and SAE
ampe_cb* getAmpeCallbacks();
sae_cb* getSaeCallbacks();

namespace openr {
namespace fbmeshd {

// This is a static class that provides certain helpers necessary for
// interfacing with libsae
class AuthsaeCallbackHelpers final {
  // This class is static, remove default copy/move
  AuthsaeCallbackHelpers() = delete;
  AuthsaeCallbackHelpers(const AuthsaeCallbackHelpers&) = delete;
  AuthsaeCallbackHelpers(AuthsaeCallbackHelpers&&) = delete;
  AuthsaeCallbackHelpers& operator=(const AuthsaeCallbackHelpers&) = delete;
  AuthsaeCallbackHelpers& operator=(AuthsaeCallbackHelpers&&) = delete;

 public:
  // Initialise the static event loop pointer
  static void init(fbzmq::ZmqEventLoop& zmqLoop);

  // Methods for controlling timeouts
  static int64_t addTimeoutToEventLoop(
      std::chrono::milliseconds timeout, fbzmq::TimeoutCallback callback);
  static void removeTimeoutFromEventLoop(int64_t timeoutId);

 private:
  static void verifyInitialized();

  static fbzmq::ZmqEventLoop* zmqLoop_;
};

} // namespace fbmeshd
} // namespace openr
