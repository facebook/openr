/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>

namespace openr {
template <typename ClientType>
class KvStore;
} // namespace openr

namespace openr::e2e {

using E2eKvStoreClient = apache::thrift::Client<thrift::OpenrCtrlCpp>;
using E2eKvStore = KvStore<E2eKvStoreClient>;

class OpenrKvStoreService final {
 public:
  OpenrKvStoreService();
  ~OpenrKvStoreService();

  OpenrKvStoreService(const OpenrKvStoreService&) = delete;
  OpenrKvStoreService& operator=(const OpenrKvStoreService&) = delete;

  uint16_t port() const;
  void start(E2eKvStore& kvStore, const std::string& nodeName);
  void stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace openr::e2e
