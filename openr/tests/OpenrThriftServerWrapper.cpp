/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
    Monitor* monitor,
    PersistentStore* configStore,
    PrefixManager* prefixManager,
    Spark* spark,
    std::shared_ptr<const Config> config)
    : nodeName_(nodeName),
      decision_(decision),
      fib_(fib),
      kvStore_(kvStore),
      linkMonitor_(linkMonitor),
      monitor_(monitor),
      configStore_(configStore),
      prefixManager_(prefixManager),
      spark_(spark),
      config_(config) {
  CHECK(!nodeName_.empty());
}

void
OpenrThriftServerWrapper::run() {
  // create openrCtrlHandler
  ctrlHandler_ = std::make_shared<OpenrCtrlHandler>(
      nodeName_,
      std::unordered_set<std::string>{},
      &evb_,
      decision_,
      fib_,
      kvStore_,
      linkMonitor_,
      monitor_,
      configStore_,
      prefixManager_,
      spark_,
      config_);
  // Create main-event-loop
  evbThread_ = std::thread([&]() { evb_.run(); });
  evb_.waitUntilRunning();

  // setup openrCtrlThrift server for client to connect to
  std::shared_ptr<apache::thrift::ThriftServer> server =
      std::make_shared<apache::thrift::ThriftServer>();
  server->setNumIOWorkerThreads(1);
  server->setNumAcceptThreads(1);
  server->setPort(0);
  server->setInterface(ctrlHandler_);
  thriftServerThread_.start(std::move(server));

  LOG(INFO) << "Successfully started openr-ctrl thrift server";
}

void
OpenrThriftServerWrapper::stop() {
  // ATTN: it is user's responsibility to close the queue passed
  //       to OpenrThrifyServerWrapper before calling stop()
  thriftServerThread_.stop();
  thriftServerThread_.join();

  LOG(INFO) << "Stopping openr-ctrl handler";
  CHECK(ctrlHandler_.unique()) << "Unexpected ownership of ctrlHandler";
  ctrlHandler_.reset();

  LOG(INFO) << "Stopping ctrl eventbase";
  evb_.stop();
  evb_.waitUntilStopped();
  evbThread_.join();

  LOG(INFO) << "Successfully stopped openr-ctrl thrift server";
}

} // namespace openr
