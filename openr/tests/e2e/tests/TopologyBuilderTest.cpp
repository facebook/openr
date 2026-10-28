/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/IPAddress.h>
#include <gtest/gtest.h>
#include <openr/tests/e2e/TopologyBuilder.h>

namespace openr::e2e {

TEST(TopologyBuilderTest, Build_MixedTopology_PreservesIntent) {
  NodeOptions real;
  real.areas = {"pod", "plane"};
  NodeOptions synthetic;
  synthetic.areas = {"pod"};
  PrefixOptions prefix;
  prefix.area = "pod";
  LinkOptions primary;
  primary.name = "primary";
  primary.area = "pod";
  LinkOptions backup;
  backup.area = "pod";

  const auto topology = TopologyBuilder{"mixed"}
                            .router("r1", real)
                            .peer("r2", synthetic)
                            .prefix("r2", "10.2.1.1/16", prefix)
                            .link("r1", "r2", primary)
                            .link("r1", "r2", backup)
                            .build();

  EXPECT_EQ(1, topology.prefixCount());
  EXPECT_EQ(
      "10.2.0.0/16",
      folly::IPAddress::networkToString(
          topology.node("r2").prefixes.front().prefix));
  EXPECT_EQ("primary", topology.links().front().name);
  EXPECT_EQ("link_r1_r2_2", topology.links().at(1).name);
  EXPECT_EQ(NodeKind::REAL, topology.node("r1").kind);
  EXPECT_EQ(NodeKind::SYNTHETIC, topology.node("r2").kind);
}

TEST(TopologyBuilderTest, Prefix_InvalidNetwork_RejectsRequest) {
  TopologyBuilder builder{"invalid-prefix"};
  builder.realNode("r1");

  EXPECT_THROW(builder.prefix("not-a-prefix"), TopologyError);
}

} // namespace openr::e2e
