/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/PrefixState.h>

using namespace openr;

class PrefixStateTestFixture : public ::testing::Test {
 protected:
  static thrift::IpPrefix
  getAddrFromSeed(size_t seed, bool isV4) {
    CHECK_LE(seed, 255);
    return toIpPrefix(folly::sformat(
        "{}10.0.0.{}/{}", isV4 ? "" : "::ffff:", seed, isV4 ? 32 : 128));
  }

  PrefixState state_;
  std::unordered_map<std::string, thrift::PrefixDatabase> prefixDbs_;

  void
  SetUp() override {
    auto const numNodes = getNumNodes();
    for (size_t i = 0; i < numNodes; ++i) {
      std::string nodeName = std::to_string(i);
      prefixDbs_[nodeName] = createPrefixDbForNode(nodeName, i);
      EXPECT_TRUE(state_.updatePrefixDatabase(prefixDbs_[nodeName]));
    }
  }

  virtual size_t
  getNumNodes() const {
    return 2;
  }

  virtual thrift::PrefixDatabase
  createPrefixDbForNode(std::string const& name, size_t prefixSeed) const {
    return createPrefixDb(
        name,
        {createPrefixEntry(getAddrFromSeed(prefixSeed, false)),
         createPrefixEntry(getAddrFromSeed(prefixSeed, true))});
  }
};

TEST_F(PrefixStateTestFixture, basicOperation) {
  EXPECT_EQ(state_.getPrefixDatabases(), prefixDbs_);
  auto const dbEntry = *prefixDbs_.begin();
  EXPECT_FALSE(state_.updatePrefixDatabase(dbEntry.second));

  auto prefixDb1Updated = dbEntry.second;
  prefixDb1Updated.prefixEntries.at(0).type = thrift::PrefixType::BREEZE;
  EXPECT_TRUE(state_.updatePrefixDatabase(prefixDb1Updated));
  EXPECT_FALSE(state_.updatePrefixDatabase(prefixDb1Updated));
  EXPECT_EQ(prefixDb1Updated, state_.getPrefixDatabases().at(dbEntry.first));

  prefixDb1Updated.prefixEntries.at(0).forwardingType =
      thrift::PrefixForwardingType::SR_MPLS;
  EXPECT_TRUE(state_.updatePrefixDatabase(prefixDb1Updated));
  EXPECT_FALSE(state_.updatePrefixDatabase(prefixDb1Updated));
  EXPECT_EQ(prefixDb1Updated, state_.getPrefixDatabases().at(dbEntry.first));

  EXPECT_TRUE(state_.deletePrefixDatabase(dbEntry.first));
  auto modifiedPrefixDbs = prefixDbs_;
  modifiedPrefixDbs.erase(dbEntry.first);
  EXPECT_NE(prefixDbs_, modifiedPrefixDbs);
  EXPECT_EQ(state_.getPrefixDatabases(), modifiedPrefixDbs);
  EXPECT_FALSE(state_.deletePrefixDatabase(dbEntry.first));
  EXPECT_TRUE(state_.updatePrefixDatabase(dbEntry.second));
}

class GetLoopbackViasTest : public PrefixStateTestFixture,
                            public ::testing::WithParamInterface<bool> {};

TEST_P(GetLoopbackViasTest, basicOperation) {
  bool isV4 = GetParam();
  std::unordered_set<std::string> nodes;
  for (auto const& [name, db] : prefixDbs_) {
    nodes.emplace(name);
  }
  const auto loopbacks = state_.getLoopbackVias(nodes, isV4, folly::none);

  EXPECT_EQ(loopbacks.size(), prefixDbs_.size());

  for (auto const& node : nodes) {
    auto const& prefixDb = prefixDbs_.at(node);
    EXPECT_NE(
        loopbacks.end(),
        std::find_if(
            loopbacks.begin(),
            loopbacks.end(),
            [&prefixDb, isV4](thrift::NextHopThrift const& nh) {
              for (auto const& prefixEntry : prefixDb.prefixEntries) {
                auto byteCount = isV4 ? folly::IPAddressV4::byteCount()
                                      : folly::IPAddressV6::byteCount();
                if (prefixEntry.type == thrift::PrefixType::LOOPBACK &&
                    prefixEntry.prefix.prefixAddress.addr.size() == byteCount &&
                    nh.address.addr == prefixEntry.prefix.prefixAddress.addr) {
                  return true;
                }
              }
              return false;
            }));
  }

  // delete loopback for each node
  for (auto const& node : nodes) {
    auto const& prefixDb = prefixDbs_.at(node);
    for (auto const& prefixEntry : prefixDb.prefixEntries) {
      state_.deleteLoopbackPrefix(prefixEntry.prefix, node);
    }
  }

  const auto loopbacks2 = state_.getLoopbackVias(nodes, isV4, folly::none);
  EXPECT_EQ(loopbacks2.size(), 0);
}

INSTANTIATE_TEST_CASE_P(
    LoopbackViasInstance, GetLoopbackViasTest, ::testing::Bool());

TEST_F(PrefixStateTestFixture, getNodeHostLoopbacksV4) {
  std::pair<std::string, thrift::BinaryAddress> pair1(
      "0", getAddrFromSeed(0, true).prefixAddress);
  std::pair<std::string, thrift::BinaryAddress> pair2(
      "1", getAddrFromSeed(1, true).prefixAddress);
  EXPECT_THAT(
      state_.getNodeHostLoopbacksV4(),
      testing::UnorderedElementsAre(pair1, pair2));

  EXPECT_TRUE(state_.deletePrefixDatabase("0"));
  EXPECT_THAT(
      state_.getNodeHostLoopbacksV4(), testing::UnorderedElementsAre(pair2));
}

TEST_F(PrefixStateTestFixture, getNodeHostLoopbacksV6) {
  std::pair<std::string, thrift::BinaryAddress> pair1(
      "0", getAddrFromSeed(0, false).prefixAddress);
  std::pair<std::string, thrift::BinaryAddress> pair2(
      "1", getAddrFromSeed(1, false).prefixAddress);
  EXPECT_THAT(
      state_.getNodeHostLoopbacksV6(),
      testing::UnorderedElementsAre(pair1, pair2));

  EXPECT_TRUE(state_.deletePrefixDatabase("0"));
  EXPECT_THAT(
      state_.getNodeHostLoopbacksV6(), testing::UnorderedElementsAre(pair2));
}

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
