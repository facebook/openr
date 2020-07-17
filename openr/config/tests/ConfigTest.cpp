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
const auto& testSeedPrefix =
    folly::IPAddress::createNetwork("fc00:cafe:babe::/64");
const uint8_t testAllocationPfxLen = 128;

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

openr::thrift::PrefixAllocationConfig
getPrefixAllocationConfig(openr::thrift::PrefixAllocationMode mode) {
  openr::thrift::PrefixAllocationConfig pfxAllocationConf;
  pfxAllocationConf.prefix_allocation_mode = mode;
  if (mode == openr::thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE) {
    pfxAllocationConf.seed_prefix_ref() =
        folly::IPAddress::networkToString(testSeedPrefix);
    pfxAllocationConf.allocate_prefix_len_ref() = testAllocationPfxLen;
  }
  return pfxAllocationConf;
}

openr::thrift::AreaConfig
getAreaConfig(const std::string& areaId) {
  openr::thrift::AreaConfig area;
  area.area_id = areaId;
  area.interface_regexes.emplace_back("fboss.*");
  area.neighbor_regexes.emplace_back("rsw.*");
  return area;
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

  // out of range enum: prefix_allocation_mode
  {
    auto validTConf = getBasicOpenrConfig();
    validTConf.enable_prefix_allocation_ref() = true;
    validTConf.prefix_allocation_config_ref() =
        thrift::PrefixAllocationConfig();

    std::string validConfStr;
    EXPECT_NO_THROW(apache::thrift::SimpleJSONSerializer().serialize(
        validTConf, &validConfStr));

    folly::dynamic invalidConf = folly::parseJson(validConfStr);
    // prefix_allocation_mode range [0-2]
    invalidConf["prefix_allocation_config"]["prefix_allocation_mode"] = 3;

    folly::test::TemporaryFile invalidConfFile;
    folly::writeFileAtomic(
        invalidConfFile.path().string(), folly::toJson(invalidConf));
    EXPECT_ANY_THROW(Config(invalidConfFile.path().string()));
  }
  // out of range enum: prefix_forwarding_type
  {
    folly::dynamic invalidConfig;
    invalidConfig = folly::parseJson(validConfigStr_);
    invalidConfig["prefix_forwarding_type"] = 3;
    LOG(INFO) << invalidConfig;

    folly::test::TemporaryFile invalidConfigFile;
    folly::writeFileAtomic(
        invalidConfigFile.path().string(), folly::toJson(invalidConfig));
    EXPECT_ANY_THROW(Config(invalidConfigFile.path().string()));
  }
}

TEST(ConfigTest, PopulateAreaConfig) {
  // area

  // duplicate area id
  {
    auto confInvalidArea = getBasicOpenrConfig();
    confInvalidArea.areas.emplace_back(getAreaConfig("1"));
    confInvalidArea.areas.emplace_back(getAreaConfig("1"));
    EXPECT_THROW((Config(confInvalidArea)), std::invalid_argument);
  }

  // area config - empty neighbor and interfsace regexes
  {
    openr::thrift::AreaConfig area;
    area.area_id = thrift::KvStore_constants::kDefaultArea();
    std::vector<openr::thrift::AreaConfig> vec = {area};
    auto confInvalidArea = getBasicOpenrConfig(
        "node-1",
        "domain",
        std::make_unique<std::vector<openr::thrift::AreaConfig>>(vec));
    EXPECT_THROW((Config(confInvalidArea)), std::invalid_argument);
  }

  // non-empty interface regex
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id = thrift::KvStore_constants::kDefaultArea();
    areaConfig.interface_regexes.emplace_back("iface.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig(
        "node-1",
        "domain",
        std::make_unique<std::vector<openr::thrift::AreaConfig>>(vec));
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  // non-empty neighbor regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id = thrift::KvStore_constants::kDefaultArea();
    areaConfig.neighbor_regexes.emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig(
        "node-1",
        "domain",
        std::make_unique<std::vector<openr::thrift::AreaConfig>>(vec));
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  // non-empty neighbor and interface regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id = thrift::KvStore_constants::kDefaultArea();
    areaConfig.interface_regexes.emplace_back("iface.*");
    areaConfig.neighbor_regexes.emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig(
        "node-1",
        "domain",
        std::make_unique<std::vector<openr::thrift::AreaConfig>>(vec));
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id = thrift::KvStore_constants::kDefaultArea();
    areaConfig.interface_regexes.emplace_back("iface.*");
    areaConfig.neighbor_regexes.emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig(
        "node-1",
        "domain",
        std::make_unique<std::vector<openr::thrift::AreaConfig>>(vec));
    Config cfg = Config(confValidArea);
    EXPECT_EQ(cfg.getAreaConfiguration().size(), 1);
    EXPECT_EQ(
        cfg.getAreaConfiguration().count(
            thrift::KvStore_constants::kDefaultArea()),
        1);
    EXPECT_EQ(cfg.getAreaConfiguration().count("1"), 0);
  }
}

TEST(ConfigTest, PopulateInternalDb) {
  // features

  // enable_ordered_fib_programming = true with multiple areas
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.areas.emplace_back(getAreaConfig("1"));
    confInvalid.areas.emplace_back(getAreaConfig("2"));
    confInvalid.enable_ordered_fib_programming_ref() = true;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // KSP2_ED_ECMP with IP
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.prefix_forwarding_type = thrift::PrefixForwardingType::IP;
    confInvalid.prefix_forwarding_algorithm =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // RibPolicy
  {
    auto conf = getBasicOpenrConfig();
    conf.enable_rib_policy = true;
    EXPECT_TRUE(Config(conf).isRibPolicyEnabled());
  }

  // kvstore

  // flood_msg_per_sec <= 0
  {
    auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
    confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref() = getFloodRate();
    confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref()
        ->flood_msg_per_sec = 0;
    EXPECT_THROW((Config(confInvalidFloodMsgPerSec)), std::out_of_range);
  }
  // flood_msg_burst_size <= 0
  {
    auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
    confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref() = getFloodRate();
    confInvalidFloodMsgPerSec.kvstore_config.flood_rate_ref()
        ->flood_msg_burst_size = 0;
    EXPECT_THROW((Config(confInvalidFloodMsgPerSec)), std::out_of_range);
  }

  // Spark

  // Exception: neighbor_discovery_port <= 0 or > 65535
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config.neighbor_discovery_port = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);

    confInvalidSpark.spark_config.neighbor_discovery_port = 65536;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: hello_time_s <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config.hello_time_s = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: fastinit_hello_time_ms <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config.fastinit_hello_time_ms = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: fastinit_hello_time_ms > hello_time_s
  {
    auto confInvalidSpark2 = getBasicOpenrConfig();
    confInvalidSpark2.spark_config.fastinit_hello_time_ms = 10000;
    confInvalidSpark2.spark_config.hello_time_s = 2;
    EXPECT_THROW(auto c = Config(confInvalidSpark2), std::invalid_argument);
  }

  // Exception: keepalive_time_s <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config.keepalive_time_s = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: keepalive_time_s > hold_time_s
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config.keepalive_time_s = 10;
    confInvalidSpark.spark_config.hold_time_s = 5;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception: graceful_restart_time_s < 3 * keepalive_time_s
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config.keepalive_time_s = 10;
    confInvalidSpark.spark_config.graceful_restart_time_s = 20;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception step_detector_fast_window_size >= 0
  //           step_detector_slow_window_size >= 0
  //           step_detector_lower_threshold >= 0
  //           step_detector_upper_threshold >= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config.step_detector_conf.fast_window_size = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config.step_detector_conf.slow_window_size = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config.step_detector_conf.lower_threshold = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config.step_detector_conf.upper_threshold = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception step_detector_fast_window_size > step_detector_slow_window_size
  //           step_detector_lower_threshold > step_detector_upper_threshold
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config.step_detector_conf.fast_window_size = 10;
    confInvalidSpark.spark_config.step_detector_conf.slow_window_size = 5;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);

    confInvalidSpark.spark_config.step_detector_conf.upper_threshold = 5;
    confInvalidSpark.spark_config.step_detector_conf.lower_threshold = 10;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Monitor

  // Exception monitor_max_event_log >= 0
  {
    auto confInvalidMon = getBasicOpenrConfig();
    confInvalidMon.monitor_config.max_event_log = -1;
    EXPECT_THROW(auto c = Config(confInvalidMon), std::out_of_range);
  }

  // link monitor

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

  // prefix allocation

  // enable_prefix_allocation = true, prefix_allocation_config = null
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }
  // enable_prefix_allocation = true with multiple areas
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.areas.emplace_back(getAreaConfig("1"));
    confInvalidPa.areas.emplace_back(getAreaConfig("2"));
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }
  // prefix_allocation_mode != DYNAMIC_ROOT_NODE, seed_prefix and
  // allocate_prefix_len set
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->prefix_allocation_mode =
        thrift::PrefixAllocationMode::DYNAMIC_LEAF_NODE;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }
  // prefix_allocation_mode = DYNAMIC_ROOT_NODE, seed_prefix and
  // allocate_prefix_len = null
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() =
        thrift::PrefixAllocationConfig();
    confInvalidPa.prefix_allocation_config_ref()->prefix_allocation_mode =
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }
  // seed_prefix: invalid ipadrres format
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->seed_prefix_ref() =
        "fc00:cafe:babe:/64";
    EXPECT_ANY_THROW((Config(confInvalidPa)));
  }
  // allocate_prefix_len: <= seed_prefix subnet length
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->allocate_prefix_len_ref() =
        60;
    EXPECT_THROW((Config(confInvalidPa)), std::out_of_range);
  }
  // seed_prefix v4, enable_v4 = false
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_v4_ref() = false;

    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->seed_prefix_ref() =
        "127.0.0.1/24";
    confInvalidPa.prefix_allocation_config_ref()->allocate_prefix_len_ref() =
        32;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }

  // bgp peering

  // bgp peering enabled with empty bgp_config
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.enable_bgp_peering_ref() = true;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // watchdog

  // watchdog enabled with empty watchdog_config
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.enable_watchdog_ref() = true;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }
}

TEST(ConfigTest, GeneralGetter) {
  // config without bgp peering
  {
    auto tConfig = getBasicOpenrConfig(
        "node-1",
        "domain",
        std::nullopt, /* area config */
        true /* enableV4 */,
        false /* enableSegmentRouting */,
        false /* orderedFibProgramming */,
        true /*dryrun*/);
    auto config = Config(tConfig);

    // getNodeName
    EXPECT_EQ("node-1", config.getNodeName());

    // getDomainName
    EXPECT_EQ("domain", config.getDomainName());

    // getAreaIds
    auto areaC = config.getAreaConfiguration();
    EXPECT_EQ(1, areaC.size());
    EXPECT_EQ(1, areaC.count(thrift::KvStore_constants::kDefaultArea()));

    // enable_v4
    EXPECT_TRUE(config.isV4Enabled());
    // enable_segment_routing
    EXPECT_FALSE(config.isSegmentRoutingEnabled());
    // isBgpPeeringEnabled
    EXPECT_FALSE(config.isBgpPeeringEnabled());
    // enable_flood_optimization
    EXPECT_FALSE(config.isFloodOptimizationEnabled());

    // getSparkConfig
    EXPECT_EQ(tConfig.spark_config, config.getSparkConfig());
  }

  // config with bgp peering
  {
    auto tConfig = getBasicOpenrConfig("fsw001");
    tConfig.enable_bgp_peering_ref() = true;

    FLAGS_node_name = "fsw001";
    const auto& bgpConf = GflagConfig::getBgpAutoConfig();
    tConfig.bgp_config_ref() = bgpConf;

    auto config = Config(tConfig);

    // isBgpPeeringEnabled
    EXPECT_TRUE(config.isBgpPeeringEnabled());
    EXPECT_EQ(bgpConf, config.getBgpConfig());
    EXPECT_EQ(
        0, bgpConf.translation_config_ref()->communities_to_name_ref()->size());
    EXPECT_EQ(0, bgpConf.translation_config_ref()->asn_to_area_ref()->size());
  }

  // config with watchdog
  {
    auto tConfig = getBasicOpenrConfig("fsw001");
    tConfig.enable_watchdog_ref() = true;
    const auto& watchdogConf = thrift::WatchdogConfig();
    tConfig.watchdog_config_ref() = watchdogConf;

    auto config = Config(tConfig);

    EXPECT_TRUE(config.isWatchdogEnabled());
    EXPECT_EQ(watchdogConf, config.getWatchdogConfig());
  }
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

TEST(ConfigTest, PrefixAllocatorGetter) {
  auto tConfig = getBasicOpenrConfig();
  tConfig.enable_prefix_allocation_ref() = true;
  const auto paConf = getPrefixAllocationConfig(
      thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
  tConfig.prefix_allocation_config_ref() = paConf;
  auto config = Config(tConfig);

  // isPrefixAllocationEnabled
  EXPECT_TRUE(config.isPrefixAllocationEnabled());

  // getPrefixAllocationConfig
  EXPECT_EQ(paConf, config.getPrefixAllocationConfig());

  // getPrefixAllocationParams
  const PrefixAllocationParams& params = {testSeedPrefix, testAllocationPfxLen};
  EXPECT_EQ(params, config.getPrefixAllocationParams());
}

TEST(ConfigTest, BgpPeeringConfig) {
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
