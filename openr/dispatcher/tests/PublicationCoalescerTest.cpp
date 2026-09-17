/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <openr/dispatcher/PublicationCoalescer.h>

#include <set>
#include <string>
#include <vector>

#include <fmt/core.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace openr {

namespace {

const std::string kArea{"area-a"};
const std::string kOtherArea{"area-b"};

// Value carrying a payload, i.e. a real advertisement.
thrift::Value
value(const std::string& data, int64_t version = 1) {
  return createThriftValue(version, "node1", data);
}

/*
 * TTL refresh: same (version, originatorId) as the value it refreshes, higher
 * ttlVersion, and crucially NO payload.
 */
thrift::Value
ttlRefresh(int64_t version = 1, int64_t ttlVersion = 2) {
  return createThriftValue(
      version, "node1", std::nullopt /* data */, 30000 /* ttl */, ttlVersion);
}

KvStorePublication
pub(const thrift::KeyVals& keyVals,
    const std::vector<std::string>& expiredKeys = {},
    const std::string& area = kArea) {
  return KvStorePublication(createThriftPublication(
      keyVals, expiredKeys, std::nullopt, std::nullopt, area));
}

thrift::Publication&
asPub(KvStorePublication& publication) {
  return std::get<thrift::Publication>(publication);
}

std::set<std::string>
keyNames(KvStorePublication& publication) {
  std::set<std::string> keys;
  for (const auto& [key, unused] : *asPub(publication).keyVals()) {
    keys.emplace(key);
  }
  return keys;
}

std::set<std::string>
expiredNames(KvStorePublication& publication) {
  const auto& expired = *asPub(publication).expiredKeys();
  return std::set<std::string>(expired.begin(), expired.end());
}

} // namespace

/*
 * A publication is keyed by its area and explicitly merged, preserving every
 * independent key delta.
 */
TEST(ClassifyKvStorePublication, PublicationIsKeyedByAreaAndMerges) {
  auto first = pub({{"key1", value("v1")}}, {}, kArea);
  auto second = pub({{"key2", value("v2")}}, {}, kOtherArea);

  const auto firstKey = classifyKvStorePublication(first);
  const auto secondKey = classifyKvStorePublication(second);

  EXPECT_EQ(kArea, firstKey.key);
  EXPECT_EQ(kOtherArea, secondKey.key);
  EXPECT_EQ(messaging::StateSuppressionAction::MERGE_PENDING, firstKey.action);
  EXPECT_EQ(messaging::StateSuppressionAction::MERGE_PENDING, secondKey.action);
}

/*
 * An InitializationEvent must survive and start a new coalescing epoch for
 * every area, so it is a KEY_BARRIER under the empty key.
 */
TEST(ClassifyKvStorePublication, InitializationEventIsAGlobalKeyBarrier) {
  KvStorePublication synced{thrift::InitializationEvent::KVSTORE_SYNCED};
  KvStorePublication adjSynced{
      thrift::InitializationEvent::ADJACENCY_DB_SYNCED};

  for (auto* event : {&synced, &adjSynced}) {
    const auto stateKey = classifyKvStorePublication(*event);
    EXPECT_EQ(messaging::StateSuppressionAction::KEY_BARRIER, stateKey.action);
    EXPECT_TRUE(stateKey.key.empty());
  }
}

TEST(KvStorePublicationSuppressionPolicy, EventStartsNewCoalescingEpoch) {
  messaging::RWQueue<KvStorePublication> queue(
      "kvstore-publications", getKvStorePublicationSuppressionPolicy());

  queue.push(pub({{"A1", value("A1")}}, {}, kArea));
  queue.push(pub({{"B1", value("B1")}}, {}, kOtherArea));
  queue.push(KvStorePublication{thrift::InitializationEvent::KVSTORE_SYNCED});
  queue.push(pub({{"A2", value("A2")}}, {}, kArea));
  queue.push(pub({{"B2", value("B2")}}, {}, kOtherArea));
  queue.push(pub({{"A3", value("A3")}}, {}, kArea));

  ASSERT_EQ(5, queue.size());

  auto firstA = queue.get().value();
  auto firstB = queue.get().value();
  auto event = queue.get().value();
  auto secondB = queue.get().value();
  auto mergedA = queue.get().value();

  EXPECT_EQ((std::set<std::string>{"A1"}), keyNames(firstA));
  EXPECT_EQ((std::set<std::string>{"B1"}), keyNames(firstB));
  EXPECT_EQ(
      thrift::InitializationEvent::KVSTORE_SYNCED,
      std::get<thrift::InitializationEvent>(event));
  EXPECT_EQ((std::set<std::string>{"B2"}), keyNames(secondB));
  EXPECT_EQ((std::set<std::string>{"A2", "A3"}), keyNames(mergedA));
}

/*
 * An InitializationEvent is a one-shot ordering signal, so it is a hard
 * barrier in both directions: it is never absorbed, and nothing is absorbed
 * into it.
 */
TEST(CoalesceKvStorePublications, InitializationEventIsABarrier) {
  auto publication = pub({{"key1", value("v1")}});
  KvStorePublication event{thrift::InitializationEvent::KVSTORE_SYNCED};

  EXPECT_FALSE(coalesceKvStorePublications(publication, event));
  EXPECT_FALSE(coalesceKvStorePublications(event, publication));
}

/*
 * Disjoint keys accumulate and a later value supersedes an earlier one for the
 * same key.
 */
TEST(CoalesceKvStorePublications, LatestValueWinsPerKey) {
  auto existing = pub({{"key1", value("old")}, {"key2", value("v2")}});
  auto incoming = pub({{"key1", value("new")}, {"key3", value("v3")}});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  const std::set<std::string> expected{"key1", "key2", "key3"};
  EXPECT_EQ(expected, keyNames(existing));
  EXPECT_EQ("new", *asPub(existing).keyVals()->at("key1").value());
}

/*
 * A TTL refresh carries no payload. Folding one onto a pending value update
 * must refresh liveness WITHOUT dropping that payload, otherwise a reader that
 * has not drained yet would never see the value.
 */
TEST(CoalesceKvStorePublications, TtlRefreshPreservesPendingPayload) {
  auto existing = pub({{"key1", value("payload", 1 /* version */)}});
  auto incoming =
      pub({{"key1", ttlRefresh(1 /* version */, 7 /* ttlVersion */)}});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  const auto& merged = asPub(existing).keyVals()->at("key1");
  ASSERT_TRUE(merged.value().has_value());
  EXPECT_EQ("payload", *merged.value());
  EXPECT_EQ(7, *merged.ttlVersion());
}

/*
 * The payload is only preserved for a refresh of the SAME (version,
 * originatorId). A value-less update at a higher version is a genuine
 * replacement and must be applied as-is.
 */
TEST(CoalesceKvStorePublications, ValuelessUpdateAtNewVersionReplaces) {
  auto existing = pub({{"key1", value("payload", 1 /* version */)}});
  auto incoming =
      pub({{"key1", ttlRefresh(2 /* version */, 0 /* ttlVersion */)}});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  const auto& merged = asPub(existing).keyVals()->at("key1");
  EXPECT_FALSE(merged.value().has_value());
  EXPECT_EQ(2, *merged.version());
}

/*
 * A later expiry supersedes a pending update: the key leaves keyVals and
 * appears in expiredKeys, so the two collections stay disjoint.
 */
TEST(CoalesceKvStorePublications, ExpirySupersedesPendingUpdate) {
  auto existing = pub({{"key1", value("v1")}, {"key2", value("v2")}});
  auto incoming = pub({}, {"key1"});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  const std::set<std::string> remaining{"key2"};
  const std::set<std::string> expired{"key1"};
  EXPECT_EQ(remaining, keyNames(existing));
  EXPECT_EQ(expired, expiredNames(existing));
}

/*
 * A later re-advertisement supersedes a pending expiry, so a resurrected key
 * is not left in expiredKeys where consumers would delete it again.
 */
TEST(CoalesceKvStorePublications, ReadvertisementSupersedesPendingExpiry) {
  auto existing = pub({}, {"key1", "key2"});
  auto incoming = pub({{"key1", value("back")}});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  const std::set<std::string> alive{"key1"};
  const std::set<std::string> stillExpired{"key2"};
  EXPECT_EQ(alive, keyNames(existing));
  EXPECT_EQ(stillExpired, expiredNames(existing));
}

/*
 * expiredKeys is a list on the wire, so the same key expiring across two
 * merges must not accumulate duplicates.
 */
TEST(CoalesceKvStorePublications, ExpiredKeysAreDeduplicated) {
  auto existing = pub({}, {"key1"});
  auto incoming = pub({}, {"key1", "key2"});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  EXPECT_EQ(2, asPub(existing).expiredKeys()->size());
  const std::set<std::string> expired{"key1", "key2"};
  EXPECT_EQ(expired, expiredNames(existing));
}

/*
 * A value-less TTL refresh is not a re-advertisement: it must NOT clear a
 * pending tombstone, otherwise the reader is told the key is alive while
 * receiving no value and the expiry is lost outright.
 */
TEST(CoalesceKvStorePublications, TtlRefreshDoesNotResurrectExpiredKey) {
  auto existing = pub({}, {"key1"});
  auto incoming = pub({{"key1", ttlRefresh()}});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  const std::set<std::string> stillExpired{"key1"};
  EXPECT_EQ(stillExpired, expiredNames(existing));
  /*
   * And it must not linger in keyVals as a payload-less entry beside its own
   * tombstone: consumers apply keyVals then expiredKeys, so a key in both is
   * resurrected and immediately deleted.
   */
  EXPECT_TRUE(keyNames(existing).empty());
}

/*
 * Processing updates against the temporary expiry set before processing
 * incoming expiries keeps the collections disjoint even when a key appears in
 * both incoming collections.
 */
TEST(CoalesceKvStorePublications, KeyValsAndExpiredKeysAlwaysDisjoint) {
  auto existing = pub({{"alive", value("v")}}, {"tombstoned"});
  auto incoming =
      pub({{"tombstoned", ttlRefresh()}, {"alive", value("v2")}}, {"alive"});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  const auto alive = keyNames(existing);
  const auto expired = expiredNames(existing);
  for (const auto& key : expired) {
    EXPECT_EQ(0, alive.count(key)) << key << " is in both collections";
  }
  // Expiry wins for a key that is both re-advertised and expired.
  EXPECT_EQ(1, expired.count("alive"));
}

/*
 * Defence in depth for the disjointness invariant: even if one publication
 * ever carried the same key in both collections, expiry must win and the key
 * must not end up in both.
 */
TEST(CoalesceKvStorePublications, SameKeyInBothIncomingCollectionsExpires) {
  auto existing = pub({{"key2", value("v2")}});
  auto incoming = pub({{"key1", value("v1")}}, {"key1"});

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  const std::set<std::string> alive{"key2"};
  const std::set<std::string> expired{"key1"};
  EXPECT_EQ(alive, keyNames(existing));
  EXPECT_EQ(expired, expiredNames(existing));
}

/*
 * A stale TTL refresh (lower ttlVersion) must not roll the pending entry
 * backwards, so the merge is monotonic regardless of arrival order.
 */
TEST(CoalesceKvStorePublications, StaleTtlRefreshIsIgnored) {
  auto existing = pub({{"key1", value("payload")}});
  auto fresh = pub({{"key1", ttlRefresh(1 /* version */, 9 /* ttlVersion */)}});
  ASSERT_TRUE(coalesceKvStorePublications(existing, fresh));

  auto stale = pub({{"key1", ttlRefresh(1 /* version */, 4 /* ttlVersion */)}});
  EXPECT_TRUE(coalesceKvStorePublications(existing, stale));

  EXPECT_EQ(9, *asPub(existing).keyVals()->at("key1").ttlVersion());
}

/*
 * Metadata belongs to the original pending publication and is not reconciled
 * while key-value state is merged.
 */
TEST(CoalesceKvStorePublications, KeepsOriginalPerfEvents) {
  auto existing = pub({{"key1", value("v1")}});
  auto incoming = pub({{"key2", value("v2")}});

  thrift::PerfEvents oldest;
  oldest.events()->emplace_back(createPerfEvent("node1", "OLDEST", 1000));
  asPub(existing).perfEvents() = oldest;

  thrift::PerfEvents newest;
  newest.events()->emplace_back(createPerfEvent("node1", "NEWEST", 2000));
  asPub(incoming).perfEvents() = newest;

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  ASSERT_TRUE(asPub(existing).perfEvents().has_value());
  ASSERT_EQ(1, asPub(existing).perfEvents()->events()->size());
  EXPECT_EQ(
      "OLDEST", *asPub(existing).perfEvents()->events()->front().eventDescr());
}

/*
 * An original publication without perfEvents remains unstamped even when the
 * incoming publication carries them.
 */
TEST(CoalesceKvStorePublications, DoesNotAdoptIncomingPerfEvents) {
  auto existing = pub({{"key1", value("v1")}});
  auto incoming = pub({{"key2", value("v2")}});

  thrift::PerfEvents events;
  events.events()->emplace_back(createPerfEvent("node1", "ONLY", 1000));
  asPub(incoming).perfEvents() = events;

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  EXPECT_FALSE(asPub(existing).perfEvents().has_value());
}

TEST(CoalesceKvStorePublications, KeepsOriginalTimestamp) {
  auto existing = pub({{"key1", value("v1")}});
  auto incoming = pub({{"key2", value("v2")}});
  asPub(existing).timestamp_ms() = 1000;
  asPub(incoming).timestamp_ms() = 2000;

  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  EXPECT_EQ(1000, asPub(existing).timestamp_ms().value());
}

/*
 * Regression guard for the O(N^2)-under-the-lock pathology that D117840303
 * removed from the route-update coalescer: accumulate many distinct expiries,
 * then resurrect the first, middle and last and verify only those tombstones
 * are dropped.
 */
TEST(CoalesceKvStorePublications, AccumulatesAndResurrectsManyExpiredKeys) {
  constexpr size_t kNumExpired{2048};

  auto existing = pub({});
  for (size_t i = 0; i < kNumExpired; ++i) {
    auto incoming = pub({}, {fmt::format("key{}", i)});
    // Precondition for everything below; EXPECT_ would emit 2048 failures.
    ASSERT_TRUE(coalesceKvStorePublications(existing, incoming));
  }
  ASSERT_EQ(kNumExpired, asPub(existing).expiredKeys()->size());

  const std::vector<size_t> resurrected{0, kNumExpired / 2, kNumExpired - 1};
  thrift::KeyVals readvertised;
  for (const auto index : resurrected) {
    readvertised.emplace(fmt::format("key{}", index), value("back"));
  }
  auto incoming = pub(readvertised);
  EXPECT_TRUE(coalesceKvStorePublications(existing, incoming));

  EXPECT_EQ(
      kNumExpired - resurrected.size(), asPub(existing).expiredKeys()->size());

  const auto expired = expiredNames(existing);
  for (const auto index : resurrected) {
    const auto key = fmt::format("key{}", index);
    EXPECT_EQ(0, expired.count(key)) << key << " should no longer be expired";
    EXPECT_EQ(1, asPub(existing).keyVals()->count(key));
  }
}

} // namespace openr
