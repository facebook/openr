/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <folly/container/F14Set.h>
#include <openr/tests/e2e/Topology.h>
#include <openr/tests/e2e/TopologyMutationTypes.h>

namespace openr::e2e {

struct TopologyChangeSet {
  Topology topology;
  folly::F14FastSet<std::string> affectedNodes;
};

class TopologyMutator final {
 public:
  static TopologyChangeSet apply(
      const Topology& topology, const std::vector<TopologyMutation>& mutations);
};

} // namespace openr::e2e
