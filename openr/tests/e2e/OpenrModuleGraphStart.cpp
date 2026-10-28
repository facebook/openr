/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrModuleGraphInternal.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include <folly/SocketAddress.h>
#include <folly/container/F14Set.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/common/MainUtil.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/Decision.h>
#include <openr/dispatcher/Dispatcher.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/spark/Spark.h>
#include <openr/tests/e2e/OpenrQueueBundle.h>
#include <openr/tests/e2e/TestPlatform.h>
#include <openr/tests/mocks/MockIoProvider.h>

namespace openr::e2e {

void
OpenrModuleGraph::Impl::start() {
  if (stopped_.load()) {
    throw std::logic_error("Stopped Open/R module graph cannot be restarted");
  }
  if (started_.exchange(true)) {
    throw std::logic_error("Open/R module graph cannot be started twice");
  }

  auto readers = queues_.makeModuleReaders(config_->isQueueCoalescingEnabled());
  auto dispatcherInput = queues_.kvStoreUpdates.getReader("dispatcher");
  auto publications =
      queues_.makeDispatcherReaders(config_->isQueueCoalescingEnabled());

  try {
    persistentStore_ = startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-persistent-store",
        std::make_unique<PersistentStore>(config_));

    kvStore_ = startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-kvstore",
        std::make_unique<E2eKvStore>(
            queues_.kvStoreUpdates,
            std::move(readers.kvStorePeers),
            std::move(readers.kvStoreRequests),
            queues_.logSamples,
            config_->getAreaIds(),
            config_->toThriftKvStoreConfig(),
            config_->getFabricConfig()));
    kvStoreService_.start(*kvStore_, config_->getNodeName());

    dispatcher_ = startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-dispatcher",
        std::make_unique<Dispatcher>(
            std::move(dispatcherInput), queues_.dispatchedPublications));

    prefixManager_ = startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-prefix-manager",
        std::make_unique<PrefixManager>(
            queues_.staticRouteUpdates,
            queues_.kvRequests,
            queues_.prefixManagerInitializationEvents,
            std::move(publications.prefixManager),
            std::move(readers.prefixManagerPrefixes),
            std::move(readers.prefixManagerFibRoutes),
            config_));

    if (manageIoProvider_) {
      ioThread_ = std::make_unique<std::thread>(
          [ioProvider = ioProvider_] { ioProvider->start(); });
      ioProvider_->waitUntilRunning();
    }

    spark_ = startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-spark",
        std::make_unique<Spark>(
            std::move(readers.sparkInterfaces),
            std::move(readers.sparkInitialization),
            std::move(readers.sparkAddresses),
            queues_.neighborUpdates,
            ioProvider_,
            config_));

    auto linkMonitor = std::make_unique<LinkMonitor>(
        config_,
        &platform_.netlinkSocket(),
        persistentStore_,
        queues_.interfaceUpdates,
        queues_.prefixUpdates,
        queues_.peerUpdates,
        queues_.logSamples,
        queues_.kvRequests,
        std::move(publications.linkMonitor),
        std::move(readers.linkMonitorNeighbors),
        std::move(readers.linkMonitorNetlink));
    linkMonitor->setAsMockMode();
    linkMonitor_ = startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-link-monitor",
        std::move(linkMonitor));

    decision_ = startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-decision",
        std::make_unique<Decision>(
            config_,
            std::move(readers.decisionPeers),
            std::move(publications.decision),
            std::move(readers.decisionStaticRoutes),
            queues_.routeUpdates,
            queues_.kvRequests));

    fib_ = startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-fib",
        std::make_unique<Fib>(
            config_, std::move(readers.fibRoutes), queues_.fibRouteUpdates));

    auto controlEventBase = std::make_unique<OpenrEventBase>();
    controlHandler_ = std::make_shared<OpenrCtrlHandler>(
        config_->getNodeName(),
        folly::F14FastSet<std::string>{},
        controlEventBase.get(),
        decision_,
        fib_,
        kvStore_,
        linkMonitor_,
        nullptr,
        persistentStore_,
        prefixManager_,
        spark_,
        config_,
        dispatcher_);
    startEventBase(
        threads_,
        orderedModules_,
        nullptr,
        "e2e-control",
        std::move(controlEventBase));

    auto controlServer = std::make_shared<apache::thrift::ThriftServer>();
    controlServer->setInterface(controlHandler_);
    controlServer->setAddress(folly::SocketAddress("::1", 0));
    controlServer->setQueueTimeout(std::chrono::milliseconds::zero());
    controlServer_ =
        std::make_unique<apache::thrift::ScopedServerInterfaceThread>(
            controlServer);
  } catch (...) {
    stop();
    throw;
  }
}

void
OpenrModuleGraph::start() {
  impl_->start();
}

} // namespace openr::e2e
