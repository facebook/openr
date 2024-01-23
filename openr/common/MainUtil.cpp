/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

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
    } catch (const std::exception&) {
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

std::shared_ptr<apache::thrift::ThriftServer>
setUpThriftServer(
    std::shared_ptr<const Config> config,
    std::shared_ptr<openr::OpenrCtrlHandler>& handler,
    std::shared_ptr<wangle::SSLContextConfig> sslContext) {
  // Setup OpenrCtrl thrift server
  CHECK(handler);
  auto server = std::make_shared<apache::thrift::ThriftServer>();
  server->setInterface(handler);
  server->setNumIOWorkerThreads(1);
  // Intentionally kept this as (1). If you're changing to higher number please
  // address thread safety for private member variables in OpenrCtrlHandler
  server->setNumCPUWorkerThreads(1);
  // Enable TOS reflection on the server socket
  server->setTosReflect(true);
  // Set the port and interface for OpenrCtrl thrift server
  server->setPort(*config->getThriftServerConfig().openr_ctrl_port());
  // Set workers join timeout
  server->setWorkersJoinTimeout(std::chrono::seconds{
      *config->getThriftServerConfig().workers_join_timeout()});
  // Set the time the thrift requests are allowed to stay on the queue.
  // (if not set explicitly, the default value is 100ms)
  server->setQueueTimeout(Constants::kThriftServerQueueTimeout);

  // Setup TLS
  if (config->isSecureThriftServerEnabled()) {
    setupThriftServerTls(
        *server,
        config->getSSLThriftPolicy(),
        config->getSSLSeedPath(),
        sslContext);
  }
  return server;
}

void
waitTillStart(std::shared_ptr<apache::thrift::ThriftServer> server) {
  while (true) {
    auto evb = server->getServeEventBase();
    if (evb != nullptr and evb->isRunning()) {
      break;
    }
    std::this_thread::yield();
  }
}

} // namespace openr
