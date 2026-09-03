/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/coro/GtestHelpers.h>
#include <folly/init/Init.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreServiceHandler.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;

namespace {

using KvStoreServiceClient =
    apache::thrift::Client<openr::thrift::KvStoreService>;

} // namespace

class KvStoreServiceHandlerTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    thrift::KvStoreConfig kvStoreConfig;
    kvStoreConfig.node_name() = nodeName_;
    const folly::F14FastSet<std::string> areaIds{kTestingAreaName.t};

    // Spawn kvStore instance with wrapper
    kvStoreWrapper_ =
        std::make_unique<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>(
            areaIds, kvStoreConfig);
    kvStoreWrapper_->run();

    handler_ = std::make_unique<
        KvStoreServiceHandler<thrift::KvStoreServiceAsyncClient>>(
        nodeName_, kvStoreWrapper_->getKvStore());
  }

  void
  TearDown() override {
    thriftClient_.reset();
    thriftServer_.reset();

    // release handler_ first with kvStoreWrapper_'s ref count
    handler_.reset();
    kvStoreWrapper_->stop();
  }

  KvStoreServiceClient&
  getKvStoreServiceClient() {
    if (!thriftServer_) {
      thriftServer_ =
          std::make_unique<apache::thrift::ScopedServerInterfaceThread>(
              handler_, "::1", 0);
      thriftClient_ = thriftServer_->newClient<KvStoreServiceClient>();
    }
    return *thriftClient_;
  }

 protected:
  const std::string nodeName_{"node"};
  std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>
      kvStoreWrapper_;
  std::shared_ptr<KvStoreServiceHandler<thrift::KvStoreServiceAsyncClient>>
      handler_;
  std::unique_ptr<apache::thrift::ScopedServerInterfaceThread> thriftServer_;
  std::unique_ptr<KvStoreServiceClient> thriftClient_;
};

CO_TEST_F(KvStoreServiceHandlerTestFixture, GetKvStoreHashFilteredArea) {
  const std::string key{"key1"};
  const auto value = createThriftValue(1, nodeName_, std::string("value1"));
  CO_ASSERT_TRUE(kvStoreWrapper_->setKey(kTestingAreaName, key, value));

  thrift::KeyDumpParams params;
  params.keys() = {key};
  auto publication =
      co_await getKvStoreServiceClient().co_getKvStoreHashFilteredArea(
          params, kTestingAreaName);

  CO_ASSERT_EQ(1, publication.keyVals()->size());
  EXPECT_FALSE(publication.keyVals()->at(key).value().has_value());
}

CO_TEST_F(KvStoreServiceHandlerTestFixture, SetKvStoreKeyValues) {
  const std::string key{"key1"};
  const auto value = createThriftValue(1, nodeName_, std::string("value1"));
  thrift::KeySetParams params;
  params.keyVals()->emplace(key, value);

  auto result = co_await getKvStoreServiceClient().co_setKvStoreKeyValues(
      params, kTestingAreaName);

  CO_ASSERT_TRUE(result.noMergeReasons()->empty());
  const auto keyVals = kvStoreWrapper_->dumpAll(kTestingAreaName);
  CO_ASSERT_EQ(1, keyVals.count(key));
  EXPECT_EQ(value, keyVals.at(key));
}

CO_TEST_F(KvStoreServiceHandlerTestFixture, GetKvStoreKeyValsArea) {
  const std::string key{"key1"};
  const auto value = createThriftValue(1, nodeName_, std::string("value1"));
  CO_ASSERT_TRUE(kvStoreWrapper_->setKey(kTestingAreaName, key, value));

  auto publication =
      co_await getKvStoreServiceClient().co_getKvStoreKeyValsArea(
          std::vector<std::string>{key}, kTestingAreaName);
  CO_ASSERT_EQ(1, publication.keyVals()->size());
  EXPECT_EQ(value, publication.keyVals()->at(key));

  publication = co_await getKvStoreServiceClient().co_getKvStoreKeyValsArea(
      std::vector<std::string>{"missing-key"}, kTestingAreaName);
  EXPECT_TRUE(publication.keyVals()->empty());
}

CO_TEST_F(KvStoreServiceHandlerTestFixture, SetKvStoreKeyVals) {
  const std::string key{"key1"};
  const auto value = createThriftValue(1, nodeName_, std::string("value1"));
  thrift::KeySetParams params;
  params.keyVals()->emplace(key, value);

  co_await getKvStoreServiceClient().co_setKvStoreKeyVals(
      params, kTestingAreaName);

  const auto keyVals = kvStoreWrapper_->dumpAll(kTestingAreaName);
  CO_ASSERT_EQ(1, keyVals.count(key));
  EXPECT_EQ(value, keyVals.at(key));

  const std::string missingArea{"missing-area"};
  CO_ASSERT_THROW(
      co_await getKvStoreServiceClient().co_setKvStoreKeyVals(
          params, missingArea),
      thrift::KvStoreError);
}

CO_TEST_F(KvStoreServiceHandlerTestFixture, GetKvStorePeersArea) {
  auto& client = getKvStoreServiceClient();
  auto peers = co_await client.co_getKvStorePeersArea(kTestingAreaName);

  EXPECT_TRUE(peers.empty());

  const std::string missingArea{"missing-area"};
  CO_ASSERT_THROW(
      co_await client.co_getKvStorePeersArea(missingArea),
      thrift::KvStoreError);
}

TEST_F(KvStoreServiceHandlerTestFixture, GetNodeName) {
  EXPECT_EQ(nodeName_, handler_->getNodeName());
}

CO_TEST_F(KvStoreServiceHandlerTestFixture, KvStoreApis) {
  thrift::KeyVals kvs(
      {{"key1", createThriftValue(1, nodeName_, std::string("value1"))},
       {"key2", createThriftValue(1, nodeName_, std::string("value2"))},
       {"key3", createThriftValue(1, nodeName_, std::string("value3"))}});

  for (const auto& [key, value] : kvs) {
    CO_ASSERT_TRUE(kvStoreWrapper_->setKey(kTestingAreaName, key, value));
  }

  {
    // get API with regex matching
    //
    // positive test case
    thrift::KeyDumpParams params;
    params.keys() = {"key"};
    params.originatorIds() = {"fake_node"};
    params.oper() = thrift::FilterOperator::OR;

    auto pub =
        co_await getKvStoreServiceClient().co_getKvStoreKeyValsFilteredArea(
            params, kTestingAreaName);
    auto keyVals = *pub.keyVals();
    EXPECT_EQ(3, keyVals.size());
    EXPECT_EQ(keyVals.at("key1"), kvs.at("key1"));
    EXPECT_EQ(keyVals.at("key2"), kvs.at("key2"));
    EXPECT_EQ(keyVals.at("key3"), kvs.at("key3"));
  }
  {
    // get API with regex matching
    //
    // negative test case
    thrift::KeyDumpParams params;
    params.keys() = {"key"};
    params.originatorIds() = {"fake_node"};
    params.oper() = thrift::FilterOperator::AND;

    auto pub =
        co_await getKvStoreServiceClient().co_getKvStoreKeyValsFilteredArea(
            params, kTestingAreaName);
    auto keyVals = *pub.keyVals();
    EXPECT_EQ(0, keyVals.size());
  }
}
