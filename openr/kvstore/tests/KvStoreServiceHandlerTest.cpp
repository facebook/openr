/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <gtest/gtest.h>

#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreServiceHandler.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;

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
    // release handler_ first with kvStoreWrapper_'s ref count
    handler_.reset();
    kvStoreWrapper_->stop();
  }

 protected:
  const std::string nodeName_{"node"};
  std::unique_ptr<KvStoreWrapper<thrift::KvStoreServiceAsyncClient>>
      kvStoreWrapper_;
  std::shared_ptr<KvStoreServiceHandler<thrift::KvStoreServiceAsyncClient>>
      handler_;
};

TEST_F(KvStoreServiceHandlerTestFixture, GetNodeName) {
  const auto& name = handler_->getNodeName();
  EXPECT_EQ(name, nodeName_);
}

TEST_F(KvStoreServiceHandlerTestFixture, KvStoreApis) {
  thrift::KeyVals kvs(
      {{"key1", createThriftValue(1, nodeName_, std::string("value1"))},
       {"key2", createThriftValue(1, nodeName_, std::string("value2"))},
       {"key3", createThriftValue(1, nodeName_, std::string("value3"))}});

  {
    // set API
    thrift::KeySetParams params;
    params.keyVals() = kvs;
    handler_
        ->semifuture_setKvStoreKeyVals(
            std::make_unique<thrift::KeySetParams>(std::move(params)),
            std::make_unique<std::string>(kTestingAreaName))
        .get();
  }
  {
    // get API without regex matching
    //
    // positive test case
    std::vector<std::string> filterKeys{"key1"};
    auto pub = handler_
                   ->semifuture_getKvStoreKeyValsArea(
                       std::make_unique<std::vector<std::string>>(
                           std::move(filterKeys)),
                       std::make_unique<std::string>(kTestingAreaName))
                   .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(1, keyVals.size());
    EXPECT_EQ(keyVals.at("key1"), kvs.at("key1"));
  }
  {
    // get API without regex matching
    //
    // negative test case
    std::vector<std::string> filterKeys{"key"};
    auto pub = handler_
                   ->semifuture_getKvStoreKeyValsArea(
                       std::make_unique<std::vector<std::string>>(
                           std::move(filterKeys)),
                       std::make_unique<std::string>(kTestingAreaName))
                   .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(0, keyVals.size());
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
        handler_
            ->semifuture_getKvStoreKeyValsFilteredArea(
                std::make_unique<thrift::KeyDumpParams>(std::move(params)),
                std::make_unique<std::string>(kTestingAreaName))
            .get();
    auto keyVals = *pub->keyVals();
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

    auto pub = handler_
                   ->semifuture_getKvStoreKeyValsFilteredArea(
                       std::make_unique<thrift::KeyDumpParams>(params),
                       std::make_unique<std::string>(kTestingAreaName))
                   .get();
    auto keyVals = *pub->keyVals();
    EXPECT_EQ(0, keyVals.size());
  }
}
