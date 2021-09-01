/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/OpenrThriftCtrlServer.h>

namespace openr {

void
OpenrThriftCtrlServer::start(
    std::shared_ptr<const Config> config,
    std::shared_ptr<openr::OpenrCtrlHandler>& ctrlHandler,
    std::shared_ptr<wangle::SSLContextConfig> sslContext) {
  // Setup OpenrCtrl thrift server
  CHECK(ctrlHandler);
  thriftCtrlServer_ = std::make_unique<apache::thrift::ThriftServer>();
  thriftCtrlServer_->setInterface(ctrlHandler);
  thriftCtrlServer_->setNumIOWorkerThreads(1);
  // Intentionally kept this as (1). If you're changing to higher number please
  // address thread safety for private member variables in OpenrCtrlHandler
  thriftCtrlServer_->setNumCPUWorkerThreads(1);
  // Enable TOS reflection on the server socket
  thriftCtrlServer_->setTosReflect(true);
  // Set the port and interface for OpenrCtrl thrift server
  thriftCtrlServer_->setPort(
      config->getThriftServerConfig().get_openr_ctrl_port());

  // Setup TLS
  if (config->isSecureThriftServerEnabled()) {
    setupThriftServerTls(
        *thriftCtrlServer_,
        config->getSSLThriftPolicy(),
        config->getSSLSeedPath(),
        sslContext);
  }

  // Serve
  thriftCtrlServerThread_ = std::thread([&]() noexcept {
    LOG(INFO) << "Starting ThriftCtrlServer thread ...";
    folly::setThreadName("openr-ThriftCtrlServer");
    thriftCtrlServer_->serve();
    LOG(INFO) << "ThriftCtrlServer thread got stopped.";
  });
  // Wait until thrift server starts
  while (true) {
    auto evb = thriftCtrlServer_->getServeEventBase();
    if (evb != nullptr and evb->isRunning()) {
      break;
    }
    std::this_thread::yield();
  }
}

void
OpenrThriftCtrlServer::stop() {
  // Stop & destroy thrift server. Will reduce ref-count on ctrlHandler
  thriftCtrlServer_->stop();
  thriftCtrlServerThread_.join();
  thriftCtrlServer_.reset();
}

} // namespace openr
