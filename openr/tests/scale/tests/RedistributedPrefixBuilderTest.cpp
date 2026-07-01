/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdexcept>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/NetworkUtil.h>
#include <openr/tests/scale/RedistributedPrefixBuilder.h>
#include <openr/tests/scale/VirtualRouter.h>

namespace openr {

TEST(
    RedistributedPrefixBuilderTest,
    SingleHopMarksRibAppendsOriginBumpsDistance) {
  auto entry =
      buildRedistributedPrefixEntry(toIpPrefix("2401:db00::/64"), {"pod"});
  EXPECT_EQ(entry.type().value(), thrift::PrefixType::RIB);
  EXPECT_THAT(entry.area_stack().value(), ::testing::ElementsAre("pod"));
  EXPECT_EQ(entry.metrics()->distance().value(), 1);
  EXPECT_EQ(entry.forwardingType().value(), thrift::PrefixForwardingType::IP);
}

TEST(RedistributedPrefixBuilderTest, MultiHopKeepsTraversalOrderAndCountsHops) {
  auto entry = buildRedistributedPrefixEntry(
      toIpPrefix("2401:db00:1::/64"), {"pod", "plane", "spine"});
  // index 0 is the originating area; distance counts ABR hops, which equals the
  // number of areas the prefix traversed.
  EXPECT_THAT(
      entry.area_stack().value(),
      ::testing::ElementsAre("pod", "plane", "spine"));
  EXPECT_EQ(entry.metrics()->distance().value(), 3);
  EXPECT_EQ(entry.type().value(), thrift::PrefixType::RIB);
}

TEST(
    RedistributedPrefixBuilderTest,
    RedistributeOnceAppendsAreaAndBumpsDistance) {
  // One ABR hop has already been applied; a second atomic hop appends the new
  // source area and bumps the distance again.
  auto entry =
      buildRedistributedPrefixEntry(toIpPrefix("2401:db00:2::/64"), {"A"});
  redistributePrefixOnce(entry, "B");
  EXPECT_THAT(entry.area_stack().value(), ::testing::ElementsAre("A", "B"));
  EXPECT_EQ(entry.metrics()->distance().value(), 2);
  EXPECT_EQ(entry.type().value(), thrift::PrefixType::RIB);
}

TEST(RedistributedPrefixBuilderTest, EmptyTraversedAreasThrows) {
  EXPECT_THROW(
      buildRedistributedPrefixEntry(toIpPrefix("2401:db00:3::/64"), {}),
      std::invalid_argument);
}

TEST(RedistributedPrefixBuilderTest, AddRedistributedPrefixesAppendsToRouter) {
  VirtualRouter r;
  r.nodeName = "abr-proxy";
  r.area = "dut-area";
  // The proxy already advertises one (redistributed) prefix; the new ones
  // append.
  r.advertisedPrefixes.push_back(
      buildRedistributedPrefixEntry(toIpPrefix("2401:db00:9::/64"), {"seed"}));

  const std::vector<thrift::IpPrefix> foreign{
      toIpPrefix("2401:db00:a::/64"), toIpPrefix("2401:db00:b::/64")};
  addRedistributedPrefixesToRouter(r, foreign, {"pod"});

  ASSERT_EQ(r.advertisedPrefixes.size(), 3u);
  for (size_t i = 1; i < r.advertisedPrefixes.size(); ++i) {
    const auto& e = r.advertisedPrefixes[i];
    EXPECT_EQ(e.type().value(), thrift::PrefixType::RIB);
    EXPECT_EQ(e.metrics()->distance().value(), 1);
    EXPECT_THAT(e.area_stack().value(), ::testing::ElementsAre("pod"));
  }
}

TEST(RedistributedPrefixBuilderTest, RedistributeResetsNonTransitiveAttrs) {
  // A caller may hand redistributePrefixOnce an entry carrying non-default
  // non-transitive attributes; crossing an area boundary must reset all of
  // them, exactly as production PrefixManager::resetNonTransitiveAttrs does.
  auto entry =
      buildRedistributedPrefixEntry(toIpPrefix("2401:db00:4::/64"), {"A"});
  entry.forwardingType() = thrift::PrefixForwardingType::SR_MPLS;
  entry.forwardingAlgorithm() = thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
  entry.minNexthop() = 4;
  entry.weight() = 7;

  redistributePrefixOnce(entry, "B");

  EXPECT_EQ(entry.forwardingType().value(), thrift::PrefixForwardingType::IP);
  EXPECT_EQ(
      entry.forwardingAlgorithm().value(),
      thrift::PrefixForwardingAlgorithm::SP_ECMP);
  EXPECT_FALSE(entry.minNexthop().has_value());
  EXPECT_FALSE(entry.weight().has_value());
}

} // namespace openr
