/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <gtest/gtest.h>
#include <openr/common/Util.h>
#include <openr/tests/e2e/AdjacencyMaterializer.h>
#include <openr/tests/e2e/TopologyAllocator.h>
#include <openr/tests/e2e/TopologyBuilder.h>

namespace openr::e2e {

TEST(AdjacencyMaterializerTest, Materialize_SyntheticNodesOnly_UsesAreas) {
  NodeOptions pod;
  pod.areas = {"pod"};
  LinkOptions link;
  link.area = "pod";
  link.metric = 20;
  const auto topology = TopologyAllocator::allocate(
      TopologyBuilder{"materialize"}
          .realNode("r1", pod)
          .syntheticNode("r2", pod)
          .syntheticNode("r3", pod)
          .link("r1", "r2", link)
          .link("r2", "r3", link)
          .link("r2", "r3", link)
          .build());

  const auto keys = materializeAdjacencies(topology);

  ASSERT_EQ(2, keys.size());
  EXPECT_EQ("adj:r2", keys.at(0).key.key);
  EXPECT_EQ("pod", keys.at(0).key.area);
  apache::thrift::CompactSerializer serializer;
  const auto database =
      readThriftObjStr<thrift::AdjacencyDatabase>(keys.at(0).data, serializer);
  EXPECT_EQ("r2", *database.thisNodeName());
  EXPECT_EQ(3, database.adjacencies()->size());
  EXPECT_EQ(20, *database.adjacencies()->front().metric());
  EXPECT_EQ("adj:r3", keys.at(1).key.key);
}

TEST(
    AdjacencyMaterializerTest, Materialize_AsymmetricLatency_UsesRoundTripSum) {
  LinkOptions link;
  link.first.latency = std::chrono::milliseconds{1};
  link.second.latency = std::chrono::milliseconds{9};
  const auto topology = TopologyAllocator::allocate(
      TopologyBuilder{"latency"}
          .syntheticNode("r1")
          .syntheticNode("r2")
          .link("r1", "r2", link)
          .build());

  const auto keys = materializeAdjacencies(topology);

  ASSERT_EQ(2, keys.size());
  apache::thrift::CompactSerializer serializer;
  for (const auto& key : keys) {
    const auto database =
        readThriftObjStr<thrift::AdjacencyDatabase>(key.data, serializer);
    ASSERT_EQ(1, database.adjacencies()->size());
    EXPECT_EQ(10000, *database.adjacencies()->front().rtt());
  }
}

} // namespace openr::e2e
