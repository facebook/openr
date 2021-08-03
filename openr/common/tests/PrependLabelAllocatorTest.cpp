/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/PrependLabelAllocator.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>

using namespace openr;

namespace {

std::shared_ptr<Config>
getTestConfig() {
  // generate a config for testing
  auto tConfig = getBasicOpenrConfig(
      "1",
      "domain",
      {},
      false, /* enableV4 */
      true, /* enableSegmentRouting */
      false, /* dryrun */
      false, /* enableV4OverV6Nexthop */
      false, /* enableAdjLabels */
      true /* enablePrependLabels */
  );

  return std::make_shared<Config>(tConfig);
}

/**
 * Verify label allocation.
 */
TEST(PrependLabelAllocator, GetLabel) {
  auto config = getTestConfig();
  auto prependLabelAllocator = std::make_unique<PrependLabelAllocator>(config);
  EXPECT_EQ(
      prependLabelAllocator->getNewMplsLabel(true),
      MplsConstants::kSrV4StaticMplsRouteRange.first);
  EXPECT_EQ(
      prependLabelAllocator->getNewMplsLabel(false),
      MplsConstants::kSrV6StaticMplsRouteRange.first);
}

/**
 * Verify label de-allocation.
 */
TEST(PrependLabelAllocator, FreeLabel) {
  auto config = getTestConfig();
  auto prependLabelAllocator = std::make_unique<PrependLabelAllocator>(config);

  // Allocate labels
  auto prependLabelV4 = prependLabelAllocator->getNewMplsLabel(true);
  auto prependLabelV6 = prependLabelAllocator->getNewMplsLabel(false);

  // Free the labels
  prependLabelAllocator->freeMplsLabel(true, prependLabelV4, "nh4");
  prependLabelAllocator->freeMplsLabel(false, prependLabelV6, "nh6");

  // Re-allocation of label should use labels from the free label list
  EXPECT_EQ(
      prependLabelAllocator->getNewMplsLabel(true),
      MplsConstants::kSrV4StaticMplsRouteRange.first);
  EXPECT_EQ(
      prependLabelAllocator->getNewMplsLabel(false),
      MplsConstants::kSrV6StaticMplsRouteRange.first);
}

/**
 * Increment refCount for a new NH set. Assign label.
 * Then decrement refCount and verify the value of the label.
 */
TEST(PrependLabelAllocator, RefCount) {
  auto config = getTestConfig();
  auto prependLabelAllocator = std::make_unique<PrependLabelAllocator>(config);

  std::set<folly::IPAddress> labelNextHopSet;
  labelNextHopSet.emplace(folly::IPAddress("192.168.0.1"));
  labelNextHopSet.emplace(folly::IPAddress("192.168.0.2"));

  auto& [refCount, label] =
      prependLabelAllocator->getNextHopSetToLabel()[labelNextHopSet];
  prependLabelAllocator->incrementRefCount(labelNextHopSet);
  EXPECT_EQ(refCount, 1);
  EXPECT_EQ(label, 0);

  label = 101; /* New Label */
  auto oldLabel = prependLabelAllocator->decrementRefCount(labelNextHopSet);
  EXPECT_EQ(refCount, 0);
  EXPECT_EQ(oldLabel, label);
}
} // namespace

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
