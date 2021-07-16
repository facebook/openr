/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/MplsUtil.h>

using namespace openr;

TEST(UtilTest, MplsUtilTest) {
  EXPECT_TRUE(isMplsLabelValid(1132));
  EXPECT_TRUE(isMplsLabelValid((1 << 20) - 1));
  EXPECT_FALSE(isMplsLabelValid(1 << 20));
  EXPECT_FALSE(isMplsLabelValid(1 << 30));
}

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
