/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <folly/container/F14Set.h>
#include <openr/tests/e2e/KvStoreVersionStore.h>
#include <openr/tests/e2e/Topology.h>

namespace openr::e2e {

struct SerializedKvStoreKey {
  KvStoreKeyRef key;
  std::string originator;
  std::string data;
  std::optional<std::string> withdrawalData;
};

std::vector<SerializedKvStoreKey> materializeAdjacencies(
    const Topology& topology,
    const folly::F14FastSet<std::string>& selectedNodes = {});

} // namespace openr::e2e
