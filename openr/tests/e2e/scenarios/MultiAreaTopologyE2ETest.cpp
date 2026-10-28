/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <chrono>

#include <openr/tests/e2e/OpenrE2e.h>
#include <openr/tests/e2e/TopologyBuilder.h>

namespace openr::e2e {
namespace {

constexpr char kAreaAPrefix[] = "2001:db8:a::/64";
constexpr char kAreaBPrefix[] = "2001:db8:b::/64";

Topology
twoAreaTopology() {
  NodeOptions border;
  border.areas = {"A", "B"};
  NodeOptions peerA;
  peerA.areas = {"A"};
  NodeOptions peerB;
  peerB.areas = {"B"};
  LinkOptions linkA;
  linkA.name = "a-border";
  linkA.area = "A";
  LinkOptions linkB;
  linkB.name = "border-b";
  linkB.area = "B";
  return TopologyBuilder{"two-area-boundary"}
      .realNode("border", border)
      .syntheticNode("area-a-peer", peerA)
      .syntheticNode("area-b-peer", peerB)
      .link("area-a-peer", "border", linkA)
      .link("border", "area-b-peer", linkB)
      .build();
}

TEST(MultiAreaTopologyE2ETest, CrossAreaRoutesConvergeAndWithdraw) {
  OpenrE2eLab lab{twoAreaTopology()};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  ASSERT_TRUE(lab.awaitAdjacency("area-a-peer", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("area-b-peer", /*isUp=*/true, kTimeout));
  PrefixOptions areaA;
  areaA.area = "A";
  PrefixOptions areaB;
  areaB.area = "B";
  lab.advertisePrefix("area-a-peer", kAreaAPrefix, areaA);
  lab.advertisePrefix("area-b-peer", kAreaBPrefix, areaB);
  ASSERT_TRUE(
      lab.awaitProgrammedRoute(kAreaAPrefix, {"area-a-peer"}, kTimeout));
  ASSERT_TRUE(
      lab.awaitProgrammedRoute(kAreaBPrefix, {"area-b-peer"}, kTimeout));
  ASSERT_TRUE(lab.awaitPeerPrefix(
      "area-b-peer", kAreaAPrefix, /*isAdvertised=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitPeerPrefix(
      "area-a-peer", kAreaBPrefix, /*isAdvertised=*/true, kTimeout));

  lab.withdrawPrefix("area-a-peer", kAreaAPrefix, areaA);
  EXPECT_TRUE(lab.awaitRouteAbsent(kAreaAPrefix, kTimeout));
  EXPECT_TRUE(lab.awaitPeerPrefix(
      "area-b-peer", kAreaAPrefix, /*isAdvertised=*/false, kTimeout));
  EXPECT_TRUE(
      lab.awaitProgrammedRoute(kAreaBPrefix, {"area-b-peer"}, kTimeout));
}

TEST(MultiAreaTopologyE2ETest, CrossAreaLinkFailureWithdrawsAndRecovers) {
  OpenrE2eLab lab{twoAreaTopology()};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  PrefixOptions areaA;
  areaA.area = "A";
  PrefixOptions areaB;
  areaB.area = "B";
  lab.advertisePrefix("area-a-peer", kAreaAPrefix, areaA);
  lab.advertisePrefix("area-b-peer", kAreaBPrefix, areaB);
  ASSERT_TRUE(
      lab.awaitProgrammedRoute(kAreaBPrefix, {"area-b-peer"}, kTimeout));
  ASSERT_TRUE(lab.awaitPeerPrefix(
      "area-a-peer", kAreaBPrefix, /*isAdvertised=*/true, kTimeout));

  lab.setLinkState("border-b", /*isUp=*/false);
  ASSERT_TRUE(lab.awaitAdjacency("area-b-peer", /*isUp=*/false, kTimeout));
  ASSERT_TRUE(lab.awaitRouteAbsent(kAreaBPrefix, kTimeout));
  ASSERT_TRUE(lab.awaitPeerPrefix(
      "area-a-peer", kAreaBPrefix, /*isAdvertised=*/false, kTimeout));
  ASSERT_TRUE(
      lab.awaitProgrammedRoute(kAreaAPrefix, {"area-a-peer"}, kTimeout));

  lab.setLinkState("border-b", /*isUp=*/true);
  ASSERT_TRUE(lab.awaitAdjacency("area-b-peer", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(
      lab.awaitProgrammedRoute(kAreaBPrefix, {"area-b-peer"}, kTimeout));
  ASSERT_TRUE(lab.awaitPeerPrefix(
      "area-a-peer", kAreaBPrefix, /*isAdvertised=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitPeerPrefix(
      "area-b-peer", kAreaAPrefix, /*isAdvertised=*/true, kTimeout));
}

} // namespace
} // namespace openr::e2e
