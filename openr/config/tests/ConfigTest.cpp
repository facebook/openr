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
  lmConf.include_interface_regexes_ref()->emplace_back("fboss.*");
  lmConf.exclude_interface_regexes_ref()->emplace_back("eth.*");
  lmConf.redistribute_interface_regexes_ref()->emplace_back("lo");
  return lmConf;
}

openr::thrift::KvstoreFloodRate
getFloodRate() {
  openr::thrift::KvstoreFloodRate floodrate;
  floodrate.flood_msg_per_sec_ref() = 1;
  floodrate.flood_msg_burst_size_ref() = 1;
  return floodrate;
}

openr::thrift::PrefixAllocationConfig
getPrefixAllocationConfig(openr::thrift::PrefixAllocationMode mode) {
  openr::thrift::PrefixAllocationConfig pfxAllocationConf;
  pfxAllocationConf.prefix_allocation_mode_ref() = mode;
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
  *area.area_id_ref() = areaId;
  area.include_interface_regexes_ref()->emplace_back("fboss.*");
  area.neighbor_regexes_ref()->emplace_back("rsw.*");
  return area;
}

const std::string myArea = "myArea";

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
    confInvalidArea.areas_ref()->emplace_back(getAreaConfig("1"));
    confInvalidArea.areas_ref()->emplace_back(getAreaConfig("1"));
    EXPECT_THROW((Config(confInvalidArea)), std::invalid_argument);
  }

  // non-empty interface regex
  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = myArea;
    areaConfig.include_interface_regexes_ref()->emplace_back("iface.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  // non-empty neighbor regexes
  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = myArea;
    areaConfig.neighbor_regexes_ref()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  // non-empty neighbor and interface regexes
  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = myArea;
    areaConfig.include_interface_regexes_ref()->emplace_back("iface.*");
    areaConfig.neighbor_regexes_ref()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = myArea;
    areaConfig.include_interface_regexes_ref()->emplace_back("iface.*");
    areaConfig.neighbor_regexes_ref()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", "domain", vec);
    Config cfg = Config(confValidArea);
    // default area and domain area
    EXPECT_EQ(cfg.getAreas().size(), 1);
    EXPECT_EQ(cfg.getAreas().count(myArea), 1);
    EXPECT_EQ(cfg.getAreas().count("1"), 0);
  }

  // invalid include_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = myArea;
    areaConfig.include_interface_regexes_ref()->emplace_back("[0-9]++");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }
  //  invalid exclude_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = myArea;
    areaConfig.exclude_interface_regexes_ref()->emplace_back("boom\\");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }
  //  invalid redistribute_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = myArea;
    areaConfig.redistribute_interface_regexes_ref()->emplace_back("*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }
}

TEST(ConfigTest, AreaConfiguration) {
  openr::thrift::AreaConfig areaConfig;
  *areaConfig.area_id_ref() = "myArea";
  areaConfig.include_interface_regexes_ref()->emplace_back("iface.*");
  areaConfig.exclude_interface_regexes_ref()->emplace_back(".*400.*");
  areaConfig.exclude_interface_regexes_ref()->emplace_back(".*450.*");
  areaConfig.redistribute_interface_regexes_ref()->emplace_back("loopback1");
  areaConfig.neighbor_regexes_ref()->emplace_back("fsw.*");
  Config cfg{getBasicOpenrConfig("node-1", "domain", {areaConfig})};

  auto const& areaConf = cfg.getAreas().at("myArea");
  EXPECT_TRUE(areaConf.shouldPeerWithNeighbor("fsw001"));
  EXPECT_FALSE(areaConf.shouldPeerWithNeighbor("rsw001"));
  EXPECT_FALSE(areaConf.shouldPeerWithNeighbor(""));

  EXPECT_TRUE(areaConf.shouldDiscoverOnIface("iface20"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("iface400"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("iface450"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("loopback1"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface(""));

  EXPECT_TRUE(areaConf.shouldRedistributeIface("loopback1"));
  EXPECT_FALSE(areaConf.shouldRedistributeIface("loopback10"));
  EXPECT_FALSE(areaConf.shouldRedistributeIface("iface450"));
  EXPECT_FALSE(areaConf.shouldRedistributeIface(""));
}

TEST(ConfigTest, PopulateInternalDb) {
  // features

  // KSP2_ED_ECMP with IP
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.prefix_forwarding_type_ref() = thrift::PrefixForwardingType::IP;
    confInvalid.prefix_forwarding_algorithm_ref() =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // RibPolicy
  {
    auto conf = getBasicOpenrConfig();
    conf.enable_rib_policy_ref() = true;
    EXPECT_TRUE(Config(conf).isRibPolicyEnabled());
  }

  // kvstore

  // flood_msg_per_sec <= 0
  {
    auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
    confInvalidFloodMsgPerSec.kvstore_config_ref()->flood_rate_ref() =
        getFloodRate();
    confInvalidFloodMsgPerSec.kvstore_config_ref()
        ->flood_rate_ref()
        ->flood_msg_per_sec_ref() = 0;
    EXPECT_THROW((Config(confInvalidFloodMsgPerSec)), std::out_of_range);
  }
  // flood_msg_burst_size <= 0
  {
    auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
    confInvalidFloodMsgPerSec.kvstore_config_ref()->flood_rate_ref() =
        getFloodRate();
    confInvalidFloodMsgPerSec.kvstore_config_ref()
        ->flood_rate_ref()
        ->flood_msg_burst_size_ref() = 0;
    EXPECT_THROW((Config(confInvalidFloodMsgPerSec)), std::out_of_range);
  }

  // Spark

  // Exception: neighbor_discovery_port <= 0 or > 65535
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->neighbor_discovery_port_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);

    confInvalidSpark.spark_config_ref()->neighbor_discovery_port_ref() = 65536;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: hello_time_s <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->hello_time_s_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: fastinit_hello_time_ms <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->fastinit_hello_time_ms_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: fastinit_hello_time_ms > hello_time_s
  {
    auto confInvalidSpark2 = getBasicOpenrConfig();
    confInvalidSpark2.spark_config_ref()->fastinit_hello_time_ms_ref() = 10000;
    confInvalidSpark2.spark_config_ref()->hello_time_s_ref() = 2;
    EXPECT_THROW(auto c = Config(confInvalidSpark2), std::invalid_argument);
  }

  // Exception: keepalive_time_s <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->keepalive_time_s_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: keepalive_time_s > hold_time_s
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->keepalive_time_s_ref() = 10;
    confInvalidSpark.spark_config_ref()->hold_time_s_ref() = 5;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception: graceful_restart_time_s < 3 * keepalive_time_s
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->keepalive_time_s_ref() = 10;
    confInvalidSpark.spark_config_ref()->graceful_restart_time_s_ref() = 20;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception step_detector_fast_window_size >= 0
  //           step_detector_slow_window_size >= 0
  //           step_detector_lower_threshold >= 0
  //           step_detector_upper_threshold >= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->fast_window_size_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->slow_window_size_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->lower_threshold_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->upper_threshold_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception step_detector_fast_window_size > step_detector_slow_window_size
  //           step_detector_lower_threshold > step_detector_upper_threshold
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->fast_window_size_ref() = 10;
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->slow_window_size_ref() = 5;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);

    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->upper_threshold_ref() = 5;
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->lower_threshold_ref() = 10;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Monitor

  // Exception monitor_max_event_log >= 0
  {
    auto confInvalidMon = getBasicOpenrConfig();
    confInvalidMon.monitor_config_ref()->max_event_log_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidMon), std::out_of_range);
  }

  // link monitor

  // linkflap_initial_backoff_ms < 0
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config_ref()->linkflap_initial_backoff_ms_ref() =
        -1;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }
  // linkflap_max_backoff_ms < 0
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config_ref()->linkflap_max_backoff_ms_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }
  // linkflap_initial_backoff_ms > linkflap_max_backoff_ms
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config_ref()->linkflap_initial_backoff_ms_ref() =
        360000;
    confInvalidLm.link_monitor_config_ref()->linkflap_max_backoff_ms_ref() =
        300000;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }

  // prefix allocation

  // enable_prefix_allocation = true, prefix_allocation_config = null
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }
  // prefix_allocation_mode != DYNAMIC_ROOT_NODE, seed_prefix and
  // allocate_prefix_len set
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->prefix_allocation_mode_ref() =
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
    confInvalidPa.prefix_allocation_config_ref()->prefix_allocation_mode_ref() =
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

    // Both bgp-config & translation-config are none
    confInvalid.bgp_config_ref().reset();
    confInvalid.bgp_translation_config_ref().reset();
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);

    // bgp config is set but translation-config is not
    confInvalid.bgp_config_ref() = thrift::BgpConfig();
    confInvalid.bgp_translation_config_ref().reset();
    // TODO: Expect an exception instead of default initialization
    // EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
    EXPECT_EQ(
        thrift::BgpRouteTranslationConfig(),
        Config(confInvalid).getBgpTranslationConfig());

    // translation-config is set but bgp-config is not
    confInvalid.bgp_config_ref().reset();
    confInvalid.bgp_translation_config_ref() =
        thrift::BgpRouteTranslationConfig();
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
        {}, /* area config */
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
    EXPECT_EQ(1, config.getAreas().size());
    EXPECT_EQ(1, config.getAreas().count(kTestingAreaName));

    // enable_v4
    EXPECT_TRUE(config.isV4Enabled());
    // enable_segment_routing
    EXPECT_FALSE(config.isSegmentRoutingEnabled());
    // isBgpPeeringEnabled
    EXPECT_FALSE(config.isBgpPeeringEnabled());
    // enable_flood_optimization
    EXPECT_FALSE(config.isFloodOptimizationEnabled());
    // enable_best_route_selection
    EXPECT_FALSE(config.isBestRouteSelectionEnabled());

    // getSparkConfig
    EXPECT_EQ(*tConfig.spark_config_ref(), config.getSparkConfig());
  }

  // config with bgp peering
  {
    auto tConfig = getBasicOpenrConfig("fsw001");
    tConfig.enable_bgp_peering_ref() = true;

    FLAGS_node_name = "fsw001";
    const auto& bgpConf = GflagConfig::getBgpAutoConfig();
    tConfig.bgp_config_ref() = bgpConf;
    tConfig.bgp_translation_config_ref() = thrift::BgpRouteTranslationConfig();

    auto config = Config(tConfig);

    // isBgpPeeringEnabled
    EXPECT_TRUE(config.isBgpPeeringEnabled());
    EXPECT_EQ(bgpConf, config.getBgpConfig());
    EXPECT_EQ(
        thrift::BgpRouteTranslationConfig(), config.getBgpTranslationConfig());
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
  *tConfig.link_monitor_config_ref() = lmConf;
  // set empty area list to see doamin get converted to area
  tConfig.set_areas({});
  auto config = Config(tConfig);

  // getLinkMonitorConfig
  EXPECT_EQ(lmConf, config.getLinkMonitorConfig());

  // check to see the link monitor options got converted to an area config with
  // domainName
  auto const& domainNameArea =
      config.getAreas().at(thrift::KvStore_constants::kDefaultArea());
  EXPECT_TRUE(domainNameArea.shouldDiscoverOnIface("fboss10"));
  EXPECT_FALSE(domainNameArea.shouldDiscoverOnIface("eth0"));

  EXPECT_TRUE(domainNameArea.shouldRedistributeIface("lo"));
  EXPECT_FALSE(domainNameArea.shouldRedistributeIface("eth0"));
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
