/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/Types.h>

using namespace std;
using namespace openr;

TEST(TypesTest, fromStrTest) {
  const std::string nodeName{"node-1"};
  const std::string badNodeName{"\\\\[]{}"};
  const std::string areaId = "default-area";
  const std::string badAreaId{"{FILL_ME_IN}"};
  const std::string prefix{"1.1.1.1/32"};
  const std::string badPrefix{"1.1."};

  const std::string validStr{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kPrefixDbMarker.toString(),
      nodeName,
      areaId,
      prefix)};
  const std::string invalidStrWithoutAreaId{fmt::format(
      "{}{}:[{}]", Constants::kPrefixDbMarker.toString(), nodeName, prefix)};
  const std::string invalidStrWithBadType{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kAdjDbMarker.toString(),
      nodeName,
      areaId,
      prefix)};
  const std::string invalidStrWithBadNode{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kPrefixDbMarker.toString(),
      badNodeName,
      areaId,
      prefix)};
  const std::string invalidStrWithBadAreaId{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kPrefixDbMarker.toString(),
      nodeName,
      badAreaId,
      prefix)};
  const std::string invalidStrWithBadPrefix{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kPrefixDbMarker.toString(),
      nodeName,
      areaId,
      badPrefix)};

  auto maybePrefixKey = PrefixKey::fromStr(validStr);
  EXPECT_FALSE(maybePrefixKey.hasError());
  EXPECT_EQ(nodeName, maybePrefixKey.value().getNodeName());
  EXPECT_EQ(areaId, maybePrefixKey.value().getPrefixArea());
  EXPECT_EQ(
      folly::IPAddress::createNetwork(prefix),
      maybePrefixKey.value().getCIDRNetwork());
  EXPECT_EQ(
      fmt::format(
          "{}{}:{}:[{}]",
          Constants::kPrefixDbMarker.toString(),
          nodeName,
          areaId,
          prefix),
      maybePrefixKey.value().getPrefixKey());

  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithoutAreaId).hasError());
  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadType).hasError());
  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadNode).hasError());
  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadAreaId).hasError());
  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadPrefix).hasError());
}

TEST(TypesTest, fromStrV2Test) {
  const std::string nodeName{"node-1"};
  const std::string badNodeName{"\\\\[]{}"};
  const std::string areaId = "non-default-area";
  const std::string prefix{"1.1.1.1/32"};
  const std::string badPrefix{"1.1."};

  const std::string validStr{fmt::format(
      "{}{}:[{}]", Constants::kPrefixDbMarker.toString(), nodeName, prefix)};
  const std::string invalidStrWithBadFormat{fmt::format(
      "{}{}:[{}]:{}",
      Constants::kPrefixDbMarker.toString(),
      nodeName,
      prefix,
      areaId)};
  const std::string invalidStrWithBadType{fmt::format(
      "{}{}:[{}]", Constants::kAdjDbMarker.toString(), nodeName, prefix)};
  const std::string invalidStrWithBadNode{fmt::format(
      "{}{}:[{}]", Constants::kPrefixDbMarker.toString(), badNodeName, prefix)};
  const std::string invalidStrWithBadPrefix{fmt::format(
      "{}{}:[{}]", Constants::kPrefixDbMarker.toString(), nodeName, badPrefix)};

  auto maybePrefixKey = PrefixKey::fromStrV2(validStr, areaId);
  EXPECT_FALSE(maybePrefixKey.hasError());
  EXPECT_EQ(nodeName, maybePrefixKey.value().getNodeName());
  EXPECT_EQ(areaId, maybePrefixKey.value().getPrefixArea());
  EXPECT_EQ(
      folly::IPAddress::createNetwork(prefix),
      maybePrefixKey.value().getCIDRNetwork());
  EXPECT_EQ(
      fmt::format(
          "{}{}:[{}]", Constants::kPrefixDbMarker.toString(), nodeName, prefix),
      maybePrefixKey.value().getPrefixKeyStr());

  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadFormat).hasError());
  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadType).hasError());
  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadNode).hasError());
  EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadPrefix).hasError());
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
