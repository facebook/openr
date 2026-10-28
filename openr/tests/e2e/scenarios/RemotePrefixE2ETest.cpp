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

TEST(OpenrE2eTest, RemotePrefixProgramsAndWithdraws) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("spine");
  lab.start();

  constexpr std::chrono::milliseconds kTimeout{60'000};
  constexpr char kPrefix[] = "2001:db8:10::/64";

  ASSERT_TRUE(lab.awaitAdjacency("spine", /*isUp=*/true, kTimeout));
  lab.advertisePrefix("spine", kPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kPrefix, {"spine"}, kTimeout));

  lab.withdrawPrefix("spine", kPrefix);
  EXPECT_TRUE(lab.awaitRouteAbsent(kPrefix, kTimeout));
}

TEST(OpenrE2eTest, RemotePrefixUpdateRemovesOnlyWithdrawnRoute) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("spine");
  lab.start();

  constexpr std::chrono::milliseconds kTimeout{60'000};
  constexpr char kOldPrefix[] = "2001:db8:10::/64";
  constexpr char kNewPrefix[] = "2001:db8:11::/64";

  ASSERT_TRUE(lab.awaitAdjacency("spine", /*isUp=*/true, kTimeout));
  lab.advertisePrefix("spine", kOldPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kOldPrefix, {"spine"}, kTimeout));

  lab.advertisePrefix("spine", kNewPrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kNewPrefix, {"spine"}, kTimeout));
  lab.withdrawPrefix("spine", kOldPrefix);

  EXPECT_TRUE(lab.awaitRouteAbsent(kOldPrefix, kTimeout));
  EXPECT_TRUE(lab.awaitProgrammedRoute(kNewPrefix, {"spine"}, kTimeout));
}

} // namespace
} // namespace openr::e2e
