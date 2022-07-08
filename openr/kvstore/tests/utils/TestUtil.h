/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/kvstore/KvStoreWrapper.h>

namespace openr {
namespace util {

std::string genNodeName(size_t i);

enum class ClusterTopology {
  LINEAR = 0,
  RING = 1,
  // TODO: add more topo
};

void generateTopo(
    const std::vector<std::unique_ptr<::openr::KvStoreWrapper<
        ::openr::thrift::KvStoreServiceAsyncClient>>>& stores,
    ClusterTopology topo);

void validateLastNodeKey(
    size_t numExpectedKey,
    ::openr::KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* node);

} // namespace util
} // namespace openr
