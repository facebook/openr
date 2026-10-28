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

TEST(OpenrE2eTest, PeerJoinsRunningNodeAndAddsRouteNextHop) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("alpha");
  lab.start();

  constexpr std::chrono::milliseconds kTimeout{60'000};
  constexpr char kPrefix[] = "2001:db8:80::/64";

  ASSERT_TRUE(lab.awaitAdjacency("alpha", /*isUp=*/true, kTimeout));
  lab.advertisePrefix("alpha", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"alpha"}, kTimeout));

  lab.addPeer("beta");
  ASSERT_TRUE(lab.awaitAdjacency("beta", /*isUp=*/true, kTimeout));
  lab.advertisePrefix("beta", kPrefix);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"alpha", "beta"}, kTimeout));
}

} // namespace
} // namespace openr::e2e
