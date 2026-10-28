/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <openr/tests/e2e/OpenrKvStoreService.h>
#include <openr/tests/e2e/OpenrModuleGraph.h>

namespace apache::thrift {
class ScopedServerInterfaceThread;
}

namespace openr {
class Decision;
class Dispatcher;
class Fib;
class LinkMonitor;
class OpenrCtrlHandler;
class OpenrEventBase;
class PersistentStore;
class PrefixManager;
class Spark;
} // namespace openr

namespace openr::e2e {

class OpenrModuleGraph::Impl {
 public:
  Impl(
      std::shared_ptr<const Config> config,
      OpenrQueueBundle& queues,
      TestPlatform& platform,
      OpenrKvStoreService& kvStoreService,
      std::shared_ptr<MockIoProvider> ioProvider,
      bool manageIoProvider);
  ~Impl();

  void start();
  void stop();

  std::shared_ptr<const Config> config_;
  OpenrQueueBundle& queues_;
  TestPlatform& platform_;
  OpenrKvStoreService& kvStoreService_;
  std::shared_ptr<MockIoProvider> ioProvider_;
  const bool manageIoProvider_;

  std::vector<std::thread> threads_;
  std::vector<std::unique_ptr<OpenrEventBase>> orderedModules_;
  std::unique_ptr<std::thread> ioThread_;
  std::shared_ptr<OpenrCtrlHandler> controlHandler_;
  std::unique_ptr<apache::thrift::ScopedServerInterfaceThread> controlServer_;

  PersistentStore* persistentStore_{nullptr};
  E2eKvStore* kvStore_{nullptr};
  Dispatcher* dispatcher_{nullptr};
  PrefixManager* prefixManager_{nullptr};
  Spark* spark_{nullptr};
  LinkMonitor* linkMonitor_{nullptr};
  Decision* decision_{nullptr};
  Fib* fib_{nullptr};
  std::atomic<bool> started_{false};
  std::atomic<bool> stopped_{false};
};

} // namespace openr::e2e
