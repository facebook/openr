/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/tests/e2e/KvStoreVersionStore.h>
#include <openr/tests/e2e/Topology.h>

namespace openr::e2e {

enum class KvStoreUpdateType { SET, WITHDRAW };

struct KvStoreUpdate {
  KvStoreKeyRef key;
  KvStoreUpdateType type{KvStoreUpdateType::SET};
  int64_t version{0};
  std::optional<thrift::Value> value;
};

class TopologyMaterializer final {
 public:
  explicit TopologyMaterializer(KvStoreVersionStore& versions);

  std::vector<KvStoreUpdate> materialize(const Topology& topology);
  std::vector<KvStoreUpdate> diff(
      const Topology& before,
      const Topology& after,
      const folly::F14FastSet<std::string>& affectedNodes);
  static folly::F14FastMap<std::string, int64_t> expectedVersions(
      const std::vector<KvStoreUpdate>& updates);

 private:
  KvStoreVersionStore& versions_;
};

} // namespace openr::e2e
