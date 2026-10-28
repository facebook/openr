/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrModuleGraphInternal.h>

#include <stdexcept>
#include <utility>

#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/common/OpenrEventBase.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/tests/e2e/OpenrQueueBundle.h>
#include <openr/tests/mocks/MockIoProvider.h>

namespace openr::e2e {

OpenrModuleGraph::Impl::Impl(
    std::shared_ptr<const Config> config,
    OpenrQueueBundle& queues,
    TestPlatform& platform,
    OpenrKvStoreService& kvStoreService,
    std::shared_ptr<MockIoProvider> ioProvider,
    bool manageIoProvider)
    : config_{std::move(config)},
      queues_{queues},
      platform_{platform},
      kvStoreService_{kvStoreService},
      ioProvider_{std::move(ioProvider)},
      manageIoProvider_{manageIoProvider} {
  if (!config_ || !ioProvider_) {
    throw std::invalid_argument(
        "Open/R graph requires config and I/O provider");
  }
}

OpenrModuleGraph::Impl::~Impl() = default;

OpenrModuleGraph::OpenrModuleGraph(
    std::shared_ptr<const Config> config,
    OpenrQueueBundle& queues,
    TestPlatform& platform,
    OpenrKvStoreService& kvStoreService,
    std::shared_ptr<MockIoProvider> ioProvider,
    bool manageIoProvider)
    : impl_{std::make_unique<Impl>(
          std::move(config),
          queues,
          platform,
          kvStoreService,
          std::move(ioProvider),
          manageIoProvider)} {}

OpenrModuleGraph::~OpenrModuleGraph() {
  stop();
}

void
OpenrModuleGraph::Impl::stop() {
  if (stopped_.exchange(true)) {
    return;
  }

  queues_.close();
  if (controlHandler_) {
    controlHandler_->stop();
  }
  controlServer_.reset();
  controlHandler_.reset();
  kvStoreService_.stop();

  for (auto module = orderedModules_.rbegin(); module != orderedModules_.rend();
       ++module) {
    (*module)->stop();
    (*module)->waitUntilStopped();
  }
  if (manageIoProvider_ && ioThread_) {
    ioProvider_->stop();
    ioThread_->join();
    ioThread_.reset();
  }
  for (auto& thread : threads_) {
    thread.join();
  }
  threads_.clear();
  orderedModules_.clear();
}

void
OpenrModuleGraph::stop() {
  impl_->stop();
}

uint16_t
OpenrModuleGraph::controlPort() const {
  if (!impl_->controlServer_) {
    throw std::logic_error("Open/R control server is not running");
  }
  return impl_->controlServer_->getPort();
}

} // namespace openr::e2e
