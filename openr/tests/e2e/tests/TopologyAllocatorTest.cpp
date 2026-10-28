/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>
#include <openr/tests/e2e/TopologyAllocator.h>
#include <openr/tests/e2e/TopologyBuilder.h>

namespace openr::e2e {

TEST(TopologyAllocatorTest, Allocate_MissingValues_IsDeterministicAndUnique) {
  const auto input = TopologyBuilder{"allocation"}
                         .realNode("r1")
                         .syntheticNode("r2")
                         .syntheticNode("r3")
                         .link("r1", "r2")
                         .sharedSegment("lan", {"r1", "r3"})
                         .build();

  const auto first = TopologyAllocator::allocate(input);
  const auto second = TopologyAllocator::allocate(input);

  ASSERT_TRUE(first.node("r1").nodeLabel.has_value());
  EXPECT_EQ(first.node("r1").nodeLabel, second.node("r1").nodeLabel);
  EXPECT_NE(first.node("r1").nodeLabel, first.node("r2").nodeLabel);
  EXPECT_EQ(100001, *first.node("r1").nodeLabel);
  ASSERT_TRUE(first.links().front().first.interfaceIndex.has_value());
  ASSERT_TRUE(
      first.segments().front().endpoints.front().interfaceIndex.has_value());
  EXPECT_EQ(1, *first.links().front().first.interfaceIndex);
  EXPECT_EQ(2, *first.segments().front().endpoints.front().interfaceIndex);
  EXPECT_EQ(2, first.links().front().first.addresses.size());
  EXPECT_EQ(
      folly::IPAddress("fe80::1:1"),
      first.links().front().first.addresses.at(0).first);
  EXPECT_EQ(
      folly::IPAddress("10.0.1.0"),
      first.links().front().first.addresses.at(1).first);
  EXPECT_EQ(31, first.links().front().first.addresses.at(1).second);
  EXPECT_TRUE(first.links().front().second.addresses.at(1).first.inSubnet(
      first.links().front().first.addresses.at(1).first, 31));
  EXPECT_EQ(
      first.links().front().first.addresses,
      second.links().front().first.addresses);
  EXPECT_NE(
      first.links().front().first.adjacencyLabel,
      first.segments().front().endpoints.front().adjacencyLabel);
}

TEST(TopologyAllocatorTest, Allocate_ExplicitValues_PreservesValues) {
  LinkOptions link;
  link.first.interfaceIndex = 9;
  link.first.adjacencyLabel = 60001;
  link.first.addresses = {folly::IPAddress::createNetwork("fe80::99/128")};
  NodeOptions node;
  node.nodeLabel = 200001;
  const auto input = TopologyBuilder{"explicit"}
                         .realNode("r1", node)
                         .syntheticNode("r2")
                         .link("r1", "r2", link)
                         .build();

  const auto output = TopologyAllocator::allocate(input);

  ASSERT_TRUE(output.node("r1").nodeLabel.has_value());
  ASSERT_TRUE(output.links().front().first.interfaceIndex.has_value());
  ASSERT_TRUE(output.links().front().first.adjacencyLabel.has_value());
  EXPECT_EQ(200001, *output.node("r1").nodeLabel);
  EXPECT_EQ(9, *output.links().front().first.interfaceIndex);
  EXPECT_EQ(60001, *output.links().front().first.adjacencyLabel);
  EXPECT_EQ(
      folly::IPAddress("fe80::99"),
      output.links().front().first.addresses.front().first);
  EXPECT_EQ(2, output.links().front().first.addresses.size());
}

TEST(TopologyAllocatorTest, Allocate_InvalidLabelRanges_Throws) {
  const auto topology = TopologyBuilder{"ranges"}.realNode("r1").build();
  AllocationOptions options;
  options.firstNodeLabel = 0;
  EXPECT_THROW(TopologyAllocator::allocate(topology, options), TopologyError);
  options.firstNodeLabel = 100001;
  options.firstAdjacencyLabel = -1;
  EXPECT_THROW(TopologyAllocator::allocate(topology, options), TopologyError);
}

TEST(TopologyAllocatorTest, Allocate_DuplicateNodeLabel_Throws) {
  NodeOptions node;
  node.nodeLabel = 100001;
  const auto topology = TopologyBuilder{"node-labels"}
                            .realNode("r1", node)
                            .syntheticNode("s1", node)
                            .build();
  EXPECT_THROW(TopologyAllocator::allocate(topology), TopologyError);
}

TEST(TopologyAllocatorTest, Allocate_DuplicateAdjacencyLabel_Throws) {
  LinkOptions options;
  options.first.adjacencyLabel = 60001;
  const auto topology = TopologyBuilder{"adjacency-labels"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("s2")
                            .link("r1", "s1", options)
                            .link("r1", "s2", options)
                            .build();
  EXPECT_THROW(TopologyAllocator::allocate(topology), TopologyError);
}

TEST(TopologyAllocatorTest, DuplicateInterfaceIndexRejectedBeforeAllocation) {
  LinkOptions options;
  options.first.interfaceIndex = 9;
  EXPECT_THROW(
      TopologyBuilder{"interface-indexes"}
          .realNode("r1")
          .syntheticNode("s1")
          .syntheticNode("s2")
          .link("r1", "s1", options)
          .link("r1", "s2", options)
          .build(),
      TopologyError);
}

TEST(TopologyAllocatorTest, Allocate_ExhaustedLabelRanges_Throws) {
  const auto topology = TopologyBuilder{"exhausted"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .link("r1", "s1")
                            .build();
  AllocationOptions options;
  options.firstNodeLabel = std::numeric_limits<int32_t>::max();
  EXPECT_THROW(TopologyAllocator::allocate(topology, options), TopologyError);
  options.firstNodeLabel = 100001;
  options.firstAdjacencyLabel = std::numeric_limits<int32_t>::max();
  EXPECT_THROW(TopologyAllocator::allocate(topology, options), TopologyError);
}

} // namespace openr::e2e
