/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>

namespace openr::e2e {

struct KvStoreKeyRef {
  std::string area;
  std::string key;
};

struct AssignedVersion : KvStoreKeyRef {
  int64_t version{0};
};

std::string scopedKvStoreKey(const KvStoreKeyRef& key);

class KvStoreVersionStore final {
 public:
  std::vector<AssignedVersion> next(const std::vector<KvStoreKeyRef>& keys);
  int64_t current(const KvStoreKeyRef& key) const;

 private:
  using Key = std::pair<std::string, std::string>;
  folly::Synchronized<folly::F14FastMap<Key, int64_t>> versions_;
};

} // namespace openr::e2e
