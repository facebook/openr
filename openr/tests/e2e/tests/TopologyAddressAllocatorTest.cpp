/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <openr/tests/e2e/TopologyAddressAllocator.h>

namespace openr::e2e {

TEST(TopologyAddressAllocatorTest, AllocateLink_PartialV4_UsesPeerSubnet) {
  LinkSpec link;
  link.first.addresses = {
      folly::IPAddress::createNetwork("192.168.0.1/24", -1, false)};

  allocateLinkAddresses(link, 1);

  ASSERT_EQ(2, link.second.addresses.size());
  const auto& generated = link.second.addresses.at(1);
  EXPECT_TRUE(generated.first.isV4());
  EXPECT_EQ(24, generated.second);
  EXPECT_TRUE(generated.first.inSubnet(
      link.first.addresses.front().first, link.first.addresses.front().second));
  EXPECT_NE(link.first.addresses.front().first, generated.first);
}

TEST(TopologyAddressAllocatorTest, AllocateSegment_MissingAddresses_AreUnique) {
  SegmentSpec segment;
  segment.endpoints.resize(3);

  allocateSegmentAddresses(segment, 7);

  EXPECT_EQ(
      folly::IPAddress("10.0.7.1"),
      segment.endpoints.at(0).addresses.at(1).first);
  EXPECT_NE(
      segment.endpoints.at(0).addresses.at(1).first,
      segment.endpoints.at(2).addresses.at(1).first);
}

TEST(TopologyAddressAllocatorTest, AllocateLink_ExhaustedSubnet_Throws) {
  LinkSpec link;
  EXPECT_THROW(allocateLinkAddresses(link, 0x10000), TopologyError);
}

TEST(TopologyAddressAllocatorTest, AllocateLink_DifferentSubnets_Throws) {
  LinkSpec link;
  link.first.addresses = {
      folly::IPAddress::createNetwork("192.0.2.1/24", -1, false)};
  link.second.addresses = {
      folly::IPAddress::createNetwork("198.51.100.1/24", -1, false)};
  EXPECT_THROW(allocateLinkAddresses(link, 1), TopologyError);
}

TEST(TopologyAddressAllocatorTest, AllocateLink_HostOnlySubnet_Throws) {
  LinkSpec link;
  link.first.addresses = {
      folly::IPAddress::createNetwork("192.0.2.1/32", -1, false)};
  EXPECT_THROW(allocateLinkAddresses(link, 1), TopologyError);
}

TEST(TopologyAddressAllocatorTest, AllocateSegment_FullSubnet_Throws) {
  SegmentSpec segment;
  segment.endpoints.resize(3);
  segment.endpoints.at(0).addresses = {
      folly::IPAddress::createNetwork("192.0.2.0/31", -1, false)};
  segment.endpoints.at(1).addresses = {
      folly::IPAddress::createNetwork("192.0.2.1/31", -1, false)};
  EXPECT_THROW(allocateSegmentAddresses(segment, 1), TopologyError);
}

TEST(TopologyAddressAllocatorTest, AllocateSegment_TooManyEndpoints_Throws) {
  SegmentSpec segment;
  segment.endpoints.resize(255);
  EXPECT_THROW(allocateSegmentAddresses(segment, 1), TopologyError);
}

} // namespace openr::e2e
