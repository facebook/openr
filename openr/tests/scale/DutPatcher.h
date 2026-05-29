/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include <openr/tests/scale/TopologyGenerator.h>
#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer_types.h>

namespace openr {

/*
 * DutPatcher mutates a generated Topology to inject DUT-specific test state.
 *
 * Where TopologyGenerator builds a fabric topology from scratch, DutPatcher
 * operates on an already-generated Topology and stitches the DUT into it:
 *   - computes the DUT's expected peer list based on dutRole + topology shape
 *   - removes the placeholder leaf-0 when the DUT replaces it
 *   - inserts the DUT as a router and adds neighbor->DUT adjacencies
 *
 * All methods are pure functions of their inputs; the class exists only as
 * a discoverable grouping (no state).
 */
class DutPatcher {
 public:
  // Compute the list of node names that the DUT will peer with, based on its
  // role and the configured topology shape.
  static std::vector<std::string> buildDutNeighborNames(
      const thrift::ScaleTestConfig& cfg);

  // When the DUT is a leaf it replaces leaf-0 in the topology. Strip that
  // node and any adjacencies/interfaces that reference it.
  static void stripReplacedLeaf(Topology& topo);

  // Insert the DUT into the topology and append a neighbor->DUT adjacency to
  // each of the DUT's neighbors. Neighbors are distributed in contiguous
  // blocks across the configured interfaces (`numNeighbors / numInterfaces`
  // per interface; the last interface absorbs the remainder) so each
  // interface carries roughly equal load.
  static void patchDutIntoTopology(
      Topology& topo,
      const std::string& dutNodeName,
      const std::vector<std::string>& dutNeighborNames,
      const std::vector<std::string>& interfaces);
};

} // namespace openr
