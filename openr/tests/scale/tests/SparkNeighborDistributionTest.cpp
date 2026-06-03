/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <map>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <openr/tests/scale/SparkNeighborDistribution.h>

namespace openr {

namespace {

std::vector<UsableInterface>
makeInterfaces(int count) {
  std::vector<UsableInterface> ifaces;
  for (int i = 0; i < count; ++i) {
    ifaces.push_back(
        {fmt::format("eth{}", i),
         100 + i,
         fmt::format("fe80::{}", i + 1),
         fmt::format("10.0.{}.1", i)});
  }
  return ifaces;
}

std::vector<std::string>
makeNeighbors(int count) {
  std::vector<std::string> names;
  for (int i = 0; i < count; ++i) {
    names.push_back(fmt::format("spine-{}", i));
  }
  return names;
}

// Count how many placements landed on each host interface.
std::map<std::string, int>
perInterfaceCounts(const std::vector<SparkNeighborPlacement>& placements) {
  std::map<std::string, int> counts;
  for (const auto& p : placements) {
    counts[p.hostIfName]++;
  }
  return counts;
}

} // namespace

TEST(DistributeSparkNeighborsTest, PlacesEveryNeighborExactlyOnceInOrder) {
  const auto plan =
      distributeSparkNeighbors(makeInterfaces(2), makeNeighbors(4));
  std::vector<std::string> placed;
  for (const auto& p : plan) {
    placed.push_back(p.neighborName);
  }
  EXPECT_EQ(placed, makeNeighbors(4));
}

TEST(DistributeSparkNeighborsTest, EvenlyDistributesAcrossInterfaces) {
  const auto plan =
      distributeSparkNeighbors(makeInterfaces(2), makeNeighbors(4));
  const auto counts = perInterfaceCounts(plan);
  EXPECT_EQ(counts.at("eth0"), 2);
  EXPECT_EQ(counts.at("eth1"), 2);
}

TEST(DistributeSparkNeighborsTest, RemainderGoesToLastInterface) {
  // 5 neighbors / 2 interfaces => floor(5/2)=2 on the first, remainder on last.
  const auto plan =
      distributeSparkNeighbors(makeInterfaces(2), makeNeighbors(5));
  const auto counts = perInterfaceCounts(plan);
  EXPECT_EQ(counts.at("eth0"), 2);
  EXPECT_EQ(counts.at("eth1"), 3);
}

TEST(DistributeSparkNeighborsTest, AssignsContiguousBlocksPerInterface) {
  // First two neighbors on eth0, last three on eth1 (contiguous, in order).
  const auto plan =
      distributeSparkNeighbors(makeInterfaces(2), makeNeighbors(5));
  ASSERT_EQ(plan.size(), 5u);
  EXPECT_EQ(plan[0].hostIfName, "eth0");
  EXPECT_EQ(plan[1].hostIfName, "eth0");
  EXPECT_EQ(plan[2].hostIfName, "eth1");
  EXPECT_EQ(plan[3].hostIfName, "eth1");
  EXPECT_EQ(plan[4].hostIfName, "eth1");
}

TEST(
    DistributeSparkNeighborsTest,
    PlacementCarriesInterfaceAddrsAndDerivedIfName) {
  const auto plan =
      distributeSparkNeighbors(makeInterfaces(1), makeNeighbors(1));
  ASSERT_EQ(plan.size(), 1u);
  const auto& p = plan[0];
  EXPECT_EQ(p.neighborName, "spine-0");
  EXPECT_EQ(p.neighborIfName, "spine-0-to-dut");
  EXPECT_EQ(p.hostIfName, "eth0");
  EXPECT_EQ(p.hostIfIndex, 100);
  EXPECT_EQ(p.v6Addr, "fe80::1");
  EXPECT_EQ(p.v4Addr, "10.0.0.1");
}

TEST(
    DistributeSparkNeighborsTest, FewerNeighborsThanInterfacesPlacesAllOnLast) {
  // 1 neighbor / 3 interfaces => floor(1/3)=0 each, remainder (the single
  // neighbor) lands on the last interface.
  const auto plan =
      distributeSparkNeighbors(makeInterfaces(3), makeNeighbors(1));
  ASSERT_EQ(plan.size(), 1u);
  EXPECT_EQ(plan[0].hostIfName, "eth2");
}

TEST(DistributeSparkNeighborsTest, NoUsableInterfacesReturnsEmpty) {
  EXPECT_TRUE(distributeSparkNeighbors({}, makeNeighbors(3)).empty());
}

TEST(DistributeSparkNeighborsTest, NoNeighborsReturnsEmpty) {
  EXPECT_TRUE(distributeSparkNeighbors(makeInterfaces(2), {}).empty());
}

} // namespace openr
