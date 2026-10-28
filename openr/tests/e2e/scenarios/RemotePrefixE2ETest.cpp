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

} // namespace
} // namespace openr::e2e
