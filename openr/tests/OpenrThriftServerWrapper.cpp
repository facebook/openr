/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/tests/OpenrThriftServerWrapper.h"

namespace openr {

OpenrThriftServerWrapper::OpenrThriftServerWrapper(
    std::string const& nodeName,
    Decision* decision,
    Fib* fib,
    KvStore* kvStore,
    LinkMonitor* linkMonitor,
    PersistentStore* configStore,
    PrefixManager* prefixManager,
    MonitorSubmitUrl const& monitorSubmitUrl,
    KvStoreLocalPubUrl const& kvStoreLocalPubUrl,
    fbzmq::Context& context)
    : nodeName_(nodeName),
      monitorSubmitUrl_(monitorSubmitUrl),
      kvStoreLocalPubUrl_(kvStoreLocalPubUrl),
      context_(context),
      decision_(decision),
      fib_(fib),
      kvStore_(kvStore),
      linkMonitor_(linkMonitor),
      configStore_(configStore),
      prefixManager_(prefixManager) {
  CHECK(!nodeName_.empty());
}

void
OpenrThriftServerWrapper::run() {
  // Create main-event-loop
  mainEvlThread_ = std::thread([&]() { mainEvl_.run(); });
  mainEvl_.waitUntilRunning();

  tm_ = apache::thrift::concurrency::ThreadManager::newSimpleThreadManager(
      1, false);
  tm_->threadFactory(
      std::make_shared<apache::thrift::concurrency::PosixThreadFactory>());
  tm_->start();

  // create openrCtrlHandler
  openrCtrlHandler_ = std::make_shared<OpenrCtrlHandler>(
      nodeName_,
      std::unordered_set<std::string>{},
      decision_,
      fib_,
      kvStore_,
      linkMonitor_,
      configStore_,
      prefixManager_,
      monitorSubmitUrl_,
      kvStoreLocalPubUrl_,
      mainEvl_,
      context_);
  openrCtrlHandler_->setThreadManager(tm_.get());

  // setup openrCtrlThrift server for client to connect to
  std::shared_ptr<apache::thrift::ThriftServer> server =
      std::make_shared<apache::thrift::ThriftServer>();
  server->setNumIOWorkerThreads(1);
  server->setNumAcceptThreads(1);
  server->setPort(0);
  server->setInterface(openrCtrlHandler_);
  openrCtrlThriftServerThread_.start(std::move(server));

  LOG(INFO) << "Successfully started openr-ctrl thrift server";
}

void
OpenrThriftServerWrapper::stop() {
  mainEvl_.stop();
  mainEvlThread_.join();
  openrCtrlHandler_.reset();
  tm_->join();
  openrCtrlThriftServerThread_.stop();

  LOG(INFO) << "Successfully stopped openr-ctrl thrift server";
}

} // namespace openr
