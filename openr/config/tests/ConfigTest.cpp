/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>
#include <utility>

#include <folly/FileUtil.h>
#include <folly/experimental/TestUtil.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/config/Config.h>
#include <openr/config/GflagConfig.h>
#include <openr/config/tests/Utils.h>

#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {
using apache::thrift::FragileConstructor::FRAGILE;

class ConfigTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    validConfig_ = getBasicOpenrConfig();

    auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
    try {
      jsonSerializer.serialize(validConfig_, &validConfigStr_);
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Could not serialize config: " << folly::exceptionStr(ex);
    }

    folly::writeFileAtomic(validConfigFile_.path().string(), validConfigStr_);
  }

 protected:
  thrift::OpenrConfig validConfig_;
  std::string validConfigStr_;
  folly::test::TemporaryFile validConfigFile_;
};

TEST_F(ConfigTestFixture, ConstructFromFile) {
  EXPECT_NO_THROW(Config(validConfigFile_.path().string()));

  // thrift format error
  {
    folly::dynamic invalidConfig;
    invalidConfig = folly::parseJson(validConfigStr_);
    invalidConfig["areas"] = "This should be a vector";
    LOG(INFO) << invalidConfig;

    folly::test::TemporaryFile invalidConfigFile;
    folly::writeFileAtomic(
        invalidConfigFile.path().string(), folly::toJson(invalidConfig));
    EXPECT_ANY_THROW(Config(invalidConfigFile.path().string()));
  }
}

TEST(ConfigTest, PopulateInternalDb) {
  // area
  {
    thrift::AreaConfig area1;
    area1.area_id = "1";
    area1.interface_regexes.emplace_back("fboss.*");
    area1.neighbor_regexes.emplace_back("rsw.*");

    // duplicate area id
    auto confInvalidArea = getBasicOpenrConfig();
    confInvalidArea.areas.emplace_back(area1);
    confInvalidArea.areas.emplace_back(area1);
    EXPECT_THROW(auto c = Config(confInvalidArea), std::invalid_argument);
  }

  // kvstore
  {
    thrift::KvstoreFloodRate floodrate;
    floodrate.flood_msg_per_sec = 1;
    floodrate.flood_msg_burst_size = 1;
    // invalid flood_msg_per_sec
    {
      auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
      floodrate.flood_msg_per_sec = 0;
      confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref() = floodrate;
      EXPECT_THROW(
          auto c = Config(confInvalidFloodMsgPerSec), std::out_of_range);
    }
    // invalid flood_msg_burst_size
    {
      auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
      floodrate.flood_msg_burst_size = 0;
      confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref() = floodrate;
      EXPECT_THROW(
          auto c = Config(confInvalidFloodMsgPerSec), std::out_of_range);
    }
  }
}

TEST(ConfigTest, getBgpAutoConfig) {
  {
    FLAGS_node_name = "fsw001";
    auto bgpConfig = GflagConfig::getBgpAutoConfig();
    EXPECT_EQ(2201, *bgpConfig.local_confed_as_ref());
  }
  {
    FLAGS_node_name = "rsw001";
    auto bgpConfig = GflagConfig::getBgpAutoConfig();
    EXPECT_EQ(2101, *bgpConfig.local_confed_as_ref());
  }
}

} // namespace openr
