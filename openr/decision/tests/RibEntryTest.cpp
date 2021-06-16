/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/Util.h>
#include <openr/decision/RibEntry.h>

namespace openr {

const auto path1_2_1_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    1,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));
const auto path1_2_2_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));

const auto path1_3_1_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_1"),
    1,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));
const auto path1_3_2_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_2"),
    2,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));
const auto path1_3_1_php = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_1"),
    1,
    createMplsAction(thrift::MplsActionCode::PHP));
const auto path1_2_1_php = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    1,
    createMplsAction(thrift::MplsActionCode::PHP));
const auto path1_2_2_php = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2,
    createMplsAction(thrift::MplsActionCode::PHP));
const auto path1_2_2_pop = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    2,
    createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP));

TEST(RibEntryTest, RibMplsEntry_filterNexthopsToUniqueAction) {
  RibMplsEntry ribEntry(0);

  // Validate pop route
  ribEntry.nexthops = {path1_2_2_pop};
  ribEntry.filterNexthopsToUniqueAction();
  EXPECT_EQ(
      ribEntry.nexthops,
      std::unordered_set<thrift::NextHopThrift>({path1_2_2_pop}));

  // PHP (direct next-hops) are preferred over SWAP (indirect next-hops)
  // Metric is ignored
  ribEntry.nexthops = {
      path1_2_1_swap, path1_2_2_php, path1_3_1_swap, path1_2_2_php};
  ribEntry.filterNexthopsToUniqueAction();
  EXPECT_EQ(
      ribEntry.nexthops,
      std::unordered_set<thrift::NextHopThrift>(
          {path1_2_2_php, path1_2_2_php}));

  // PHP (direct next-hops) are preferred over SWAP (indirect next-hops)
  // Metric is ignored
  ribEntry.nexthops = {
      path1_2_1_php, path1_2_2_swap, path1_3_1_php, path1_3_2_swap};
  ribEntry.filterNexthopsToUniqueAction();
  EXPECT_EQ(
      ribEntry.nexthops,
      std::unordered_set<thrift::NextHopThrift>(
          {path1_2_1_php, path1_3_1_php}));

  // Prefer PHP over SWAP for metric tie
  ribEntry.nexthops = {path1_2_1_swap, path1_3_1_php};
  ribEntry.filterNexthopsToUniqueAction();
  EXPECT_EQ(
      ribEntry.nexthops,
      std::unordered_set<thrift::NextHopThrift>({path1_3_1_php}));
}

} // namespace openr

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
