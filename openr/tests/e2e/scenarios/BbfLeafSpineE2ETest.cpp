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

constexpr char kEbLoopback[] = "2001:db8:400::1/128";

Topology
bbfLeafSpine() {
  LinkOptions firstUplink;
  firstUplink.name = "leaf1-spine1";
  LinkOptions secondUplink;
  secondUplink.name = "leaf1-spine2";
  LinkOptions firstDownlink;
  firstDownlink.name = "spine1-leaf3";
  return TopologyBuilder{"bbf-leaf-spine"}
      .realNode("eb01-l001")
      .syntheticNode("eb01-s001")
      .syntheticNode("eb01-s002")
      .syntheticNode("eb01-l003")
      .syntheticNode("eb01.labbfb1")
      .prefix("eb01.labbfb1", kEbLoopback)
      .link("eb01-l001", "eb01-s001", firstUplink)
      .link("eb01-l001", "eb01-s002", secondUplink)
      .link("eb01-s001", "eb01-l003", firstDownlink)
      .link("eb01-s002", "eb01-l003")
      .link("eb01-l003", "eb01.labbfb1")
      .build();
}

TEST(BbfLeafSpineE2ETest, SpineUplinkLossKeepsEbLoopbackReachable) {
  OpenrE2eLab lab{bbfLeafSpine()};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  ASSERT_TRUE(lab.awaitAdjacency("eb01-s001", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("eb01-s002", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitProgrammedRoute(
      kEbLoopback, {"eb01-s001", "eb01-s002"}, kTimeout));

  lab.setLinkState("leaf1-spine1", /*isUp=*/false);
  ASSERT_TRUE(lab.awaitAdjacency("eb01-s001", /*isUp=*/false, kTimeout));
  ASSERT_TRUE(lab.awaitProgrammedRoute(kEbLoopback, {"eb01-s002"}, kTimeout));

  lab.setLinkState("leaf1-spine1", /*isUp=*/true);
  ASSERT_TRUE(lab.awaitAdjacency("eb01-s001", /*isUp=*/true, kTimeout));
  EXPECT_TRUE(lab.awaitProgrammedRoute(
      kEbLoopback, {"eb01-s001", "eb01-s002"}, kTimeout));
}

TEST(BbfLeafSpineE2ETest, LinkCostAndHardDrainsMoveEbLoopbackRoute) {
  OpenrE2eLab lab{bbfLeafSpine()};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  ASSERT_TRUE(lab.awaitProgrammedRoute(
      kEbLoopback, {"eb01-s001", "eb01-s002"}, kTimeout));

  lab.setLinkMetric("spine1-leaf3", "eb01-s001", 50);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kEbLoopback, {"eb01-s002"}, kTimeout));
  lab.setLinkMetric("spine1-leaf3", "eb01-s001", 1);
  ASSERT_TRUE(lab.awaitProgrammedRoute(
      kEbLoopback, {"eb01-s001", "eb01-s002"}, kTimeout));

  lab.setNodeOverload("eb01-s001", /*overloaded=*/true);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kEbLoopback, {"eb01-s002"}, kTimeout));
  lab.setNodeOverload("eb01-s001", /*overloaded=*/false);
  ASSERT_TRUE(lab.awaitProgrammedRoute(
      kEbLoopback, {"eb01-s001", "eb01-s002"}, kTimeout));

  lab.setLinkOverload("leaf1-spine1", "eb01-l001", /*overloaded=*/true);
  ASSERT_TRUE(lab.awaitProgrammedRoute(kEbLoopback, {"eb01-s002"}, kTimeout));
  lab.setLinkOverload("leaf1-spine1", "eb01-l001", /*overloaded=*/false);
  EXPECT_TRUE(lab.awaitProgrammedRoute(
      kEbLoopback, {"eb01-s001", "eb01-s002"}, kTimeout));
}

} // namespace
} // namespace openr::e2e
