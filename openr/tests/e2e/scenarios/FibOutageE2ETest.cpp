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

TEST(OpenrE2eTest, FibServiceOutagePreservesPendingRoute) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("spine");
  lab.start();

  constexpr std::chrono::milliseconds kTimeout{60'000};
  constexpr char kBaselinePrefix[] = "2001:db8:80::/64";
  constexpr char kPendingPrefix[] = "2001:db8:90::/64";

  ASSERT_TRUE(lab.awaitAdjacency("spine", /*isUp=*/true, kTimeout));
  lab.advertisePrefix("spine", kBaselinePrefix);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kBaselinePrefix, {"spine"}, kTimeout));

  lab.setFibServiceAvailable(/*isAvailable=*/false);
  lab.advertisePrefix("spine", kPendingPrefix);
  ASSERT_TRUE(lab.awaitComputedRoute(kPendingPrefix, kTimeout));
  ASSERT_TRUE(lab.awaitRouteAbsent(kPendingPrefix, kTimeout));

  lab.setFibServiceAvailable(/*isAvailable=*/true);
  EXPECT_TRUE(lab.awaitProgrammedRoute(kPendingPrefix, {"spine"}, kTimeout));
}

} // namespace
} // namespace openr::e2e
