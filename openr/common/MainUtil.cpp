// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "openr/common/MainUtil.h"

namespace openr {

bool
waitForFibService(const folly::EventBase& signalHandlerEvb, int port) {
  auto waitForFibStart = std::chrono::steady_clock::now();
  auto switchState = thrift::SwitchRunState::UNINITIALIZED;
  folly::EventBase evb;
  std::unique_ptr<openr::thrift::FibServiceAsyncClient> client;

  /*
   * Blocking wait with 2 conditions:
   *  - signalHandlerEvb is still running, aka, NO SIGINT/SIGQUIT/SIGTERM
   *  - switch is NOT ready to accept thrift request, aka, NOT CONFIGURED
   */
  while (signalHandlerEvb.isRunning() and
         thrift::SwitchRunState::CONFIGURED != switchState) {
    openr::Fib::createFibClient(evb, client, port);
    try {
      switchState = client->sync_getSwitchRunState();
    } catch (const std::exception& e) {
    }
    // sleep override
    std::this_thread::sleep_for(std::chrono::seconds(1));
    XLOG(INFO)
        << fmt::format("Waiting for FibService to come up via port: {}", port);
  }

  // signalHandlerEvb is terminated. Prepare for exit.
  if (thrift::SwitchRunState::CONFIGURED != switchState) {
    auto timeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - waitForFibStart)
                           .count();
    XLOG(INFO) << fmt::format(
        "Termination signal received. Waited for {}ms", timeElapsed);
    return false;
  }

  auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - waitForFibStart)
                    .count();
  XLOG(INFO) << fmt::format(
      "FibService is up on port {}. Waited for {}ms", port, waitMs);

  return true;
}

} // namespace openr
