/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/monitor/SystemMetrics.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

using namespace std;
using namespace openr;

TEST(MonitorTestFixture, SystemMetrics) {
  SystemMetrics systemMetrics_{};
  // First cpu% query will return folly::none
  auto cpu1 = systemMetrics_.getCPUpercentage();
  EXPECT_FALSE(cpu1.has_value());

  // First memory query
  auto rssMem1 = systemMetrics_.getRSSMemBytes();
  EXPECT_TRUE(rssMem1.has_value());
  // Check sanity of return value, check for > 1MB and < 170MB
  EXPECT_GT(rssMem1.value() / 1e6, 1);
  EXPECT_LT(rssMem1.value() / 1e6, 170);

  auto virtualMem1 = systemMetrics_.getVirtualMemBytes();
  EXPECT_TRUE(virtualMem1.has_value());
  // Check sanity of return value, check for > 1MB
  EXPECT_GT(virtualMem1.value() / 1e6, 1);

  // Fill about 100 Mbytes of memory and check if monitor reports the increase
  std::vector<int64_t> v(13 * 0x100000);
  fill(v.begin(), v.end(), 1);

  // Expect the memory is 100 Mbytes higher
  auto rssMem2 = systemMetrics_.getRSSMemBytes();
  EXPECT_TRUE(rssMem2.has_value());
  EXPECT_GT(rssMem2.value(), rssMem1.value() + 100);
  auto virtualMem2 = systemMetrics_.getVirtualMemBytes();
  EXPECT_TRUE(virtualMem2.has_value());
  EXPECT_GT(virtualMem2.value(), virtualMem1.value() + 100);

  // Expect the second cpu% query has value
  auto cpu2 = systemMetrics_.getCPUpercentage();
  EXPECT_TRUE(cpu2.has_value());
  EXPECT_GT(cpu2.value(), 0);
}

int
main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  google::InstallFailureSignalHandler();

  return RUN_ALL_TESTS();
}
