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

TEST(OpenrE2eTest, LocalPrefixAdvertisesAndWithdraws) {
  OpenrE2eLab lab{"dut"};
  lab.addPeer("spine");
  lab.start();

  constexpr std::chrono::milliseconds kTimeout{60'000};
  constexpr char kPrefix[] = "2001:db8:50::1/128";

  ASSERT_TRUE(lab.awaitAdjacency("spine", /*isUp=*/true, kTimeout));

  lab.advertiseLocalPrefix(kPrefix);
  ASSERT_TRUE(
      lab.awaitPeerPrefix("spine", kPrefix, /*isAdvertised=*/true, kTimeout));

  lab.withdrawLocalPrefix(kPrefix);
  EXPECT_TRUE(
      lab.awaitPeerPrefix("spine", kPrefix, /*isAdvertised=*/false, kTimeout));
}

} // namespace
} // namespace openr::e2e
