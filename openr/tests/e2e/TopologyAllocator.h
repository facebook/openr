/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>

#include <openr/tests/e2e/Topology.h>

namespace openr::e2e {

struct AllocationOptions {
  int32_t firstNodeLabel{100001};
  int32_t firstAdjacencyLabel{50001};
};

class TopologyAllocator final {
 public:
  static Topology allocate(
      const Topology& topology, AllocationOptions options = {});
};

} // namespace openr::e2e
