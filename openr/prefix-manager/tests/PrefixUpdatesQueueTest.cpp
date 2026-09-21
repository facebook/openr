/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <openr/common/NetworkUtil.h>
#include <openr/prefix-manager/PrefixUpdatesQueue.h>

using namespace openr;

namespace {

thrift::PrefixEntry
makePrefix(const std::string& prefix) {
  thrift::PrefixEntry entry;
  entry.prefix() = toIpPrefix(prefix);
  entry.type() = thrift::PrefixType::LOOPBACK;
  return entry;
}

PrefixEvent
makeEvent(
    PrefixEventType eventType,
    const std::string& prefix,
    folly::F14FastSet<std::string> dstAreas = {},
    thrift::PrefixType prefixType = thrift::PrefixType::LOOPBACK) {
  return PrefixEvent(
      eventType, prefixType, {makePrefix(prefix)}, std::move(dstAreas));
}

PrefixEvent
makeWithdrawEvent(const std::string& prefix, const std::string& prefixEntry) {
  auto event = makeEvent(PrefixEventType::WITHDRAW_PREFIXES, prefix);
  event.prefixEntries.emplace_back(
      std::make_shared<thrift::PrefixEntry>(makePrefix(prefixEntry)),
      folly::F14FastSet<std::string>{});
  return event;
}

} // namespace

TEST(PrefixUpdatesQueueTest, DisabledPreservesFifo) {
  messaging::ReplicateQueue<PrefixEvent> queue;
  auto reader =
      getPrefixUpdatesQueueReader(queue, /*enableQueueCoalescing=*/false);

  const auto withdraw1 =
      makeEvent(PrefixEventType::WITHDRAW_PREFIXES, "10.0.0.1/32");
  const auto add =
      makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.2/32", {"area1"});
  const auto withdraw2 =
      makeEvent(PrefixEventType::WITHDRAW_PREFIXES, "10.0.0.3/32");
  queue.push(withdraw1);
  queue.push(add);
  queue.push(withdraw2);

  ASSERT_EQ(3, reader.size());
  EXPECT_EQ(withdraw1.prefixes, reader.get()->prefixes);
  EXPECT_EQ(add.prefixes, reader.get()->prefixes);
  EXPECT_EQ(withdraw2.prefixes, reader.get()->prefixes);
}

TEST(PrefixUpdatesQueueTest, WithdrawStartsNewPendingCycle) {
  messaging::ReplicateQueue<PrefixEvent> queue;
  auto reader =
      getPrefixUpdatesQueueReader(queue, /*enableQueueCoalescing=*/true);

  const auto withdraw1 = makeWithdrawEvent("10.0.0.1/32", "10.0.0.2/32");
  const auto staleAdd1 =
      makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.3/32", {"area1"});
  const auto staleAdd2 =
      makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.4/32", {"area2"});
  const auto withdraw2 = makeWithdrawEvent("10.0.0.5/32", "10.0.0.6/32");
  const auto freshAdd1 =
      makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.7/32", {"area1"});
  const auto freshAdd2 =
      makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.8/32", {"area2"});

  queue.push(withdraw1);
  queue.push(staleAdd1);
  queue.push(staleAdd2);
  queue.push(withdraw2);
  queue.push(freshAdd1);
  queue.push(freshAdd2);

  ASSERT_EQ(3, reader.size());
  auto mergedWithdraw = reader.get().value();
  EXPECT_EQ(PrefixEventType::WITHDRAW_PREFIXES, mergedWithdraw.eventType);
  EXPECT_EQ(
      (std::vector<thrift::PrefixEntry>{
          withdraw1.prefixes.front(), withdraw2.prefixes.front()}),
      mergedWithdraw.prefixes);
  EXPECT_EQ(
      (std::vector<PrefixEntry>{
          withdraw1.prefixEntries.front(), withdraw2.prefixEntries.front()}),
      mergedWithdraw.prefixEntries);
  EXPECT_EQ(freshAdd1.prefixes, reader.get()->prefixes);
  EXPECT_EQ(freshAdd2.prefixes, reader.get()->prefixes);
}

TEST(PrefixUpdatesQueueTest, WithdrawPurgesAddsWithoutPendingWithdraw) {
  messaging::ReplicateQueue<PrefixEvent> queue;
  auto reader =
      getPrefixUpdatesQueueReader(queue, /*enableQueueCoalescing=*/true);

  const auto staleAdd =
      makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.1/32", {"area1"});
  const auto withdraw =
      makeEvent(PrefixEventType::WITHDRAW_PREFIXES, "10.0.0.2/32");
  queue.push(staleAdd);
  queue.push(withdraw);

  ASSERT_EQ(1, reader.size());
  EXPECT_EQ(withdraw.prefixes, reader.get()->prefixes);
}

TEST(PrefixUpdatesQueueTest, EmptyWithdrawStartsNewPendingCycle) {
  messaging::ReplicateQueue<PrefixEvent> queue;
  auto reader =
      getPrefixUpdatesQueueReader(queue, /*enableQueueCoalescing=*/true);

  const PrefixEvent withdraw(
      PrefixEventType::WITHDRAW_PREFIXES, thrift::PrefixType::LOOPBACK);
  queue.push(withdraw);
  queue.push(makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.1/32"));
  queue.push(withdraw);

  ASSERT_EQ(1, reader.size());
  const auto mergedWithdraw = reader.get().value();
  EXPECT_EQ(PrefixEventType::WITHDRAW_PREFIXES, mergedWithdraw.eventType);
  EXPECT_TRUE(mergedWithdraw.prefixes.empty());
  EXPECT_TRUE(mergedWithdraw.prefixEntries.empty());
}

TEST(PrefixUpdatesQueueTest, RepeatedWithdrawIsDeduplicated) {
  messaging::ReplicateQueue<PrefixEvent> queue;
  auto reader =
      getPrefixUpdatesQueueReader(queue, /*enableQueueCoalescing=*/true);

  const auto withdraw = makeWithdrawEvent("10.0.0.1/32", "10.0.0.2/32");
  queue.push(withdraw);
  queue.push(makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.3/32"));
  queue.push(withdraw);

  ASSERT_EQ(1, reader.size());
  const auto mergedWithdraw = reader.get().value();
  EXPECT_EQ(withdraw.prefixes, mergedWithdraw.prefixes);
  EXPECT_EQ(withdraw.prefixEntries, mergedWithdraw.prefixEntries);
}

TEST(PrefixUpdatesQueueTest, OtherOperationsAreDropped) {
  messaging::ReplicateQueue<PrefixEvent> queue;
  auto reader =
      getPrefixUpdatesQueueReader(queue, /*enableQueueCoalescing=*/true);

  const auto withdraw1 =
      makeEvent(PrefixEventType::WITHDRAW_PREFIXES, "10.0.0.1/32");
  const auto staleAdd = makeEvent(PrefixEventType::ADD_PREFIXES, "10.0.0.2/32");
  const auto sync =
      makeEvent(PrefixEventType::SYNC_PREFIXES_BY_TYPE, "10.0.0.3/32");
  const auto withdraw2 =
      makeEvent(PrefixEventType::WITHDRAW_PREFIXES, "10.0.0.4/32");
  queue.push(withdraw1);
  queue.push(staleAdd);
  queue.push(sync);
  queue.push(withdraw2);

  ASSERT_EQ(1, reader.size());
  EXPECT_EQ(
      (std::vector<thrift::PrefixEntry>{
          withdraw1.prefixes.front(), withdraw2.prefixes.front()}),
      reader.get()->prefixes);
}

TEST(PrefixUpdatesQueueTest, OtherPrefixTypesAreDropped) {
  messaging::ReplicateQueue<PrefixEvent> queue;
  auto reader =
      getPrefixUpdatesQueueReader(queue, /*enableQueueCoalescing=*/true);

  const auto withdraw1 =
      makeEvent(PrefixEventType::WITHDRAW_PREFIXES, "10.0.0.1/32");
  const auto bgpAdd = makeEvent(
      PrefixEventType::ADD_PREFIXES,
      "10.0.0.2/32",
      {},
      thrift::PrefixType::BGP);
  const auto withdraw2 =
      makeEvent(PrefixEventType::WITHDRAW_PREFIXES, "10.0.0.3/32");
  queue.push(withdraw1);
  queue.push(bgpAdd);
  queue.push(withdraw2);

  ASSERT_EQ(1, reader.size());
  EXPECT_EQ(
      (std::vector<thrift::PrefixEntry>{
          withdraw1.prefixes.front(), withdraw2.prefixes.front()}),
      reader.get()->prefixes);
}
