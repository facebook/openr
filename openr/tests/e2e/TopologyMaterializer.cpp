/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TopologyMaterializer.h>

#include <map>
#include <utility>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/tests/e2e/AdjacencyMaterializer.h>
#include <openr/tests/e2e/PrefixMaterializer.h>

namespace openr::e2e {

namespace {

using Key = std::pair<std::string, std::string>;
using SerializedMap = std::map<Key, SerializedKvStoreKey>;

struct PendingUpdate {
  SerializedKvStoreKey value;
  KvStoreUpdateType type{KvStoreUpdateType::SET};
};

SerializedMap
serialize(
    const Topology& topology,
    const folly::F14FastSet<std::string>& selected = {}) {
  SerializedMap result;
  auto append = [&](std::vector<SerializedKvStoreKey> keys) {
    for (auto& key : keys) {
      const Key scopedKey{key.key.area, key.key.key};
      result.emplace(scopedKey, std::move(key));
    }
  };
  append(materializeAdjacencies(topology, selected));
  append(materializePrefixes(topology, selected));
  return result;
}

std::vector<KvStoreUpdate>
assign(
    const std::vector<PendingUpdate>& changes, KvStoreVersionStore& versions) {
  std::vector<KvStoreKeyRef> keys;
  keys.reserve(changes.size());
  for (const auto& change : changes) {
    keys.push_back(change.value.key);
  }
  const auto assigned = versions.next(keys);

  std::vector<KvStoreUpdate> updates;
  updates.reserve(changes.size());
  for (size_t i = 0; i < changes.size(); ++i) {
    const auto& pending = changes.at(i);
    KvStoreUpdate update;
    update.key = pending.value.key;
    update.type = pending.type;
    update.version = assigned.at(i).version;
    update.value = createThriftValue(
        update.version,
        pending.value.originator,
        pending.value.data,
        Constants::kTtlInfinity,
        0,
        0);
    updates.push_back(std::move(update));
  }
  return updates;
}

} // namespace

TopologyMaterializer::TopologyMaterializer(KvStoreVersionStore& versions)
    : versions_{versions} {}

std::vector<KvStoreUpdate>
TopologyMaterializer::materialize(const Topology& topology) {
  std::vector<PendingUpdate> changes;
  const auto serialized = serialize(topology);
  for (const auto& [_, key] : serialized) {
    changes.push_back({key, KvStoreUpdateType::SET});
  }
  return assign(changes, versions_);
}

std::vector<KvStoreUpdate>
TopologyMaterializer::diff(
    const Topology& before,
    const Topology& after,
    const folly::F14FastSet<std::string>& affectedNodes) {
  const auto oldKeys = serialize(before, affectedNodes);
  const auto newKeys = serialize(after, affectedNodes);
  std::vector<PendingUpdate> changes;
  for (const auto& [key, value] : newKeys) {
    const auto old = oldKeys.find(key);
    if (old == oldKeys.end() || old->second.data != value.data) {
      changes.push_back({value, KvStoreUpdateType::SET});
    }
  }
  for (const auto& [key, value] : oldKeys) {
    if (!newKeys.contains(key)) {
      if (!value.withdrawalData.has_value()) {
        throw TopologyError("removed KvStore key has no withdrawal payload");
      }
      auto withdrawal = value;
      withdrawal.data = *value.withdrawalData;
      changes.push_back({std::move(withdrawal), KvStoreUpdateType::WITHDRAW});
    }
  }
  return assign(changes, versions_);
}

folly::F14FastMap<std::string, int64_t>
TopologyMaterializer::expectedVersions(
    const std::vector<KvStoreUpdate>& updates) {
  folly::F14FastMap<std::string, int64_t> result;
  for (const auto& update : updates) {
    result.emplace(scopedKvStoreKey(update.key), update.version);
  }
  return result;
}

} // namespace openr::e2e
