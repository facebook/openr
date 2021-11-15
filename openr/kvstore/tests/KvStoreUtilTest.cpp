/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/zmq/Zmq.h>
#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

class MultipleKvStoreTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    auto makeStoreWrapper = [this](std::string nodeId) {
      auto tConfig = getBasicOpenrConfig(nodeId);
      config_ = std::make_shared<Config>(tConfig);
      return std::make_shared<KvStoreWrapper>(context_, config_);
    };

    // spin up kvStore through kvStoreWrapper
    kvStoreWrapper1_ = makeStoreWrapper(nodeId1_);
    kvStoreWrapper2_ = makeStoreWrapper(nodeId2_);

    kvStoreWrapper1_->run();
    kvStoreWrapper2_->run();
  }

  void
  TearDown() override {
    // ATTN: kvStoreUpdatesQueue must be closed before destructing
    //       KvStoreClientInternal as fiber future is depending on RQueue
    kvStoreWrapper1_->stop();
    kvStoreWrapper1_.reset();
    kvStoreWrapper2_->stop();
    kvStoreWrapper2_.reset();
  }

  // var used to conmmunicate to kvStore through openrCtrl thrift server
  const std::string nodeId1_{"test_1"};
  const std::string nodeId2_{"test_2"};

  fbzmq::Context context_{};
  std::shared_ptr<Config> config_;
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper1_, kvStoreWrapper2_;
};

//
// validate mergeKeyValues
//
TEST(KvStoreUtil, mergeKeyValuesTest) {
  std::unordered_map<std::string, thrift::Value> oldStore;
  std::unordered_map<std::string, thrift::Value> myStore;
  std::unordered_map<std::string, thrift::Value> newStore;

  std::string key{"key"};

  auto thriftValue = createThriftValue(
      5, /* version */
      "node5", /* node id */
      "dummyValue",
      3600, /* ttl */
      0 /* ttl version */,
      0 /* hash */);
  oldStore.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(key),
      std::forward_as_tuple(thriftValue));
  myStore.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(key),
      std::forward_as_tuple(thriftValue));
  newStore.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(key),
      std::forward_as_tuple(thriftValue));
  auto oldKvIt = oldStore.find(key);
  auto myKvIt = myStore.find(key);
  auto newKvIt = newStore.find(key);

  // update with newer version
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    (*newKvIt->second.version_ref())++;
    auto keyVals = mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(keyVals, newStore);
  }

  // update with lower version
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    (*newKvIt->second.version_ref())--;
    auto keyVals = mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(keyVals.size(), 0);
  }

  // update with higher originatorId
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    *newKvIt->second.originatorId_ref() = "node55";
    auto keyVals = mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(keyVals, newStore);
  }

  // update with lower originatorId
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    *newKvIt->second.originatorId_ref() = "node3";
    auto keyVals = mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(keyVals.size(), 0);
  }

  // update larger value
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value_ref() = "dummyValueTest";
    auto keyVals = mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(keyVals, newStore);
  }

  // update smaller value
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value_ref() = "dummy";
    auto keyVals = mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(keyVals.size(), 0);
  }

  // update ttl only (new value.value() is none)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value_ref().reset();
    newKvIt->second.ttl_ref() = 123;
    (*newKvIt->second.ttlVersion_ref())++;
    auto keyVals = mergeKeyValues(myStore, newStore);
    auto deltaKvIt = keyVals.find(key);
    // new ttl, ttlversion
    EXPECT_EQ(
        *myKvIt->second.ttlVersion_ref(), *newKvIt->second.ttlVersion_ref());
    EXPECT_EQ(*myKvIt->second.ttl_ref(), *newKvIt->second.ttl_ref());
    // old value tho
    EXPECT_EQ(myKvIt->second.value_ref(), oldKvIt->second.value_ref());

    EXPECT_EQ(
        *deltaKvIt->second.ttlVersion_ref(), *newKvIt->second.ttlVersion_ref());
    EXPECT_EQ(*deltaKvIt->second.ttl_ref(), *newKvIt->second.ttl_ref());
    EXPECT_EQ(deltaKvIt->second.value_ref().has_value(), false);
  }

  // update ttl only (same version, originatorId and value,
  // but higher ttlVersion)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.ttl_ref() = 123;
    (*newKvIt->second.ttlVersion_ref())++;
    auto keyVals = mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(keyVals, newStore);
  }

  // invalid ttl update (higher ttlVersion, smaller value)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value_ref() = "dummy";
    (*newKvIt->second.ttlVersion_ref())++;
    auto keyVals = mergeKeyValues(myStore, newStore);
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(keyVals.size(), 0);
  }

  // bogus ttl value (see it should get ignored)
  {
    std::unordered_map<std::string, thrift::Value> emptyStore;
    newKvIt->second = thriftValue;
    newKvIt->second.ttl_ref() = -100;
    auto keyVals = mergeKeyValues(emptyStore, newStore);
    EXPECT_EQ(keyVals.size(), 0);
    EXPECT_EQ(emptyStore.size(), 0);
  }
}

//
// Test compareValues method
//
TEST(KvStoreUtil, compareValuesTest) {
  auto refValue = createThriftValue(
      5, /* version */
      "node5", /* node id */
      "dummyValue",
      3600, /* ttl */
      123 /* ttl version */,
      112233 /* hash */);
  thrift::Value v1;
  thrift::Value v2;

  // diff version
  v1 = refValue;
  v2 = refValue;
  (*v1.version_ref())++;
  {
    int rc = compareValues(v1, v2);
    EXPECT_EQ(rc, 1); // v1 is better
  }

  // diff originatorId
  v1 = refValue;
  v2 = refValue;
  *v2.originatorId_ref() = "node6";
  {
    int rc = compareValues(v1, v2);
    EXPECT_EQ(rc, -1); // v2 is better
  }

  // diff ttlVersion
  v1 = refValue;
  v2 = refValue;
  (*v1.ttlVersion_ref())++;
  {
    int rc = compareValues(v1, v2);
    EXPECT_EQ(rc, 1); // v1 is better
  }

  // same values
  v1 = refValue;
  v2 = refValue;
  {
    int rc = compareValues(v1, v2);
    EXPECT_EQ(rc, 0); // same
  }

  // hash and value are different
  v1 = refValue;
  v2 = refValue;
  v1.value_ref() = "dummyValue1";
  v1.hash_ref() = 445566;
  {
    int rc = compareValues(v1, v2);
    EXPECT_EQ(rc, 1); // v1 is better
  }

  // v2.hash is missing, values are different
  v1 = refValue;
  v2 = refValue;
  v1.value_ref() = "dummyValue1";
  v2.hash_ref().reset();
  {
    int rc = compareValues(v1, v2);
    EXPECT_EQ(rc, 1); // v1 is better
  }

  // v1.hash and v1.value are missing
  v1 = refValue;
  v2 = refValue;
  v1.value_ref().reset();
  v1.hash_ref().reset();
  {
    int rc = compareValues(v1, v2);
    EXPECT_EQ(rc, -2); // unknown
  }
}

//
// Test dumpAllWithThriftClient API
//
TEST_F(MultipleKvStoreTestFixture, dumpAllTest) {
  const std::string key1{"test_key1"};
  const std::string key2{"test_key2"};
  const std::string val1{"test_value1"};
  const std::string val2{"test_value2"};
  const uint16_t port1 = kvStoreWrapper1_->getThriftPort();
  const uint16_t port2 = kvStoreWrapper2_->getThriftPort();

  std::vector<folly::SocketAddress> sockAddrs;
  sockAddrs.emplace_back(
      folly::SocketAddress{Constants::kPlatformHost.toString(), port1});
  sockAddrs.emplace_back(
      folly::SocketAddress{Constants::kPlatformHost.toString(), port2});

  // Step1: insert (k1, v1) and (k2, v2) to different KvStore instances
  {
    thrift::Value tVal1 = createThriftValue(1, nodeId1_, val1);
    EXPECT_TRUE(kvStoreWrapper1_->setKey(kTestingAreaName, key1, tVal1));

    thrift::Value tVal2 = createThriftValue(1, nodeId2_, val2);
    EXPECT_TRUE(kvStoreWrapper2_->setKey(kTestingAreaName, key2, tVal2));
  }

  // Step2: verify fetch + aggregate 2 keys from different kvStores with prefix
  {
    const auto [db, _] =
        dumpAllWithThriftClientFromMultiple(kTestingAreaName, sockAddrs, "");
    ASSERT_TRUE(db.has_value());
    auto pub = db.value();
    EXPECT_TRUE(pub.size() == 2);
    EXPECT_TRUE(pub.count(key1));
    EXPECT_TRUE(pub.count(key2));
  }

  // Step3: shutdown thriftSevers and verify
  // dumpAllWithThriftClientFromMultiple() will get nothing.
  {
    // ATTN: kvStoreUpdatesQueue must be closed before destructing
    //       KvStoreClientInternal as fiber future is depending on RQueue
    kvStoreWrapper1_->closeQueue();
    kvStoreWrapper2_->closeQueue();
    kvStoreWrapper1_->stopThriftServer();
    kvStoreWrapper2_->stopThriftServer();

    const auto [db, _] =
        dumpAllWithThriftClientFromMultiple(kTestingAreaName, sockAddrs, "");
    ASSERT_TRUE(db.has_value());
    ASSERT_TRUE(db.value().empty());
  }
}

//
// Test KvStoreFilters APIs
//
TEST(KvStoreUtil, KvStoreFiltersTest) {
  // Nodes and keys that are in the matching list
  const std::string node1{"node1"};
  const std::string node2{"node2"};

  const std::string node1_key1{fmt::format("prefix:{}:key1", node1)};
  const auto node1_val1 = createThriftValue(
      1, /* version */
      node1, /* node id */
      "dummyValue1");

  const std::string node1_key2{fmt::format("prefix:{}:key2", node1)};
  const auto node1_val2 = createThriftValue(
      2, /* version */
      node1, /* node id */
      "dummyValue2");

  const std::string node2_key1{fmt::format("prefix:{}:key1", node2)};
  const auto node2_val1 = createThriftValue(
      3, /* version */
      node2, /* node id */
      "dummyValue1");

  // Only match the prefix from node1 and node2
  std::vector<std::string> keys = {
      fmt::format("prefix:{}:", node1), fmt::format("prefix:{}:", node2)};
  std::set<std::string> nodes = {node1, node2};

  // Node and key that are not in the matching list
  const std::string node3{"node3"};
  std::string node3_key1{fmt::format("prefix:{}:key1", node3)};
  auto node3_val1 = createThriftValue(
      1, /* version */
      node3, /* node id */
      "dummyValue1");

  // 1. Test OR logic filter - keyMatchAny()
  auto orFilter = KvStoreFilters(keys, nodes, thrift::FilterOperator::OR);
  // Match key only
  ASSERT_TRUE(orFilter.keyMatch(node1_key1, node3_val1));
  ASSERT_TRUE(orFilter.keyMatch(node2_key1, node3_val1));
  // Match node only
  ASSERT_TRUE(orFilter.keyMatch(node3_key1, node1_val1));
  ASSERT_TRUE(orFilter.keyMatch(node3_key1, node2_val1));
  // Match both
  ASSERT_TRUE(orFilter.keyMatch(node1_key1, node1_val2));
  ASSERT_TRUE(orFilter.keyMatch(node1_key2, node1_val2));
  ASSERT_TRUE(orFilter.keyMatch(node1_key2, node1_val1));
  // No match
  ASSERT_FALSE(orFilter.keyMatch(node3_key1, node3_val1));

  // 2. Test AND logic filter - keyMatchAll()
  auto andFilter = KvStoreFilters(keys, nodes, thrift::FilterOperator::AND);
  // Return true if match all attributes
  ASSERT_TRUE(andFilter.keyMatch(node1_key1, node1_val1));
  ASSERT_TRUE(andFilter.keyMatch(node1_key1, node1_val2));
  ASSERT_TRUE(andFilter.keyMatch(node1_key2, node1_val2));
  ASSERT_TRUE(andFilter.keyMatch(node1_key2, node1_val1));
  // Return false if either one doesn't match
  ASSERT_FALSE(andFilter.keyMatch(node1_key1, node3_val1)); // Match key only
  ASSERT_FALSE(andFilter.keyMatch(node3_key1, node1_val1)); // Match node only
  ASSERT_FALSE(andFilter.keyMatch(node3_key1, node3_val1)); // No match
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
