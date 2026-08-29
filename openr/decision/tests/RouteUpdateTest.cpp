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
#include <openr/messaging/ReplicateQueue.h>

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

/*
 * An incoming INCREMENTAL is absorbed into the pending element (returns true,
 * so the queue appends nothing) and the net latest state per prefix wins.
 */
TEST(CoalesceDecisionRouteUpdates, IncrementalIsAbsorbed) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");
  const auto p2 = folly::IPAddress::createNetwork("10.0.2.0/24");

  DecisionRouteUpdate pending;
  pending.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1 /* nexthops */));

  DecisionRouteUpdate incoming;
  incoming.addRouteToUpdate(makeUnicast("10.0.1.0/24", 2 /* new value */));
  incoming.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));

  EXPECT_TRUE(coalesceDecisionRouteUpdates(pending, incoming));

  EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, pending.type);
  EXPECT_EQ(2, pending.unicastRoutesToUpdate.size());
  // Latest value for p1 won -> 2 next-hops.
  ASSERT_EQ(1, pending.unicastRoutesToUpdate.count(p1));
  EXPECT_EQ(2, pending.unicastRoutesToUpdate.at(p1).nexthops.size());
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.count(p2));
}

/*
 * A FULL_SYNC is the authoritative whole-table state, so it supersedes whatever
 * is pending rather than merging into it.
 */
TEST(CoalesceDecisionRouteUpdates, FullSyncReplacesPending) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");
  const auto p2 = folly::IPAddress::createNetwork("10.0.2.0/24");

  DecisionRouteUpdate pending;
  pending.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));

  DecisionRouteUpdate incoming;
  incoming.type = DecisionRouteUpdate::FULL_SYNC;
  incoming.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));

  EXPECT_TRUE(coalesceDecisionRouteUpdates(pending, incoming));

  // The pending delta is gone -- only the snapshot survives.
  EXPECT_EQ(DecisionRouteUpdate::FULL_SYNC, pending.type);
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.size());
  EXPECT_EQ(0, pending.unicastRoutesToUpdate.count(p1));
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.count(p2));
}

/*
 * Folding later incrementals onto a pending FULL_SYNC keeps it a FULL_SYNC, so
 * a coalesced backlog still delivers the whole-table reset signal downstream.
 * This is the property PrefixManager initialization depends on.
 */
TEST(CoalesceDecisionRouteUpdates, FullSyncBaseStaysFullSync) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");
  const auto p2 = folly::IPAddress::createNetwork("10.0.2.0/24");
  const auto p3 = folly::IPAddress::createNetwork("10.0.3.0/24");

  DecisionRouteUpdate pending;
  pending.type = DecisionRouteUpdate::FULL_SYNC;
  pending.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));
  pending.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));

  DecisionRouteUpdate incoming; // default INCREMENTAL
  incoming.unicastRoutesToDelete.push_back(p1);
  incoming.addRouteToUpdate(makeUnicast("10.0.3.0/24", 1));

  EXPECT_TRUE(coalesceDecisionRouteUpdates(pending, incoming));

  EXPECT_EQ(DecisionRouteUpdate::FULL_SYNC, pending.type);
  EXPECT_EQ(0, pending.unicastRoutesToUpdate.count(p1));
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.count(p2));
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.count(p3));
  // A full-sync never carries explicit delete lists.
  EXPECT_TRUE(pending.unicastRoutesToDelete.empty());
}

/*
 * End-to-end through ReplicateQueue: a reader wired with the coalescer
 * collapses to a single pending element no matter how many updates are pushed
 * while it is not draining, and coalescing is per-reader -- a plain reader on
 * the same queue still receives every element.
 */
TEST(CoalesceDecisionRouteUpdates, BoundsBacklogPerReader) {
  messaging::ReplicateQueue<DecisionRouteUpdate> q;
  auto plain = q.getReader("plain");
  auto coalesced = q.getReader("coalesced", coalesceDecisionRouteUpdates);

  constexpr size_t kPushes = 50;
  for (size_t i = 0; i < kPushes; ++i) {
    DecisionRouteUpdate update;
    update.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));
    q.push(std::move(update));
  }
  EXPECT_EQ(kPushes, plain.size());
  EXPECT_EQ(1, coalesced.size());

  // A FULL_SYNC supersedes the pending delta rather than adding an element.
  DecisionRouteUpdate fullSync;
  fullSync.type = DecisionRouteUpdate::FULL_SYNC;
  fullSync.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));
  q.push(std::move(fullSync));
  EXPECT_EQ(1, coalesced.size());

  /*
   * Later incrementals fold into the pending full-sync, keeping it whole-table
   * and still a single element.
   */
  for (size_t i = 0; i < kPushes; ++i) {
    DecisionRouteUpdate update;
    update.addRouteToUpdate(makeUnicast("10.0.3.0/24", 1));
    q.push(std::move(update));
  }
  EXPECT_EQ(1, coalesced.size());

  const auto snapshot = coalesced.get().value();
  EXPECT_EQ(DecisionRouteUpdate::FULL_SYNC, snapshot.type);
  EXPECT_EQ(2, snapshot.unicastRoutesToUpdate.size());

  q.close();
}

/*
 * Incremental-into-incremental merges exactly as the general coalescer does.
 */
TEST(CoalesceIncrementalRouteUpdates, IncrementalIsAbsorbed) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");
  const auto p2 = folly::IPAddress::createNetwork("10.0.2.0/24");

  DecisionRouteUpdate pending;
  pending.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));

  DecisionRouteUpdate incoming;
  incoming.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));

  EXPECT_TRUE(coalesceIncrementalRouteUpdates(pending, incoming));

  EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, pending.type);
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.count(p1));
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.count(p2));
}

/*
 * An incoming FULL_SYNC is appended rather than replacing the pending delta, so
 * a snoop client still sees the earlier delta before the snapshot.
 */
TEST(CoalesceIncrementalRouteUpdates, IncomingFullSyncIsAppended) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");

  DecisionRouteUpdate pending;
  pending.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));

  DecisionRouteUpdate incoming;
  incoming.type = DecisionRouteUpdate::FULL_SYNC;
  incoming.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));

  EXPECT_FALSE(coalesceIncrementalRouteUpdates(pending, incoming));

  // Neither side mutated -- the caller appends `incoming`.
  EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, pending.type);
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.size());
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.count(p1));
}

/*
 * The case that motivates this coalescer: an incremental is NOT folded into a
 * pending FULL_SYNC. Folding would apply the delete by dropping the key from
 * the snapshot without recording it in unicastRoutesToDelete, so a snoop client
 * -- which only ever sees toThrift() output and cannot read `type` -- would
 * never learn the route was withdrawn.
 */
TEST(CoalesceIncrementalRouteUpdates, FullSyncBaseIsNotMerged) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");

  DecisionRouteUpdate pending;
  pending.type = DecisionRouteUpdate::FULL_SYNC;
  pending.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));

  DecisionRouteUpdate incoming; // default INCREMENTAL
  incoming.unicastRoutesToDelete.push_back(p1);

  EXPECT_FALSE(coalesceIncrementalRouteUpdates(pending, incoming));

  /*
   * The snapshot is untouched and the withdrawal survives as its own element,
   * so the delete is still visible to a delta-applying client.
   */
  EXPECT_EQ(DecisionRouteUpdate::FULL_SYNC, pending.type);
  EXPECT_EQ(1, pending.unicastRoutesToUpdate.count(p1));
  ASSERT_EQ(1, incoming.unicastRoutesToDelete.size());
  EXPECT_EQ(p1, incoming.unicastRoutesToDelete.front());
}

/*
 * End-to-end through ReplicateQueue: the snoop-style reader settles at two
 * elements -- the initial FULL_SYNC plus one merged incremental -- no matter
 * how far behind it falls, and the delete in the incremental is still delivered
 * explicitly rather than being absorbed into the snapshot.
 */
TEST(CoalesceIncrementalRouteUpdates, BoundsSnoopBacklogAtTwo) {
  const auto p1 = folly::IPAddress::createNetwork("10.0.1.0/24");

  messaging::ReplicateQueue<DecisionRouteUpdate> q;
  auto snoop = q.getReader("snoop", coalesceIncrementalRouteUpdates);

  // Initial subscription snapshot.
  DecisionRouteUpdate fullSync;
  fullSync.type = DecisionRouteUpdate::FULL_SYNC;
  fullSync.addRouteToUpdate(makeUnicast("10.0.1.0/24", 1));
  q.push(std::move(fullSync));
  EXPECT_EQ(1, snoop.size());

  // A withdrawal plus a burst of churn while the client is stalled.
  DecisionRouteUpdate withdraw;
  withdraw.unicastRoutesToDelete.push_back(p1);
  q.push(std::move(withdraw));
  for (size_t i = 0; i < 50; ++i) {
    DecisionRouteUpdate update;
    update.addRouteToUpdate(makeUnicast("10.0.2.0/24", 1));
    q.push(std::move(update));
  }
  EXPECT_EQ(2, snoop.size());

  // The snapshot is delivered intact...
  const auto first = snoop.get().value();
  EXPECT_EQ(DecisionRouteUpdate::FULL_SYNC, first.type);
  EXPECT_EQ(1, first.unicastRoutesToUpdate.count(p1));

  // ...followed by a delta that still carries the explicit withdrawal.
  const auto second = snoop.get().value();
  EXPECT_EQ(DecisionRouteUpdate::INCREMENTAL, second.type);
  ASSERT_EQ(1, second.unicastRoutesToDelete.size());
  EXPECT_EQ(p1, second.unicastRoutesToDelete.front());

  q.close();
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
