/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrKvStoreService.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreServiceHandler.h>

namespace openr::e2e {

class OpenrKvStoreService::Impl {
 public:
  Impl() {
    socket_ =
        folly::AsyncServerSocket::UniquePtr{new folly::AsyncServerSocket()};
    socket_->setEnableReuseAddr(false);
    socket_->bind(std::vector<folly::IPAddress>{folly::IPAddress{"::1"}}, 0);

    folly::SocketAddress address;
    socket_->getAddress(&address);
    port_ = address.getPort();
  }

  uint16_t port_{0};
  folly::AsyncServerSocket::UniquePtr socket_;
  std::shared_ptr<KvStoreServiceHandler<E2eKvStoreClient>> handler_;
  std::shared_ptr<apache::thrift::ThriftServer> server_;
  apache::thrift::util::ScopedServerThread serverThread_;
  std::atomic<bool> running_{false};
};

OpenrKvStoreService::OpenrKvStoreService() : impl_{std::make_unique<Impl>()} {}

OpenrKvStoreService::~OpenrKvStoreService() {
  stop();
}

uint16_t
OpenrKvStoreService::port() const {
  return impl_->port_;
}

void
OpenrKvStoreService::start(E2eKvStore& kvStore, const std::string& nodeName) {
  if (impl_->running_.load()) {
    return;
  }
  if (!impl_->socket_) {
    throw std::logic_error("KvStore service cannot be restarted");
  }

  impl_->handler_ = std::make_shared<KvStoreServiceHandler<E2eKvStoreClient>>(
      nodeName, &kvStore);
  impl_->server_ = std::make_shared<apache::thrift::ThriftServer>();
  impl_->server_->setNumIOWorkerThreads(1);
  impl_->server_->setNumAcceptThreads(1);
  impl_->server_->setQueueTimeout(std::chrono::milliseconds::zero());
  impl_->server_->setInterface(impl_->handler_);
  impl_->server_->useExistingSocket(std::move(impl_->socket_));
  impl_->serverThread_.start(impl_->server_);
  impl_->running_.store(true);
}

void
OpenrKvStoreService::stop() {
  if (!impl_->running_.exchange(false)) {
    return;
  }
  impl_->serverThread_.stop();
  impl_->serverThread_.join();
  impl_->server_.reset();
  impl_->handler_.reset();
}

} // namespace openr::e2e
