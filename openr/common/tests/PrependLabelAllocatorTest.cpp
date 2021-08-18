/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/PrependLabelAllocator.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <optional>

namespace openr {

class PrependLabelAllocatorTestFixture : public ::testing::Test {
 public:
  PrependLabelAllocatorTestFixture() = default;
  ~PrependLabelAllocatorTestFixture() override = default;

  void
  SetUp() override {
    config_ = std::make_shared<Config>(createConfig());
    prependLabelAllocator_ = std::make_unique<PrependLabelAllocator>(config_);
  }

  virtual thrift::OpenrConfig
  createConfig() {
    // generate a config for testing
    auto tConfig = getBasicOpenrConfig(
        "1",
        "domain",
        {},
        false, /* enableV4 */
        true, /* enableSegmentRouting */
        false, /* dryrun */
        false, /* enableV4OverV6Nexthop */
        false, /* enableAdjLabels */
        true /* enablePrependLabels */
    );

    return tConfig;
  }

  std::shared_ptr<Config> config_;
  std::unique_ptr<PrependLabelAllocator> prependLabelAllocator_;
};

/**
 * Empty nexthop set should return no label while incrementing
 * or decrementing reference count.
 */
TEST_F(PrependLabelAllocatorTestFixture, EmptyNextHopSet) {
  std::set<folly::IPAddress> labelNextHopSet;
  auto [label, isNew] =
      prependLabelAllocator_->incrementRefCount(labelNextHopSet);
  EXPECT_EQ(isNew, false);
  EXPECT_EQ(label.has_value(), false);

  auto oldLabel = prependLabelAllocator_->decrementRefCount(labelNextHopSet);
  EXPECT_EQ(oldLabel.has_value(), false);
}

/**
 * Increment reference count for a new nexthop set and assign a label.
 * Then decrement reference count and verify value of the released label.
 */
TEST_F(PrependLabelAllocatorTestFixture, AllocateAndDeAllocateLabels) {
  std::set<folly::IPAddress> labelNextHopSet;
  labelNextHopSet.emplace(folly::IPAddress("192.168.0.1"));
  labelNextHopSet.emplace(folly::IPAddress("192.168.0.2"));

  auto [label, isNew] =
      prependLabelAllocator_->incrementRefCount(labelNextHopSet);
  EXPECT_EQ(isNew, true);
  EXPECT_EQ(label.value(), 60000);

  auto oldLabel = prependLabelAllocator_->decrementRefCount(labelNextHopSet);
  EXPECT_EQ(oldLabel, 60000);
}

/**
 * Make sure when reference count is increased for an existing nexthop set, no
 * new label is created.
 */
TEST_F(PrependLabelAllocatorTestFixture, NoNewAllocation) {
  std::set<folly::IPAddress> labelNextHopSet;
  labelNextHopSet.emplace(folly::IPAddress("192.168.0.1"));
  labelNextHopSet.emplace(folly::IPAddress("192.168.0.2"));

  auto [label, isNew] =
      prependLabelAllocator_->incrementRefCount(labelNextHopSet);
  EXPECT_EQ(isNew, true);
  EXPECT_EQ(label.value(), 60000);

  // Make sure current label is non-zero and no new label is created.
  auto pair = prependLabelAllocator_->incrementRefCount(labelNextHopSet);
  EXPECT_EQ(pair.second, false);
  EXPECT_EQ(pair.first.value(), 60000);
}

/**
 * Allocate a label to a nexthop group.
 * Released the label and the nexthop group.
 * Allocate label to a new nexthop group.
 * Make sure old label gets re-used.
 */
TEST_F(PrependLabelAllocatorTestFixture, AllocateFreedLabelToNewNextHopSet) {
  // create a new nexthop set and increase refcount
  std::set<folly::IPAddress> labelNextHopSet;
  labelNextHopSet.emplace(folly::IPAddress("192.168.0.1"));
  labelNextHopSet.emplace(folly::IPAddress("192.168.0.2"));

  auto [label, isNew] =
      prependLabelAllocator_->incrementRefCount(labelNextHopSet);
  EXPECT_EQ(isNew, true);
  EXPECT_EQ(label.value(), 60000);

  // Decrement reference count and delete the label
  auto oldLabel = prependLabelAllocator_->decrementRefCount(labelNextHopSet);
  EXPECT_EQ(oldLabel, 60000);

  // create a new nexthop set and verify that the free'd label is re-allocated.
  std::set<folly::IPAddress> labelNextHopSet2;
  labelNextHopSet2.emplace(folly::IPAddress("192.168.0.1"));
  labelNextHopSet2.emplace(folly::IPAddress("192.168.0.2"));
  labelNextHopSet2.emplace(folly::IPAddress("192.168.0.3"));
  auto pair = prependLabelAllocator_->incrementRefCount(labelNextHopSet2);
  EXPECT_EQ(pair.second, true);
  EXPECT_EQ(pair.first.value(), 60000);
}

} // namespace openr

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
