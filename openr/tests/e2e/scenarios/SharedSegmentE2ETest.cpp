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

TEST(SharedSegmentE2ETest, TwoPeersOnOneLanProvideIndependentNextHops) {
  const auto topology = TopologyBuilder{"shared-lan"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("s2")
                            .sharedSegment("lan", {"r1", "s1", "s2"})
                            .build();
  OpenrE2eLab lab{topology};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  constexpr char kPrefix[] = "2001:db8:500::/64";
  ASSERT_TRUE(lab.awaitAdjacency("s1", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("s2", /*isUp=*/true, kTimeout));

  lab.advertisePrefix("s1", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1"}, kTimeout));
  lab.advertisePrefix("s2", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s1", "s2"}, kTimeout));

  lab.withdrawPrefix("s1", kPrefix);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"s2"}, kTimeout));
}

} // namespace
} // namespace openr::e2e
