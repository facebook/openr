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

TEST(MultiHopTopologyE2ETest, RemoteMetricChangeMovesProgrammedNextHop) {
  LinkOptions viaFirst;
  viaFirst.name = "s1-dst";
  viaFirst.metric = 1;
  LinkOptions viaSecond;
  viaSecond.name = "s2-dst";
  viaSecond.metric = 20;
  constexpr char kPrefix[] = "2001:db8:100::/64";
  const auto topology = TopologyBuilder{"multi-hop"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("s2")
                            .syntheticNode("dst")
                            .prefix("dst", kPrefix)
                            .link("r1", "s1")
                            .link("r1", "s2")
                            .link("s1", "dst", viaFirst)
                            .link("s2", "dst", viaSecond)
                            .build();
  OpenrE2eLab lab{topology};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  ASSERT_TRUE(lab.awaitAdjacency("s1", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("s2", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1"}, kTimeout));

  lab.setLinkMetric("s1-dst", "s1", 50);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s2"}, kTimeout));

  lab.setLinkMetric("s1-dst", "s1", 1);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1"}, kTimeout));
}

} // namespace
} // namespace openr::e2e
