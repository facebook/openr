/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <chrono>

#include <openr/tests/e2e/OpenrE2e.h>
#include <openr/tests/e2e/TopologyBuilder.h>

namespace openr::e2e {
namespace {

TEST(MultiHopTopologyE2ETest, InitialRouterMetricSelectsProgrammedNextHop) {
  LinkOptions expensive;
  expensive.name = "r1-s1";
  expensive.first.metric = 25;
  LinkOptions alternate;
  alternate.name = "s2-dst";
  alternate.metric = 10;
  constexpr char kPrefix[] = "2001:db8:110::/64";
  const auto topology = TopologyBuilder{"initial-router-metric"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("s2")
                            .syntheticNode("dst")
                            .prefix("dst", kPrefix)
                            .link("r1", "s1", expensive)
                            .link("r1", "s2")
                            .link("s1", "dst")
                            .link("s2", "dst", alternate)
                            .build();
  OpenrE2eLab lab{topology};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  EXPECT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s2"}, kTimeout));
}

constexpr char kMultiHopPrefix[] = "2001:db8:100::/64";

Topology
multiHopTopology() {
  LinkOptions toFirst;
  toFirst.name = "r1-s1";
  LinkOptions toSecond;
  toSecond.name = "r1-s2";
  LinkOptions viaFirst;
  viaFirst.name = "s1-dst";
  viaFirst.metric = 1;
  LinkOptions viaSecond;
  viaSecond.name = "s2-dst";
  viaSecond.metric = 20;
  return TopologyBuilder{"multi-hop"}
      .realNode("r1")
      .syntheticNode("s1")
      .syntheticNode("s2")
      .syntheticNode("dst")
      .prefix("dst", kMultiHopPrefix)
      .link("r1", "s1", toFirst)
      .link("r1", "s2", toSecond)
      .link("s1", "dst", viaFirst)
      .link("s2", "dst", viaSecond)
      .build();
}

TEST(MultiHopTopologyE2ETest, RemoteMetricChangeMovesProgrammedNextHop) {
  OpenrE2eLab lab{multiHopTopology()};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  ASSERT_TRUE(lab.awaitAdjacency("s1", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("s2", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitProgrammedRoute(kMultiHopPrefix, {"s1"}, kTimeout));

  lab.setLinkMetric("s1-dst", "s1", 50);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kMultiHopPrefix, {"s2"}, kTimeout));

  lab.setLinkMetric("s1-dst", "s1", 1);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kMultiHopPrefix, {"s1"}, kTimeout));
}

TEST(MultiHopTopologyE2ETest, RemoteLinkFailureMovesProgrammedNextHop) {
  OpenrE2eLab lab{multiHopTopology()};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  ASSERT_TRUE(lab.awaitProgrammedRoute(kMultiHopPrefix, {"s1"}, kTimeout));

  lab.setLinkState("r1-s1", /*isUp=*/false);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kMultiHopPrefix, {"s2"}, kTimeout));
  lab.setLinkState("r1-s1", /*isUp=*/true);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kMultiHopPrefix, {"s1"}, kTimeout));
}

TEST(MultiHopTopologyE2ETest, RemotePartitionWithdrawsAndRestoresRoute) {
  OpenrE2eLab lab{multiHopTopology()};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  ASSERT_TRUE(lab.awaitProgrammedRoute(kMultiHopPrefix, {"s1"}, kTimeout));

  lab.setPartition({"s1-dst", "s2-dst"}, /*isPartitioned=*/true);
  ASSERT_TRUE(lab.awaitRouteAbsent(kMultiHopPrefix, kTimeout));
  lab.setPartition({"s1-dst", "s2-dst"}, /*isPartitioned=*/false);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kMultiHopPrefix, {"s1"}, kTimeout));
}

TEST(MultiHopTopologyE2ETest, NamedPeerPrefixArrivesAndWithdraws) {
  const auto topology = TopologyBuilder{"dynamic-prefix"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("dst")
                            .link("r1", "s1")
                            .link("s1", "dst")
                            .build();
  OpenrE2eLab lab{topology};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  constexpr char kPrefix[] = "2001:db8:300::/64";
  ASSERT_TRUE(lab.awaitAdjacency("s1", /*isUp=*/true, kTimeout));
  PrefixOptions options;
  options.area = "0";
  options.tags = {"customer"};
  lab.advertisePrefix("dst", kPrefix, options);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1"}, kTimeout));

  lab.withdrawPrefix("dst", kPrefix, options);
  EXPECT_TRUE(lab.awaitRouteAbsent(kPrefix, kTimeout));
}

TEST(MultiHopTopologyE2ETest, PeerAndInterfaceOverloadRerouteTraffic) {
  LinkOptions direct;
  direct.name = "r1-s1";
  LinkOptions viaFirst;
  viaFirst.name = "s1-dst";
  LinkOptions viaSecond;
  viaSecond.metric = 20;
  const auto topology = TopologyBuilder{"overload"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("s2")
                            .syntheticNode("dst")
                            .prefix("dst", "2001:db8:600::/64")
                            .link("r1", "s1", direct)
                            .link("r1", "s2")
                            .link("s1", "dst", viaFirst)
                            .link("s2", "dst", viaSecond)
                            .build();
  OpenrE2eLab lab{topology};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  constexpr char kPrefix[] = "2001:db8:600::/64";
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1"}, kTimeout));
  lab.setNodeOverload("s1", /*overloaded=*/true);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s2"}, kTimeout));
  lab.setNodeOverload("s1", /*overloaded=*/false);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1"}, kTimeout));

  lab.setLinkOverload("s1-dst", "s1", /*overloaded=*/true);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s2"}, kTimeout));
  lab.setLinkOverload("s1-dst", "s1", /*overloaded=*/false);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1"}, kTimeout));

  lab.setLinkOverload("r1-s1", "r1", /*overloaded=*/true);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s2"}, kTimeout));
  lab.setLinkOverload("r1-s1", "r1", /*overloaded=*/false);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1"}, kTimeout));
}

} // namespace
} // namespace openr::e2e
