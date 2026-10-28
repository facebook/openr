/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/KvStoreVersionStore.h>

#include <limits>
#include <stdexcept>

#include <fmt/core.h>
#include <folly/container/F14Set.h>

namespace openr::e2e {

std::string
scopedKvStoreKey(const KvStoreKeyRef& key) {
  return fmt::format("{}:{}", key.area, key.key);
}

std::vector<AssignedVersion>
KvStoreVersionStore::next(const std::vector<KvStoreKeyRef>& keys) {
  folly::F14FastSet<Key> unique;
  for (const auto& key : keys) {
    if (key.area.empty() || key.key.empty() ||
        !unique.emplace(key.area, key.key).second) {
      throw std::invalid_argument("version batch contains an invalid key");
    }
  }

  auto versions = versions_.wlock();
  for (const auto& key : keys) {
    const auto it = versions->find({key.area, key.key});
    if (it != versions->end() &&
        it->second == std::numeric_limits<int64_t>::max()) {
      throw std::overflow_error("KvStore version exhausted");
    }
  }

  std::vector<AssignedVersion> assigned;
  assigned.reserve(keys.size());
  for (const auto& key : keys) {
    auto& version = (*versions)[{key.area, key.key}];
    assigned.push_back(AssignedVersion{{key.area, key.key}, ++version});
  }
  return assigned;
}

int64_t
KvStoreVersionStore::current(const KvStoreKeyRef& key) const {
  const auto versions = versions_.rlock();
  const auto it = versions->find({key.area, key.key});
  return it == versions->end() ? 0 : it->second;
}

} // namespace openr::e2e
