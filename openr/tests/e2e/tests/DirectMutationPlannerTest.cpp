/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <openr/tests/e2e/DirectMutationPlanner.h>
#include <openr/tests/e2e/TopologyBuilder.h>
#include <openr/tests/e2e/TopologyMutator.h>

namespace openr::e2e {

Topology
directTopology() {
  return TopologyBuilder{"direct"}
      .realNode("r1")
      .syntheticNode("r2")
      .link("r1", "r2")
      .build();
}

TEST(DirectMutationPlannerTest, Plan_DirectionalLatency_TargetsVirtualNetwork) {
  const auto before = directTopology();
  const auto changed = TopologyMutator::apply(
      before,
      {SetLinkLatency{"link_r1_r2_1", "r2", std::chrono::milliseconds{17}}});

  const auto actions = planDirectMutations(before, changed.topology);

  ASSERT_EQ(1, actions.size());
  EXPECT_EQ(DirectActionTarget::VIRTUAL_NETWORK, actions.front().target);
  EXPECT_EQ("r2", actions.front().node);
  const auto& latency = std::get<SetLinkLatency>(actions.front().mutation);
  EXPECT_EQ("r2", latency.node);
  EXPECT_EQ(std::chrono::milliseconds{17}, latency.latency);
}

TEST(DirectMutationPlannerTest, Plan_WithdrawPrefix_PreservesPrefixType) {
  PrefixOptions prefix;
  prefix.area = "0";
  prefix.type = thrift::PrefixType::BGP;
  const auto before = TopologyBuilder{"prefix"}
                          .realNode("r1")
                          .prefix("r1", "10.0.0.0/24", prefix)
                          .build();
  const auto changed = TopologyMutator::apply(
      before,
      {WithdrawPrefix{
          "r1",
          folly::IPAddress::createNetwork("10.0.0.0/24"),
          "0",
          thrift::PrefixType::BGP}});

  const auto actions = planDirectMutations(before, changed.topology);

  ASSERT_EQ(1, actions.size());
  const auto& withdrawal = std::get<WithdrawPrefix>(actions.front().mutation);
  EXPECT_EQ(thrift::PrefixType::BGP, withdrawal.type);
}

} // namespace openr::e2e
