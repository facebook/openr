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

TEST(OpenrE2eTest, LinkFailoverKeepsOnlyReachableNextHops) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("alpha");
  lab.addPeer("beta");
  lab.start();

  constexpr std::chrono::milliseconds kTimeout{60'000};
  constexpr char kPrefix[] = "2001:db8:70::/64";

  ASSERT_TRUE(lab.awaitAdjacency("alpha", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("beta", /*isUp=*/true, kTimeout));
  lab.advertisePrefix("alpha", kPrefix);
  lab.advertisePrefix("beta", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"alpha", "beta"}, kTimeout));

  lab.setLinkUp("alpha", /*isUp=*/false);
  ASSERT_TRUE(lab.awaitAdjacency("alpha", /*isUp=*/false, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("beta", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"beta"}, kTimeout));

  lab.setLinkUp("beta", /*isUp=*/false);
  ASSERT_TRUE(lab.awaitAdjacency("beta", /*isUp=*/false, kTimeout));
  EXPECT_TRUE(lab.awaitRouteAbsent(kPrefix, kTimeout));
}

} // namespace
} // namespace openr::e2e
