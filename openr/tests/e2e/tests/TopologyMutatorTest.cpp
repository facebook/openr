/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <openr/tests/e2e/TopologyBuilder.h>
#include <openr/tests/e2e/TopologyMutator.h>

namespace openr::e2e {

Topology
lineTopology() {
  return TopologyBuilder{"line"}
      .realNode("r1")
      .syntheticNode("r2")
      .syntheticNode("r3")
      .link("r1", "r2")
      .link("r2", "r3")
      .build();
}

TEST(TopologyMutatorTest, Apply_BatchFailure_PreservesInput) {
  const auto topology = lineTopology();

  EXPECT_THROW(
      TopologyMutator::apply(
          topology,
          {SetLinkState{"link_r1_r2_1", false},
           SetLinkMetric{"missing", std::nullopt, 10}}),
      TopologyError);
  EXPECT_TRUE(topology.link("link_r1_r2_1").operational);
}

TEST(TopologyMutatorTest, Apply_DirectionalMetricAndPrefix_UpdatesState) {
  const auto topology = lineTopology();
  PrefixSpec prefix;
  prefix.prefix = folly::IPAddress::createNetwork("10.2.0.0/16");
  prefix.area = "0";

  const auto result = TopologyMutator::apply(
      topology,
      {SetLinkMetric{"link_r1_r2_1", "r1", 50}, AdvertisePrefix{"r2", prefix}});

  EXPECT_EQ(50, result.topology.links().front().first.metric);
  EXPECT_EQ(1, result.topology.links().front().second.metric);
  EXPECT_EQ(1, result.topology.node("r2").prefixes.size());
}

TEST(TopologyMutatorTest, Apply_UnknownNode_RejectsMutation) {
  const auto topology = lineTopology();

  EXPECT_THROW(
      TopologyMutator::apply(topology, {SetNodeState{"missing", false}}),
      TopologyError);
  EXPECT_TRUE(topology.node("r2").operational);
}

TEST(TopologyMutatorTest, Apply_NodeNotOnLink_RejectsDirectionalMetric) {
  const auto topology = lineTopology();

  EXPECT_THROW(
      TopologyMutator::apply(
          topology, {SetLinkMetric{"link_r1_r2_1", "r3", 50}}),
      TopologyError);
  EXPECT_EQ(1, topology.link("link_r1_r2_1").first.metric);
}

TEST(TopologyMutatorTest, Apply_WrongPrefixArea_RejectsWithdrawal) {
  const auto topology = TopologyBuilder{"line"}
                            .realNode("r1")
                            .syntheticNode("r2")
                            .prefix("r2", "10.2.0.0/16")
                            .link("r1", "r2")
                            .build();

  EXPECT_THROW(
      TopologyMutator::apply(
          topology,
          {WithdrawPrefix{
              "r2",
              folly::IPAddress::createNetwork("10.2.0.0/16"),
              "wrong-area",
              thrift::PrefixType::LOOPBACK}}),
      TopologyError);
  EXPECT_EQ(1, topology.node("r2").prefixes.size());
}

} // namespace openr::e2e
