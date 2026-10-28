/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdexcept>

#include <gtest/gtest.h>
#include <openr/tests/e2e/KvStoreVersionStore.h>

namespace openr::e2e {

TEST(KvStoreVersionStoreTest, Next_RepeatedKey_IncreasesMonotonically) {
  KvStoreVersionStore versions;
  const KvStoreKeyRef key{"pod", "adj:r1"};

  EXPECT_EQ(1, versions.next({key}).front().version);
  EXPECT_EQ(2, versions.next({key}).front().version);
  EXPECT_EQ(2, versions.current(key));
}

TEST(KvStoreVersionStoreTest, Next_SameKeyDifferentAreas_IsIndependent) {
  KvStoreVersionStore versions;

  const auto assigned = versions.next({{"pod", "adj:r1"}, {"plane", "adj:r1"}});

  ASSERT_EQ(2, assigned.size());
  EXPECT_EQ(1, assigned.at(0).version);
  EXPECT_EQ(1, assigned.at(1).version);
  EXPECT_NE(scopedKvStoreKey(assigned.at(0)), scopedKvStoreKey(assigned.at(1)));
}

TEST(KvStoreVersionStoreTest, Next_DuplicateBatchKey_RejectsAtomically) {
  KvStoreVersionStore versions;
  const KvStoreKeyRef key{"pod", "adj:r1"};

  EXPECT_THROW(versions.next({key, key}), std::invalid_argument);
  EXPECT_EQ(0, versions.current(key));
}

} // namespace openr::e2e
