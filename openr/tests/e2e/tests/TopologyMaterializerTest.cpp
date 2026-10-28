/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <gtest/gtest.h>
#include <openr/common/Util.h>
#include <openr/tests/e2e/TopologyAllocator.h>
#include <openr/tests/e2e/TopologyBuilder.h>
#include <openr/tests/e2e/TopologyMaterializer.h>

namespace openr::e2e {

TEST(
    TopologyMaterializerTest, Materialize_RepeatedSnapshot_IncrementsVersions) {
  const auto topology = TopologyAllocator::allocate(
      TopologyBuilder{"snapshot"}.syntheticNode("r1").build());
  KvStoreVersionStore versions;
  TopologyMaterializer materializer{versions};

  const auto first = materializer.materialize(topology);
  const auto second = materializer.materialize(topology);

  ASSERT_EQ(1, first.size());
  EXPECT_EQ(KvStoreUpdateType::SET, first.front().type);
  EXPECT_EQ(1, first.front().version);
  ASSERT_TRUE(first.front().value.has_value());
  EXPECT_EQ(1, *first.front().value->version());
  EXPECT_EQ(2, second.front().version);
}

TEST(TopologyMaterializerTest, Diff_RemovedPrefix_EmitsVersionedWithdrawal) {
  const auto before = TopologyAllocator::allocate(
      TopologyBuilder{"before"}
          .syntheticNode("r1")
          .prefix("10.1.0.0/16")
          .build());
  const auto after = TopologyAllocator::allocate(
      TopologyBuilder{"after"}.syntheticNode("r1").build());
  KvStoreVersionStore versions;
  TopologyMaterializer materializer{versions};
  materializer.materialize(before);

  const auto updates = materializer.diff(before, after, {"r1"});

  ASSERT_EQ(1, updates.size());
  EXPECT_EQ(KvStoreUpdateType::WITHDRAW, updates.front().type);
  EXPECT_EQ(2, updates.front().version);
  ASSERT_TRUE(updates.front().value.has_value());
  ASSERT_TRUE(updates.front().value->value().has_value());
  apache::thrift::CompactSerializer serializer;
  const auto database = readThriftObjStr<thrift::PrefixDatabase>(
      *updates.front().value->value(), serializer);
  EXPECT_TRUE(*database.deletePrefix());
}

} // namespace openr::e2e
