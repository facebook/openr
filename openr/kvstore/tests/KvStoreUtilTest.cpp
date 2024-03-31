/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <gtest/gtest.h>

#include <openr/common/OpenrClient.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;

class MultipleKvStoreTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    auto makeStoreWrapper = [](std::string nodeId) {
      // create KvStoreConfig
      thrift::KvStoreConfig kvStoreConfig;
      kvStoreConfig.node_name() = nodeId;
      const std::unordered_set<std::string> areaIds{kTestingAreaName};

      return std::make_shared<
          KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
          areaIds, kvStoreConfig);
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
    kvStoreWrapper2_->stop();
  }

  // var used to conmmunicate to kvStore through openrCtrl thrift server
  const std::string nodeId1_{"test_1"};
  const std::string nodeId2_{"test_2"};

  std::shared_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>
      kvStoreWrapper1_, kvStoreWrapper2_;
};

//
// validate mergeKeyValues
//
TEST(KvStoreUtil, mergeKeyValuesTest) {
  thrift::KeyVals oldStore;
  thrift::KeyVals myStore;
  thrift::KeyVals newStore;

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
    (*newKvIt->second.version())++;
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(keyVals, newStore);
  }

  // update with lower version
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    (*newKvIt->second.version())--;
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(keyVals.size(), 0);
  }

  // update with higher originatorId
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    *newKvIt->second.originatorId() = "node55";
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(keyVals, newStore);
  }

  // update with lower originatorId
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    *newKvIt->second.originatorId() = "node3";
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(keyVals.size(), 0);
  }

  // update larger value
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value() = "dummyValueTest";
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(keyVals, newStore);
  }

  // update smaller value
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value() = "dummy";
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(keyVals.size(), 0);
  }

  // update ttl only (new value.value() is none)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value().reset();
    newKvIt->second.ttl() = 123;
    (*newKvIt->second.ttlVersion())++;
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    auto deltaKvIt = keyVals.find(key);
    // new ttl, ttlversion
    EXPECT_EQ(*myKvIt->second.ttlVersion(), *newKvIt->second.ttlVersion());
    EXPECT_EQ(*myKvIt->second.ttl(), *newKvIt->second.ttl());
    // old value tho
    EXPECT_EQ(myKvIt->second.value(), oldKvIt->second.value());

    EXPECT_EQ(*deltaKvIt->second.ttlVersion(), *newKvIt->second.ttlVersion());
    EXPECT_EQ(*deltaKvIt->second.ttl(), *newKvIt->second.ttl());
    EXPECT_EQ(deltaKvIt->second.value().has_value(), false);
  }

  // update ttl only (same version, originatorId and value,
  // but higher ttlVersion)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.ttl() = 123;
    (*newKvIt->second.ttlVersion())++;
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(myStore, newStore);
    EXPECT_EQ(keyVals, newStore);
  }

  // invalid ttl update (higher ttlVersion, smaller value)
  {
    myKvIt->second = thriftValue;
    newKvIt->second = thriftValue;
    newKvIt->second.value() = "dummy";
    (*newKvIt->second.ttlVersion())++;
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(myStore, oldStore);
    EXPECT_EQ(keyVals.size(), 0);
  }

  // bogus ttl value (see it should get ignored)
  {
    thrift::KeyVals emptyStore;
    newKvIt->second = thriftValue;
    newKvIt->second.ttl() = -100;
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
    EXPECT_EQ(keyVals.size(), 0);
    EXPECT_EQ(emptyStore.size(), 0);
  }

  // update with version=0 (see it should get ignored)
  {
    thrift::KeyVals emptyStore;
    newKvIt->second = thriftValue;
    newKvIt->second.version() = 0;
    auto keyVals = *mergeKeyValues(myStore, newStore).keyVals();
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
  (*v1.version())++;
  {
    ComparisonResult rc = compareValues(v1, v2);
    EXPECT_EQ(rc, ComparisonResult::FIRST); // v1 is better
  }

  // diff originatorId
  v1 = refValue;
  v2 = refValue;
  *v2.originatorId() = "node6";
  {
    ComparisonResult rc = compareValues(v1, v2);
    EXPECT_EQ(rc, ComparisonResult::SECOND); // v2 is better
  }

  // diff ttlVersion
  v1 = refValue;
  v2 = refValue;
  (*v1.ttlVersion())++;
  {
    ComparisonResult rc = compareValues(v1, v2);
    EXPECT_EQ(rc, ComparisonResult::FIRST); // v1 is better
  }

  // same values
  v1 = refValue;
  v2 = refValue;
  {
    ComparisonResult rc = compareValues(v1, v2);
    EXPECT_EQ(rc, ComparisonResult::TIED); // same
  }

  // hash and value are different
  v1 = refValue;
  v2 = refValue;
  v1.value() = "dummyValue1";
  v1.hash() = 445566;
  {
    ComparisonResult rc = compareValues(v1, v2);
    EXPECT_EQ(rc, ComparisonResult::FIRST); // v1 is better
  }

  // v2.hash is missing, values are different
  v1 = refValue;
  v2 = refValue;
  v1.value() = "dummyValue1";
  v2.hash().reset();
  {
    ComparisonResult rc = compareValues(v1, v2);
    EXPECT_EQ(rc, ComparisonResult::FIRST); // v1 is better
  }

  // v1.hash and v1.value are missing
  v1 = refValue;
  v2 = refValue;
  v1.value().reset();
  v1.hash().reset();
  {
    ComparisonResult rc = compareValues(v1, v2);
    EXPECT_EQ(rc, ComparisonResult::UNKNOWN); // unknown
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
  sockAddrs.emplace_back(Constants::kPlatformHost.toString(), port1);
  sockAddrs.emplace_back(Constants::kPlatformHost.toString(), port2);

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
        dumpAllWithThriftClientFromMultiple<thrift::KvStoreServiceAsyncClient>(
            kTestingAreaName, sockAddrs, "");
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
        dumpAllWithThriftClientFromMultiple<thrift::KvStoreServiceAsyncClient>(
            kTestingAreaName, sockAddrs, "");
    ASSERT_TRUE(db.has_value());
    ASSERT_TRUE(db.value().empty());
  }
}

//
// Test dumpAllWithThriftClient API overloaded for clients instead of addresses
//
TEST_F(MultipleKvStoreTestFixture, dumpAllWithClientsTest) {
  const std::string key1{"test_key1"};
  const std::string key2{"test_key2"};
  const std::string val1{"test_value1"};
  const std::string val2{"test_value2"};
  const uint16_t port1 = kvStoreWrapper1_->getThriftPort();
  const uint16_t port2 = kvStoreWrapper2_->getThriftPort();

  folly::EventBase* evb = folly::EventBaseManager::get()->getEventBase();
  std::vector<std::unique_ptr<thrift::OpenrCtrlCppAsyncClient>> clients;
  clients.emplace_back(
      getOpenrCtrlPlainTextClient<openr::thrift::OpenrCtrlCppAsyncClient>(
          *evb, folly::IPAddress(Constants::kPlatformHost.toString()), port1));
  clients.emplace_back(
      getOpenrCtrlPlainTextClient<openr::thrift::OpenrCtrlCppAsyncClient>(
          *evb, folly::IPAddress(Constants::kPlatformHost.toString()), port2));

  // Step1: insert (k1, v1) and (k2, v2) to different KvStore instances
  {
    thrift::Value tVal1 = createThriftValue(1, nodeId1_, val1);
    EXPECT_TRUE(kvStoreWrapper1_->setKey(kTestingAreaName, key1, tVal1));

    thrift::Value tVal2 = createThriftValue(1, nodeId2_, val2);
    EXPECT_TRUE(kvStoreWrapper2_->setKey(kTestingAreaName, key2, tVal2));
  }

  // Step2: verify fetch + aggregate 2 keys from different kvStores with prefix
  {
    const auto& pub = dumpAllWithThriftClientFromMultiple(
        *evb, kTestingAreaName, clients, "");
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

    const auto& pub = dumpAllWithThriftClientFromMultiple(
        *evb, kTestingAreaName, clients, "");
    ASSERT_TRUE(pub.empty());
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

TEST(KvStoreUtil, IsValidTtlTest) {
  EXPECT_TRUE(isValidTtl(1));
  EXPECT_TRUE(isValidTtl(Constants::kTtlInfinity));
  EXPECT_TRUE(isValidTtl(100));
  EXPECT_FALSE(isValidTtl(0));
  EXPECT_FALSE(isValidTtl(-100));
}

TEST(KvStoreUtil, IsValidVersionTest) {
  thrift::Value incomingVal;
  int64_t existingVersion;
  {
    incomingVal.version() = 5;
    existingVersion = 5;
    EXPECT_TRUE(isValidVersion(existingVersion, incomingVal));
  }
  {
    incomingVal.version() = 0;
    existingVersion = 0;
    EXPECT_FALSE(isValidVersion(existingVersion, incomingVal));
  }
  {
    incomingVal.version() = 4;
    existingVersion = 1;
    EXPECT_TRUE(isValidVersion(existingVersion, incomingVal));
  }
  {
    incomingVal.version() = 1;
    existingVersion = 4;
    EXPECT_FALSE(isValidVersion(existingVersion, incomingVal));
  }
}

TEST(KvStoreUtil, GetMergeTypeTest) {
  // Resync is needed
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    thrift::KeyVals kvStore;
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderA";
    EXPECT_EQ(
        MergeType::RESYNC_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // ttl only and ttl version is greater
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    value.ttlVersion() = 1;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.ttlVersion() = 4;
    EXPECT_EQ(
        MergeType::UPDATE_TTL_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // version is lower -> no update needed
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    value.value() = "value";
    value.version() = 4;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.version() = 1;
    EXPECT_EQ(
        MergeType::NO_UPDATE_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // version is valid for update
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    value.value() = "value";
    value.version() = 1;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.version() = 4;
    EXPECT_EQ(
        MergeType::UPDATE_ALL_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // 2nd tie breaking
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    value.value() = "value";
    value.version() = 1;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.version() = 1;
    value.originatorId() = "senderB";
    EXPECT_EQ(
        MergeType::UPDATE_ALL_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // 2nd tie breaking
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderB";
    value.value() = "value";
    value.version() = 1;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.version() = 1;
    value.originatorId() = "senderA";
    EXPECT_EQ(
        MergeType::NO_UPDATE_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // 3rd tie breaking
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    value.value() = "value";
    value.version() = 1;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.version() = 1;
    value.originatorId() = "senderA";
    value.value() = "valueA";
    EXPECT_EQ(
        MergeType::UPDATE_ALL_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // 3rd tie breaking
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    value.value() = "value";
    value.version() = 1;
    value.ttlVersion() = 1;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.version() = 1;
    value.originatorId() = "senderA";
    value.ttlVersion() = 2;
    EXPECT_EQ(
        MergeType::UPDATE_TTL_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // 3rd tie breaking
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    value.value() = "value";
    value.version() = 1;
    value.ttlVersion() = 1;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.version() = 1;
    value.originatorId() = "senderA";
    value.ttlVersion() = 1;
    EXPECT_EQ(
        MergeType::NO_UPDATE_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
  // 3rd tie breaking
  {
    std::string key = "myKey";
    thrift::Value value;
    value.originatorId() = "senderA";
    value.value() = "valueA";
    value.version() = 1;
    thrift::KeyVals kvStore{{key, value}};
    thrift::KvStoreMergeResult stats;
    std::optional<std::string> sender = "senderB";
    value.version() = 1;
    value.originatorId() = "senderA";
    value.value() = "value";
    EXPECT_EQ(
        MergeType::NO_UPDATE_NEEDED,
        getMergeType(key, value, kvStore, sender, stats));
  }
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
