/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <openr/tests/e2e/TopologyAreaConfig.h>
#include <openr/tests/e2e/TopologyBuilder.h>

namespace openr::e2e {
namespace {

TEST(TopologyAreaConfigTest, AssignsEachNeighborToTheConfiguredArea) {
  NodeOptions areas;
  areas.areas = {"pod", "plane"};
  LinkOptions pod;
  pod.area = "pod";
  LinkOptions plane;
  plane.area = "plane";
  const auto topology = TopologyBuilder{"two-areas"}
                            .realNode("r1", areas)
                            .syntheticNode("s1", areas)
                            .syntheticNode("s2", areas)
                            .link("r1", "s1", pod)
                            .link("r1", "s2", plane)
                            .build();

  const auto configured =
      makeTopologyAreaConfigs(topology, topology.node("r1"));

  ASSERT_EQ(2, configured.size());
  EXPECT_EQ("pod", *configured[0].area_id());
  EXPECT_EQ(
      std::vector<std::string>{"^s1$"}, *configured[0].neighbor_regexes());
  EXPECT_EQ("plane", *configured[1].area_id());
  EXPECT_EQ(
      std::vector<std::string>{"^s2$"}, *configured[1].neighbor_regexes());
}

TEST(TopologyAreaConfigTest, SetsRealLinkAndSegmentMetricsByInterface) {
  LinkOptions link;
  link.first.interfaceName = "eth.0";
  link.first.metric = 25;
  SegmentOptions segment;
  segment.metric = 7;
  const auto topology = TopologyBuilder{"router-interface-metrics"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("s2")
                            .syntheticNode("s3")
                            .link("r1", "s1", link)
                            .sharedSegment("lan", {"r1", "s2", "s3"}, segment)
                            .build();

  const auto metrics =
      makeTopologyInterfaceMetrics(topology, topology.node("r1"));

  ASSERT_EQ(2, metrics.size());
  EXPECT_EQ(25, *metrics[0].metric());
  EXPECT_EQ(
      std::vector<std::string>{"^eth\\.0$"}, *metrics[0].interface_regexes());
  EXPECT_EQ(7, *metrics[1].metric());
  EXPECT_EQ(
      std::vector<std::string>{"^if_r1_lan$"}, *metrics[1].interface_regexes());
}

} // namespace
} // namespace openr::e2e
