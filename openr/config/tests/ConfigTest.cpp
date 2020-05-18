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

namespace {
openr::thrift::LinkMonitorConfig
getTestLinkMonitorConfig() {
  openr::thrift::LinkMonitorConfig lmConf;
  lmConf.include_interface_regexes.emplace_back("fboss.*");
  lmConf.exclude_interface_regexes.emplace_back("eth.*");
  lmConf.redistribute_interface_regexes.emplace_back("lo");
  return lmConf;
}

openr::thrift::KvstoreFloodRate
getFloodRate() {
  openr::thrift::KvstoreFloodRate floodrate;
  floodrate.flood_msg_per_sec = 1;
  floodrate.flood_msg_burst_size = 1;
  return floodrate;
}

} // namespace

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
  {{auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
  confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref() = getFloodRate();
  confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref()->flood_msg_per_sec =
      0;
  EXPECT_THROW(auto c = Config(confInvalidFloodMsgPerSec), std::out_of_range);
}
// flood_msg_burst_size <= 0
{
  auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
  confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref() = getFloodRate();
  confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref()
      ->flood_msg_burst_size = 0;
  EXPECT_THROW(auto c = Config(confInvalidFloodMsgPerSec), std::out_of_range);
}
} // namespace openr

// link monitor
{
  // linkflap_initial_backoff_ms < 0
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config.linkflap_initial_backoff_ms = -1;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }
  // linkflap_max_backoff_ms < 0
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config.linkflap_max_backoff_ms = -1;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }
  // linkflap_initial_backoff_ms > linkflap_max_backoff_ms
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config.linkflap_initial_backoff_ms = 360000;
    confInvalidLm.link_monitor_config.linkflap_max_backoff_ms = 300000;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }

  // invalid include_interface_regexes
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config = getTestLinkMonitorConfig();
    confInvalidLm.link_monitor_config.include_interface_regexes.emplace_back(
        "[0-9]++");
    EXPECT_THROW(auto c = Config(confInvalidLm), std::invalid_argument);
  }
  //  invalid exclude_interface_regexes
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config = getTestLinkMonitorConfig();
    confInvalidLm.link_monitor_config.exclude_interface_regexes.emplace_back(
        "boom\\");
    EXPECT_THROW(auto c = Config(confInvalidLm), std::invalid_argument);
  }
  //  invalid redistribute_interface_regexes
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config = getTestLinkMonitorConfig();
    confInvalidLm.link_monitor_config.redistribute_interface_regexes
        .emplace_back("*");
    EXPECT_THROW(auto c = Config(confInvalidLm), std::invalid_argument);
  }
}
}

TEST(ConfigTest, GeneralGetter) {
  auto tConfig = getBasicOpenrConfig(
      "node-1",
      true /* enableV4 */,
      false /* enableSegmentRouting */,
      false /* orderedFibProgramming */,
      true /*dryrun*/);
  auto config = Config(tConfig);

  // getNodeName
  EXPECT_EQ("node-1", config.getNodeName());

  // getAreaIds
  auto areaIds = config.getAreaIds();
  EXPECT_EQ(1, areaIds.size());
  EXPECT_EQ(1, areaIds.count(thrift::KvStore_constants::kDefaultArea()));

  // isV4Enabled
  EXPECT_TRUE(config.isV4Enabled());
  // isSegmentRoutingEnabled
  EXPECT_FALSE(config.isSegmentRoutingEnabled());
}

TEST(ConfigTest, KvstoreGetter) {
  auto tConfig = getBasicOpenrConfig();
  auto config = Config(tConfig);
  const auto& kvstoreConf = thrift::KvstoreConfig();

  // getKvStoreConfig
  EXPECT_EQ(kvstoreConf, config.getKvStoreConfig());

  // getKvStoreKeyTtl
  EXPECT_EQ(std::chrono::milliseconds(300000), config.getKvStoreKeyTtl());
}

TEST(ConfigTest, LinkMonitorGetter) {
  auto tConfig = getBasicOpenrConfig();
  const auto& lmConf = getTestLinkMonitorConfig();
  tConfig.link_monitor_config = lmConf;
  auto config = Config(tConfig);

  // getLinkMonitorConfig
  EXPECT_EQ(lmConf, config.getLinkMonitorConfig());

  // getIncludeItfRegexes
  auto includeItfRegexes = config.getIncludeItfRegexes();
  {
    std::vector<int> matches;
    EXPECT_TRUE(includeItfRegexes->Match("fboss10", &matches));
    EXPECT_FALSE(includeItfRegexes->Match("eth0", &matches));
  }

  // getExcludeItfRegexes
  auto excludeItfRegexes = config.getExcludeItfRegexes();
  {
    std::vector<int> matches;
    EXPECT_TRUE(excludeItfRegexes->Match("eth0", &matches));
    EXPECT_FALSE(excludeItfRegexes->Match("fboss10", &matches));
  }

  // getRedistributeItfRegexes
  auto redistributeItfRegexes = config.getRedistributeItfRegexes();
  {
    std::vector<int> matches;
    EXPECT_TRUE(redistributeItfRegexes->Match("lo", &matches));
    EXPECT_FALSE(redistributeItfRegexes->Match("eth0", &matches));
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
