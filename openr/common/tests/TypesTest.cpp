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

  // v1 format prefix key string
  const std::string validStrV1{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kPrefixDbMarker.toString(),
      nodeName,
      areaId,
      prefix)};
  const std::string invalidStrWithBadTypeV1{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kAdjDbMarker.toString(),
      nodeName,
      areaId,
      prefix)};
  const std::string invalidStrWithBadNodeV1{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kPrefixDbMarker.toString(),
      badNodeName,
      areaId,
      prefix)};
  const std::string invalidStrWithBadAreaIdV1{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kPrefixDbMarker.toString(),
      nodeName,
      badAreaId,
      prefix)};
  const std::string invalidStrWithBadPrefixV1{fmt::format(
      "{}{}:{}:[{}]",
      Constants::kPrefixDbMarker.toString(),
      nodeName,
      areaId,
      badPrefix)};

  // v2 format prefix key string
  const std::string validStrV2{fmt::format(
      "{}{}:[{}]", Constants::kPrefixDbMarker.toString(), nodeName, prefix)};
  const std::string invalidStrWithBadFormatV2{fmt::format(
      "{}{}:[{}]:{}",
      Constants::kPrefixDbMarker.toString(),
      nodeName,
      prefix,
      areaId)};
  const std::string invalidStrWithBadTypeV2{fmt::format(
      "{}{}:[{}]", Constants::kAdjDbMarker.toString(), nodeName, prefix)};
  const std::string invalidStrWithBadNodeV2{fmt::format(
      "{}{}:[{}]", Constants::kPrefixDbMarker.toString(), badNodeName, prefix)};
  const std::string invalidStrWithBadPrefixV2{fmt::format(
      "{}{}:[{}]", Constants::kPrefixDbMarker.toString(), nodeName, badPrefix)};

  {
    // validate v1 format prefix key string
    auto maybePrefixKey = PrefixKey::fromStr(validStrV1);
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

    EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadTypeV1).hasError());
    EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadNodeV1).hasError());
    EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadAreaIdV1).hasError());
    EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadPrefixV1).hasError());
  }

  {
    // validate v2 format prefix key string
    auto maybePrefixKey = PrefixKey::fromStr(validStrV2, areaId);
    EXPECT_FALSE(maybePrefixKey.hasError());
    EXPECT_EQ(nodeName, maybePrefixKey.value().getNodeName());
    EXPECT_EQ(areaId, maybePrefixKey.value().getPrefixArea());
    EXPECT_EQ(
        folly::IPAddress::createNetwork(prefix),
        maybePrefixKey.value().getCIDRNetwork());
    EXPECT_EQ(
        fmt::format(
            "{}{}:[{}]",
            Constants::kPrefixDbMarker.toString(),
            nodeName,
            prefix),
        maybePrefixKey.value().getPrefixKeyV2());

    EXPECT_TRUE(
        PrefixKey::fromStr(invalidStrWithBadFormatV2, areaId).hasError());
    EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadTypeV2, areaId).hasError());
    EXPECT_TRUE(PrefixKey::fromStr(invalidStrWithBadNodeV2, areaId).hasError());
    EXPECT_TRUE(
        PrefixKey::fromStr(invalidStrWithBadPrefixV2, areaId).hasError());
  }
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
