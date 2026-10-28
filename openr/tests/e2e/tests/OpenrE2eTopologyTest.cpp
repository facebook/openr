/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <stdexcept>

#include <openr/tests/e2e/OpenrE2e.h>
#include <openr/tests/e2e/TopologyBuilder.h>

namespace openr::e2e {
namespace {

TEST(OpenrE2eTopologyTest, AcceptsNamedMultiHopSyntheticTopology) {
  const auto topology = TopologyBuilder{"multi-hop"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("dst")
                            .link("r1", "s1")
                            .link("s1", "dst")
                            .build();
  EXPECT_NO_THROW(OpenrE2eLab{topology});
}

TEST(OpenrE2eTopologyTest, PeersWithTheSameInterfaceNameRemainIndependent) {
  LinkOptions toFirst;
  toFirst.second.interfaceName = "eth0";
  LinkOptions toSecond;
  toSecond.second.interfaceName = "eth0";
  constexpr char kFirstPrefix[] = "2001:db8:410::/64";
  constexpr char kSecondPrefix[] = "2001:db8:420::/64";
  const auto topology = TopologyBuilder{"shared-peer-interface-name"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .syntheticNode("s2")
                            .prefix("s1", kFirstPrefix)
                            .prefix("s2", kSecondPrefix)
                            .link("r1", "s1", toFirst)
                            .link("r1", "s2", toSecond)
                            .build();
  OpenrE2eLab lab{topology};
  lab.start();

  constexpr std::chrono::seconds kTimeout{30};
  ASSERT_TRUE(lab.awaitAdjacency("s1", /*isUp=*/true, kTimeout));
  ASSERT_TRUE(lab.awaitAdjacency("s2", /*isUp=*/true, kTimeout));
  EXPECT_TRUE(lab.awaitProgrammedRoute(kFirstPrefix, {"s1"}, kTimeout));
  EXPECT_TRUE(lab.awaitProgrammedRoute(kSecondPrefix, {"s2"}, kTimeout));
}

TEST(OpenrE2eTopologyTest, RequiresRealRouter) {
  const auto topology =
      TopologyBuilder{"missing-router"}.syntheticNode("s1").build();
  EXPECT_THROW(OpenrE2eLab{topology}, TopologyError);
}

TEST(OpenrE2eTopologyTest, PeerStateRejectsRealOrUnknownNode) {
  const auto topology = TopologyBuilder{"peer-state-contract"}
                            .realNode("r1")
                            .syntheticNode("s1")
                            .link("r1", "s1")
                            .build();
  OpenrE2eLab lab{topology};
  lab.start();

  EXPECT_THROW(lab.setPeerState("r1", false), std::invalid_argument);
  EXPECT_THROW(lab.setPeerState("missing", false), TopologyError);
}

} // namespace
} // namespace openr::e2e
