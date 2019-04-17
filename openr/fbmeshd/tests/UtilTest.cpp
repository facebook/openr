// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/fbmeshd/common/Util.h>

using namespace openr::fbmeshd;

// test macAddrToNodeName()
TEST(UtilTest, macAddrToNodeName) {
  EXPECT_EQ(
      "faceb00c-face-b00c-face-001122334455",
      macAddrToNodeName(folly::MacAddress{"00:11:22:33:44:55"}));
  EXPECT_EQ(
      "faceb00c-face-b00c-face-aabbccddeeff",
      macAddrToNodeName(folly::MacAddress{"aa:bb:cc:dd:ee:ff"}));
}

// test nodeNameToMacAddr()
TEST(UtilTest, nodeNameToMacAddr) {
  EXPECT_EQ(
      folly::MacAddress{"00:11:22:33:44:55"},
      nodeNameToMacAddr("faceb00c-face-b00c-face-001122334455"));
  EXPECT_EQ(
      folly::MacAddress{"aa:bb:cc:dd:ee:ff"},
      nodeNameToMacAddr("faceb00c-face-b00c-face-aabbccddeeff"));
  EXPECT_EQ(folly::none, nodeNameToMacAddr("blah:blah:blah"));
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
