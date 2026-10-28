/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <openr/tests/e2e/OpenrE2e.h>

namespace openr::e2e {
namespace {

TEST(OpenrE2eTest, LinkLossRemovesOnlyFailedPeersRoute) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("alpha");
  lab.addPeer("beta");
  lab.start();

  constexpr std::chrono::milliseconds kTimeout{60'000};
  constexpr char kAlphaPrefix[] = "2001:db8:40::/64";
  constexpr char kBetaPrefix[] = "2001:db8:50::/64";

  ASSERT_TRUE(lab.awaitAdjacency("alpha", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("beta", /*isUp=*/true, kTimeout));
  lab.advertisePrefix("alpha", kAlphaPrefix);
  lab.advertisePrefix("beta", kBetaPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kAlphaPrefix, {"alpha"}, kTimeout));
  ASSERT_TRUE(lab.awaitProgrammedRoute(kBetaPrefix, {"beta"}, kTimeout));

  lab.setLinkUp("alpha", /*isUp=*/false);
  ASSERT_TRUE(lab.awaitAdjacency("alpha", /*isUp=*/false, kTimeout));
  ASSERT_TRUE(lab.awaitRouteAbsent(kAlphaPrefix, kTimeout));
  EXPECT_TRUE(lab.awaitAdjacency("beta", /*isUp=*/true, kTimeout));
  EXPECT_TRUE(lab.awaitProgrammedRoute(kBetaPrefix, {"beta"}, kTimeout));
}

TEST(OpenrE2eTest, RestoredLinkRestoresRemotePrefixRoute) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("spine");
  lab.start();

  constexpr std::chrono::milliseconds kTimeout{60'000};
  constexpr char kPrefix[] = "2001:db8:60::/64";

  ASSERT_TRUE(lab.awaitAdjacency("spine", /*isUp=*/true, kTimeout));
  lab.advertisePrefix("spine", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"spine"}, kTimeout));

  lab.setLinkUp("spine", /*isUp=*/false);
  ASSERT_TRUE(lab.awaitAdjacency("spine", /*isUp=*/false, kTimeout));
  ASSERT_TRUE(lab.awaitRouteAbsent(kPrefix, kTimeout));

  lab.setLinkUp("spine", /*isUp=*/true);
  ASSERT_TRUE(lab.awaitAdjacency("spine", /*isUp=*/true, kTimeout));
  EXPECT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"spine"}, kTimeout));
}

} // namespace
} // namespace openr::e2e
