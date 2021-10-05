/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/OpenrThriftCtrlServer.h>

namespace openr {

OpenrThriftCtrlServer::OpenrThriftCtrlServer(
    std::shared_ptr<const Config> config,
    std::shared_ptr<openr::OpenrCtrlHandler>& handler,
    std::shared_ptr<wangle::SSLContextConfig> sslContext)
    : config_{config}, ctrlHandler_{handler}, sslContext_{sslContext} {}

void
OpenrThriftCtrlServer::start() {
  // Setup OpenrCtrl thrift server
  CHECK(ctrlHandler_);
  auto server = std::make_unique<apache::thrift::ThriftServer>();
  server->setInterface(ctrlHandler_);
  server->setNumIOWorkerThreads(1);
  // Intentionally kept this as (1). If you're changing to higher number please
  // address thread safety for private member variables in OpenrCtrlHandler
  server->setNumCPUWorkerThreads(1);
  // Enable TOS reflection on the server socket
  server->setTosReflect(true);
  // Set the port and interface for OpenrCtrl thrift server
  server->setPort(config_->getThriftServerConfig().get_openr_ctrl_port());

  // Setup TLS
  if (config_->isSecureThriftServerEnabled()) {
    setupThriftServerTls(
        *server,
        config_->getSSLThriftPolicy(),
        config_->getSSLSeedPath(),
        sslContext_);
  }

  // Serve
  thriftCtrlServerThreadVec_.emplace_back(std::thread([&]() noexcept {
    LOG(INFO) << "Starting ThriftCtrlServer thread ...";
    folly::setThreadName("openr-ThriftCtrlServer");
    server->serve();
    LOG(INFO) << "ThriftCtrlServer thread got stopped.";
  }));

  // Wait until thrift server starts
  while (true) {
    auto evb = server->getServeEventBase();
    if (evb != nullptr and evb->isRunning()) {
      break;
    }
    std::this_thread::yield();
  }

  // Add to the server vector
  thriftCtrlServerVec_.emplace_back(std::move(server));
}

void
OpenrThriftCtrlServer::stop() {
  // Stop & destroy thrift server. Will reduce ref-count on ctrlHandler
  for (auto& server : thriftCtrlServerVec_) {
    server->stop();
  }
  // Wait for all threads
  for (auto& thread : thriftCtrlServerThreadVec_) {
    thread.join();
  }
  for (auto& server : thriftCtrlServerVec_) {
    server.reset();
  }
}

} // namespace openr
