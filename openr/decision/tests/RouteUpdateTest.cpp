/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/IPAddress.h>
#include <folly/container/F14Set.h>

#include <openr/common/LsdbUtil.h>
#include <openr/decision/RouteUpdate.h>

namespace openr {

namespace {

// Build a unicast entry for `cidr` with `numNextHops` distinct next-hops, so
// the next-hop count can distinguish "which version won" after a merge.
RibUnicastEntry
makeUnicast(const std::string& cidr, int numNextHops) {
  static const std::vector<std::string> kAddrs = {
      "fe80::1", "fe80::2", "fe80::3", "fe80::4"};
  folly::F14FastSet<thrift::NextHopThrift> nhs;
  for (int i = 0; i < numNextHops; ++i) {
    nhs.insert(createNextHop(
        toBinaryAddress(folly::IPAddress(kAddrs.at(i))), "iface"));
  }
  return RibUnicastEntry(folly::IPAddress::createNetwork(cidr), std::move(nhs));
}

RibMplsEntry
makeMpls(int32_t label, int numNextHops) {
  static const std::vector<std::string> kAddrs = {
      "fe80::1", "fe80::2", "fe80::3", "fe80::4"};
  folly::F14FastSet<thrift::NextHopThrift> nhs;
  for (int i = 0; i < numNextHops; ++i) {
    nhs.insert(createNextHop(
        toBinaryAddress(folly::IPAddress(kAddrs.at(i))), "iface"));
  }
  return RibMplsEntry(label, std::move(nhs));
}

} // namespace

/*
 * A later update supersedes a prior update for the same prefix (latest value
 * wins), and disjoint updates accumulate.
 */
TEST(DecisionRouteUpdateMerge, UpdateSupersedesUpdateAndAccumulates) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");
  const auto p2 = folly::IPAddress::createNetwork("10.0.2.0/24");

  DecisionRouteUpdate base;
  base.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1 /* nexthops */));

  DecisionRouteUpdate next;
  next.addRouteToUpdate(makeUnicast("10.0.1.0/24", 2 /* new value */));
  next.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));

  base.mergeInPlace(std::move(next));

  EXPECT_EQ(2, base.unicastRoutesToUpdate.size());
  EXPECT_TRUE(base.unicastRoutesToDelete.empty());
  // Latest value for p1 won -> 2 next-hops.
  ASSERT_EQ(1, base.unicastRoutesToUpdate.count(p1));
  EXPECT_EQ(2, base.unicastRoutesToUpdate.at(p1).nexthops.size());
  EXPECT_EQ(1, base.unicastRoutesToUpdate.count(p2));
}

/*
 * A later delete supersedes a prior update for the same prefix.
 */
TEST(DecisionRouteUpdateMerge, DeleteSupersedesUpdate) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");

  DecisionRouteUpdate base;
  base.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));

  DecisionRouteUpdate next;
  next.unicastRoutesToDelete.push_back(p1);

  base.mergeInPlace(std::move(next));

  EXPECT_EQ(0, base.unicastRoutesToUpdate.count(p1));
  ASSERT_EQ(1, base.unicastRoutesToDelete.size());
  EXPECT_EQ(p1, base.unicastRoutesToDelete.front());
}

/*
 * A later update supersedes a prior delete for the same prefix (prefix ends up
 * programmed, not deleted).
 */
TEST(DecisionRouteUpdateMerge, UpdateSupersedesDelete) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");

  DecisionRouteUpdate base;
  base.unicastRoutesToDelete.push_back(p1);

  DecisionRouteUpdate next;
  next.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));

  base.mergeInPlace(std::move(next));

  EXPECT_TRUE(base.unicastRoutesToDelete.empty());
  EXPECT_EQ(1, base.unicastRoutesToUpdate.count(p1));
}

/*
 * MPLS routes follow the same update/delete reconciliation.
 */
TEST(DecisionRouteUpdateMerge, MplsUpdateDeleteReconcile) {
  DecisionRouteUpdate base;
  base.addMplsRouteToUpdate(makeMpls(100, 1));
  base.mplsRoutesToDelete.push_back(200);

  DecisionRouteUpdate next;
  next.mplsRoutesToDelete.push_back(100); // delete supersedes prior update
  next.addMplsRouteToUpdate(makeMpls(200, 1)); // update supersedes prior delete

  base.mergeInPlace(std::move(next));

  EXPECT_EQ(0, base.mplsRoutesToUpdate.count(100));
  EXPECT_EQ(1, base.mplsRoutesToUpdate.count(200));
  ASSERT_EQ(1, base.mplsRoutesToDelete.size());
  EXPECT_EQ(100, base.mplsRoutesToDelete.front());
}

/*
 * Disjoint updates/deletes from both sides are all preserved.
 */
TEST(DecisionRouteUpdateMerge, DisjointPreserved) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");
  const auto p2 = folly::IPAddress::createNetwork("10.0.2.0/24");
  const auto p3 = folly::IPAddress::createNetwork("10.0.3.0/24");
  const auto p4 = folly::IPAddress::createNetwork("10.0.4.0/24");

  DecisionRouteUpdate base;
  base.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));
  base.unicastRoutesToDelete.push_back(p2);

  DecisionRouteUpdate next;
  next.addRouteToUpdate(makeUnicast("10.0.3.0/24", 1));
  next.unicastRoutesToDelete.push_back(p4);

  base.mergeInPlace(std::move(next));

  EXPECT_EQ(2, base.unicastRoutesToUpdate.size());
  EXPECT_EQ(1, base.unicastRoutesToUpdate.count(p1));
  EXPECT_EQ(1, base.unicastRoutesToUpdate.count(p3));
  EXPECT_EQ(2, base.unicastRoutesToDelete.size());
  const folly::F14FastSet<folly::CIDRNetwork> deletes(
      base.unicastRoutesToDelete.begin(), base.unicastRoutesToDelete.end());
  EXPECT_EQ(1, deletes.count(p2));
  EXPECT_EQ(1, deletes.count(p4));
}

/*
 * perfEvents/prefixType take the later update's value when set, and are
 * retained from the base when the later update leaves them unset.
 */
TEST(DecisionRouteUpdateMerge, MetadataLatestWinsButRetainsWhenUnset) {
  // Base has metadata, next does not -> retained.
  {
    DecisionRouteUpdate base;
    base.perfEvents = thrift::PerfEvents{};
    base.prefixType = thrift::PrefixType::BGP;
    DecisionRouteUpdate next; // no metadata
    base.mergeInPlace(std::move(next));
    EXPECT_TRUE(base.perfEvents.has_value());
    ASSERT_TRUE(base.prefixType.has_value());
    EXPECT_EQ(thrift::PrefixType::BGP, *base.prefixType);
  }
  // Next has metadata -> overrides.
  {
    DecisionRouteUpdate base; // no metadata
    DecisionRouteUpdate next;
    next.perfEvents = thrift::PerfEvents{};
    next.prefixType = thrift::PrefixType::VIP;
    base.mergeInPlace(std::move(next));
    EXPECT_TRUE(base.perfEvents.has_value());
    ASSERT_TRUE(base.prefixType.has_value());
    EXPECT_EQ(thrift::PrefixType::VIP, *base.prefixType);
  }
}

/*
 * mergeInPlace does not change the base's type: applying an incremental delta
 * onto a FULL_SYNC base keeps it a FULL_SYNC (with the delta applied). This is
 * relied on by the queue-side coalescer so a pending full-sync stays a
 * full-sync when later incrementals are folded into it.
 */
TEST(DecisionRouteUpdateMerge, PreservesBaseType) {
  DecisionRouteUpdate base;
  base.type = DecisionRouteUpdate::FULL_SYNC;
  base.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));

  DecisionRouteUpdate next; // default INCREMENTAL
  next.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));

  base.mergeInPlace(std::move(next));

  EXPECT_EQ(DecisionRouteUpdate::FULL_SYNC, base.type);
  EXPECT_EQ(2, base.unicastRoutesToUpdate.size());
}

/*
 * Folding an INCREMENTAL delta into a FULL_SYNC base keeps whole-table
 * semantics: a deleted key is simply dropped from the snapshot's update map and
 * NO explicit delete entry is added (a delete list is meaningless on a full
 * snapshot); an updated key is applied into the snapshot.
 */
TEST(DecisionRouteUpdateMerge, FullSyncBaseDeleteDropsFromSnapshot) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");
  const auto p2 = folly::IPAddress::createNetwork("10.0.2.0/24");
  const auto p3 = folly::IPAddress::createNetwork("10.0.3.0/24");

  DecisionRouteUpdate base;
  base.type = DecisionRouteUpdate::FULL_SYNC;
  base.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));
  base.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));
  base.addMplsRouteToUpdate(makeMpls(100, 1));

  DecisionRouteUpdate next; // default INCREMENTAL
  next.unicastRoutesToDelete.push_back(p1); // drop p1 from the snapshot
  next.addRouteToUpdate(makeUnicast("10.0.3.0/24", 1)); // add p3
  next.mplsRoutesToDelete.push_back(100); // drop label 100

  base.mergeInPlace(std::move(next));

  // Stays a whole-table snapshot.
  EXPECT_EQ(DecisionRouteUpdate::FULL_SYNC, base.type);
  // p1 dropped from the snapshot, p2 retained, p3 added.
  EXPECT_EQ(2, base.unicastRoutesToUpdate.size());
  EXPECT_EQ(0, base.unicastRoutesToUpdate.count(p1));
  EXPECT_EQ(1, base.unicastRoutesToUpdate.count(p2));
  EXPECT_EQ(1, base.unicastRoutesToUpdate.count(p3));
  // MPLS label dropped from the snapshot.
  EXPECT_EQ(0, base.mplsRoutesToUpdate.count(100));
  // A full-sync never carries explicit delete lists.
  EXPECT_TRUE(base.unicastRoutesToDelete.empty());
  EXPECT_TRUE(base.mplsRoutesToDelete.empty());
}

} // namespace openr

int
main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  FLAGS_logtostderr = true;
  return RUN_ALL_TESTS();
}
