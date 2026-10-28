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

constexpr std::chrono::milliseconds kTimeout{60'000};
constexpr char kPrefix[] = "2001:db8:40::/64";

TEST(OpenrE2eTest, EqualCostPeersProgramExactNextHops) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("alpha");
  lab.addPeer("beta");
  lab.start();

  ASSERT_TRUE(lab.awaitAdjacency("alpha", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("beta", /*isUp=*/true, kTimeout));

  lab.advertisePrefix("alpha", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"alpha"}, kTimeout));

  lab.advertisePrefix("beta", kPrefix);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"alpha", "beta"}, kTimeout));
}

TEST(OpenrE2eTest, WithdrawOneOriginPreservesRemainingNextHop) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("alpha");
  lab.addPeer("beta");
  lab.start();

  ASSERT_TRUE(lab.awaitAdjacency("alpha", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("beta", /*isUp=*/true, kTimeout));

  lab.advertisePrefix("alpha", kPrefix);
  lab.advertisePrefix("beta", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"alpha", "beta"}, kTimeout));

  lab.withdrawPrefix("alpha", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"beta"}, kTimeout));

  lab.withdrawPrefix("beta", kPrefix);
  EXPECT_TRUE(lab.awaitRouteAbsent(kPrefix, kTimeout));
}

} // namespace
} // namespace openr::e2e
