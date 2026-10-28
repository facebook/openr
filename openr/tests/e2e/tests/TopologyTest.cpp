/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <openr/tests/e2e/Topology.h>

namespace openr::e2e {

TEST(TopologyTest, Construction_ValidNodes_PreservesOrderAndCounts) {
  NodeSpec real;
  real.name = "r1";
  real.kind = NodeKind::REAL;
  NodeSpec synthetic;
  synthetic.name = "r2";

  const Topology topology{"two-nodes", {real, synthetic}};

  EXPECT_EQ((std::vector<std::string>{"r1", "r2"}), topology.nodeNames());
  EXPECT_EQ(1, topology.realNodeCount());
  EXPECT_EQ(NodeKind::REAL, topology.node("r1").kind);
  EXPECT_THROW(topology.node("missing"), TopologyError);
}

TEST(TopologyTest, Construction_DuplicateNode_Throws) {
  NodeSpec node;
  node.name = "r1";

  EXPECT_THROW((Topology{"duplicate", {node, node}}), TopologyError);
}

TEST(TopologyTest, Lookup_UnknownLink_Throws) {
  NodeSpec node;
  node.name = "r1";
  const Topology topology{"single-node", {node}};

  EXPECT_THROW(topology.link("missing"), TopologyError);
}

TEST(TopologyTest, Lookup_UnknownSegment_Throws) {
  NodeSpec node;
  node.name = "r1";
  const Topology topology{"single-node", {node}};

  EXPECT_THROW(topology.segment("missing"), TopologyError);
}

} // namespace openr::e2e
