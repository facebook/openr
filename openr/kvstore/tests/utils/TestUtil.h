/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Generator.h>
#include <folly/experimental/coro/Task.h>

#include <folly/Conv.h>

#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreWrapper.h>

namespace openr {
namespace util {

std::string genNodeName(size_t i);

enum class ClusterTopology {
  LINEAR = 0,
  RING = 1,
  STAR = 2,
  // TODO: add more topo
};

void generateTopo(
    const std::vector<std::unique_ptr<::openr::KvStoreWrapper<
        ::openr::thrift::KvStoreServiceAsyncClient>>>& stores,
    ClusterTopology topo);

// For the given node, validate if it has received all events
folly::coro::Task<void> co_validateNodeKey(
    const std::unordered_map<std::string, ::openr::thrift::Value>& events,
    ::openr::KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* node);

// Wait until ALL nodes in `stores` have received all events
folly::coro::Task<void> co_waitForConvergence(
    const std::unordered_map<std::string, ::openr::thrift::Value>& events,
    const std::vector<std::unique_ptr<
        ::openr::KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>>>&
        stores);

} // namespace util
} // namespace openr
