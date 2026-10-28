/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>

namespace openr {
class Config;
class MockIoProvider;
} // namespace openr

namespace openr::e2e {

class OpenrKvStoreService;
class OpenrQueueBundle;
class TestPlatform;

class OpenrModuleGraph final {
 public:
  OpenrModuleGraph(
      std::shared_ptr<const Config> config,
      OpenrQueueBundle& queues,
      TestPlatform& platform,
      OpenrKvStoreService& kvStoreService,
      std::shared_ptr<MockIoProvider> ioProvider,
      bool manageIoProvider);
  ~OpenrModuleGraph();

  OpenrModuleGraph(const OpenrModuleGraph&) = delete;
  OpenrModuleGraph& operator=(const OpenrModuleGraph&) = delete;

  void start();
  void stop();
  uint16_t controlPort() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace openr::e2e
