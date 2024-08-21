/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/FileUtil.h>
#include <folly/testing/TestUtil.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#define Config_TEST_FRIENDS FRIEND_TEST(ConfigTest, PopulateInternalDb);

#include <openr/common/Constants.h>
#include <openr/common/MplsUtil.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/tests/utils/Utils.h>

#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace {

openr::thrift::KvstoreFloodRate
getFloodRate() {
  openr::thrift::KvstoreFloodRate floodrate;
  floodrate.flood_msg_per_sec() = 1;
  floodrate.flood_msg_burst_size() = 1;
  return floodrate;
}

openr::thrift::AreaConfig
getAreaConfig(const std::string& areaId) {
  openr::thrift::AreaConfig area;
  area.area_id() = areaId;
  area.include_interface_regexes()->emplace_back("fboss.*");
  area.neighbor_regexes()->emplace_back("rsw.*");
  return area;
}

const std::string myArea = "myArea";

} // namespace

namespace openr {

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
    confInvalidArea.areas()->emplace_back(getAreaConfig("1"));
    confInvalidArea.areas()->emplace_back(getAreaConfig("1"));
    EXPECT_THROW((Config(confInvalidArea)), std::invalid_argument);
  }
  // cannot find policy definition for area policy
  {
    auto confInvalidAreaPolicy = getBasicOpenrConfig();
    auto areaConfig = getAreaConfig("1");
    areaConfig.import_policy_name() = "BLA";
    confInvalidAreaPolicy.areas()->emplace_back(std::move(areaConfig));
    EXPECT_THROW((Config(confInvalidAreaPolicy)), std::invalid_argument);
  }

  // non-empty interface regex
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id() = myArea;
    areaConfig.include_interface_regexes()->emplace_back("iface.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", vec);
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  // non-empty neighbor regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id() = myArea;
    areaConfig.neighbor_regexes()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", vec);
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  // non-empty neighbor and interface regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id() = myArea;
    areaConfig.include_interface_regexes()->emplace_back("iface.*");
    areaConfig.neighbor_regexes()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", vec);
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id() = myArea;
    areaConfig.include_interface_regexes()->emplace_back("iface.*");
    areaConfig.neighbor_regexes()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", vec);
    Config cfg = Config(confValidArea);
    // default area and domain area
    EXPECT_EQ(cfg.getAreas().size(), 1);
    EXPECT_EQ(cfg.getAreas().count(myArea), 1);
    EXPECT_EQ(cfg.getAreas().count("1"), 0);
  }

  // invalid include_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id() = myArea;
    areaConfig.include_interface_regexes()->emplace_back("[0-9]++");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }
  //  invalid exclude_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id() = myArea;
    areaConfig.exclude_interface_regexes()->emplace_back("boom\\");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }
  //  invalid redistribute_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id() = myArea;
    areaConfig.redistribute_interface_regexes()->emplace_back("*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }
}

TEST(ConfigTest, AreaConfiguration) {
  openr::thrift::AreaConfig areaConfig;
  areaConfig.area_id() = "myArea";
  areaConfig.include_interface_regexes()->emplace_back("iface.*");
  areaConfig.exclude_interface_regexes()->emplace_back(".*400.*");
  areaConfig.exclude_interface_regexes()->emplace_back(".*450.*");
  areaConfig.redistribute_interface_regexes()->emplace_back("loopback1");
  areaConfig.neighbor_regexes()->emplace_back("fsw.*");
  Config cfg{getBasicOpenrConfig("node-1", {areaConfig})};

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

TEST(ConfigTest, EbbInterfaceConfiguration) {
  openr::thrift::AreaConfig areaConfig;
  areaConfig.area_id() = "myArea";
  areaConfig.include_interface_regexes()->emplace_back("po1000[0-9]{1}");
  areaConfig.include_interface_regexes()->emplace_back("po[0-9]{4}");
  areaConfig.include_interface_regexes()->emplace_back("po[0-9]{6}");
  Config cfg{getBasicOpenrConfig("node-1", {areaConfig})};

  auto const& areaConf = cfg.getAreas().at("myArea");

  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("po1"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("po12"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("po123"));
  EXPECT_TRUE(areaConf.shouldDiscoverOnIface("po1234"));
  for (int i = 0; i < 10; ++i) {
    std::string interfaceName = fmt::format("po1000{}", i);
    EXPECT_TRUE(areaConf.shouldDiscoverOnIface(interfaceName));
  }
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("po10010"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("po12345"));
  EXPECT_TRUE(areaConf.shouldDiscoverOnIface("po123456"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("po1234567"));
}

TEST(ConfigTest, PopulateInternalDb) {
  // features

  // KSP2_ED_ECMP with IP
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.prefix_forwarding_type() = thrift::PrefixForwardingType::IP;
    confInvalid.prefix_forwarding_algorithm() =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // RibPolicy
  {
    auto conf = getBasicOpenrConfig();
    conf.enable_rib_policy() = true;
    EXPECT_TRUE(Config(conf).isRibPolicyEnabled());
  }

  // kvstore

  // flood_msg_per_sec <= 0
  {
    auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
    confInvalidFloodMsgPerSec.kvstore_config()->flood_rate() = getFloodRate();
    confInvalidFloodMsgPerSec.kvstore_config()
        ->flood_rate()
        ->flood_msg_per_sec() = 0;
    EXPECT_THROW((Config(confInvalidFloodMsgPerSec)), std::out_of_range);
  }
  // flood_msg_burst_size <= 0
  {
    auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
    confInvalidFloodMsgPerSec.kvstore_config()->flood_rate() = getFloodRate();
    confInvalidFloodMsgPerSec.kvstore_config()
        ->flood_rate()
        ->flood_msg_burst_size() = 0;
    EXPECT_THROW((Config(confInvalidFloodMsgPerSec)), std::out_of_range);
  }

  // Spark

  // Exception: neighbor_discovery_port <= 0 or > 65535
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config()->neighbor_discovery_port() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);

    confInvalidSpark.spark_config()->neighbor_discovery_port() = 65536;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: hello_time_s <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config()->hello_time_s() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: fastinit_hello_time_ms <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config()->fastinit_hello_time_ms() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: fastinit_hello_time_ms > hello_time_s
  {
    auto confInvalidSpark2 = getBasicOpenrConfig();
    confInvalidSpark2.spark_config()->fastinit_hello_time_ms() = 10000;
    confInvalidSpark2.spark_config()->hello_time_s() = 2;
    EXPECT_THROW(auto c = Config(confInvalidSpark2), std::invalid_argument);
  }

  // Exception: keepalive_time_s <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config()->keepalive_time_s() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: keepalive_time_s > hold_time_s
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config()->keepalive_time_s() = 10;
    confInvalidSpark.spark_config()->hold_time_s() = 5;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception: graceful_restart_time_s < 3 * keepalive_time_s
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config()->keepalive_time_s() = 10;
    confInvalidSpark.spark_config()->graceful_restart_time_s() = 20;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception step_detector_fast_window_size >= 0
  //           step_detector_slow_window_size >= 0
  //           step_detector_lower_threshold >= 0
  //           step_detector_upper_threshold >= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config()->step_detector_conf()->fast_window_size() =
        -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config()->step_detector_conf()->slow_window_size() =
        -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config()->step_detector_conf()->lower_threshold() =
        -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config()->step_detector_conf()->upper_threshold() =
        -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception step_detector_fast_window_size > step_detector_slow_window_size
  //           step_detector_lower_threshold > step_detector_upper_threshold
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config()->step_detector_conf()->fast_window_size() =
        10;
    confInvalidSpark.spark_config()->step_detector_conf()->slow_window_size() =
        5;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);

    confInvalidSpark.spark_config()->step_detector_conf()->upper_threshold() =
        5;
    confInvalidSpark.spark_config()->step_detector_conf()->lower_threshold() =
        10;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Monitor

  // Exception monitor_max_event_log >= 0
  {
    auto confInvalidMon = getBasicOpenrConfig();
    confInvalidMon.monitor_config()->max_event_log() = -1;
    EXPECT_THROW(auto c = Config(confInvalidMon), std::out_of_range);
  }

  // link monitor

  // linkflap_initial_backoff_ms < 0
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config()->linkflap_initial_backoff_ms() = -1;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }
  // linkflap_max_backoff_ms < 0
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config()->linkflap_max_backoff_ms() = -1;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }
  // linkflap_initial_backoff_ms > linkflap_max_backoff_ms
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config()->linkflap_initial_backoff_ms() = 360000;
    confInvalidLm.link_monitor_config()->linkflap_max_backoff_ms() = 300000;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }

  // watchdog

  // watchdog enabled with empty watchdog_config
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.enable_watchdog() = true;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // vip service
  {
    auto conf = getBasicOpenrConfig();
    EXPECT_FALSE(Config(conf).isVipServiceEnabled());
    conf.enable_vip_service() = true;
    EXPECT_THROW(Config(conf).isVipServiceEnabled(), std::invalid_argument);
    EXPECT_THROW(Config(conf).checkVipServiceConfig(), std::invalid_argument);
    conf.vip_service_config() = {};
    conf.vip_service_config()->ingress_policy() = "test_policy";
    // There is no area_policies, so should throw.
    EXPECT_THROW(Config(conf).checkVipServiceConfig(), std::invalid_argument);
    conf.area_policies() = neteng::config::routing_policy::PolicyConfig();
    conf.area_policies()->filters() =
        neteng::config::routing_policy::PolicyFilters();
    conf.area_policies()->filters()->routePropagationPolicy() =
        neteng::config::routing_policy::Filters();
    // There is policies, but no vip ingress policy, should throw
    EXPECT_THROW(Config(conf).checkVipServiceConfig(), std::invalid_argument);
    std::map<std::string, neteng::config::routing_policy::Filter> policy;
    policy["test_policy"] = neteng::config::routing_policy::Filter();
    conf.area_policies()->filters()->routePropagationPolicy()->objects() =
        policy;
    // There is vip ingress policy in area_policies, should pass
    EXPECT_NO_THROW(Config(conf).checkVipServiceConfig());
  }

  // FIB route deletion
  {
    auto conf = getBasicOpenrConfig();
    conf.route_delete_delay_ms() = -1;
    EXPECT_THROW((Config(conf)), std::invalid_argument);

    conf.route_delete_delay_ms() = 0;
    EXPECT_NO_THROW((Config(conf)));

    conf.route_delete_delay_ms() = 1000;
    EXPECT_NO_THROW((Config(conf)));
  }
}

TEST(ConfigTest, SoftdrainConfigTest) {
  auto tConfig = getBasicOpenrConfig();
  tConfig.enable_soft_drain() = true;

  // no soft-drained flag
  auto config = Config(tConfig);
  EXPECT_TRUE(config.isSoftdrainEnabled());
}

TEST(ConfigTest, GeneralGetter) {
  // config without bgp peering
  {
    auto tConfig = getBasicOpenrConfig(
        "node-1",
        {}, /* area config */
        true /* enableV4 */,
        false /* enableSegmentRouting */,
        true /*dryrun*/);
    auto config = Config(tConfig);

    // getNodeName
    EXPECT_EQ("node-1", config.getNodeName());

    // getAreaIds
    EXPECT_EQ(1, config.getAreas().size());
    EXPECT_EQ(1, config.getAreas().count(kTestingAreaName));

    // enable_v4
    EXPECT_TRUE(config.isV4Enabled());
    // enable_segment_routing
    EXPECT_FALSE(config.isSegmentRoutingEnabled());
    // enable_best_route_selection
    EXPECT_FALSE(config.isBestRouteSelectionEnabled());
    // enable_v4_over_v6_nexthop
    EXPECT_FALSE(config.isV4OverV6NexthopEnabled());
    // enable_vip_service
    EXPECT_FALSE(config.isVipServiceEnabled());
    // enable_soft_drain
    EXPECT_TRUE(config.isSoftdrainEnabled());

    // getSparkConfig
    EXPECT_EQ(*tConfig.spark_config(), config.getSparkConfig());
  }

  // config without bgp peering and only for v4_over_v6_nexthop
  {
    auto tConfig = getBasicOpenrConfig(
        "node-1",
        {} /* area config */,
        true /* enable v4 */,
        false /* enableSegmentRouting */,
        true /* dryrun */,
        true /* enableV4OverV6Nexthop */);
    auto config = Config(tConfig);

    // enable_v4_over_v6_nexthop
    EXPECT_TRUE(config.isV4OverV6NexthopEnabled());
  }

  // config with watchdog
  {
    auto tConfig = getBasicOpenrConfig("fsw001");
    tConfig.enable_watchdog() = true;
    const auto& watchdogConf = thrift::WatchdogConfig();
    tConfig.watchdog_config() = watchdogConf;

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
  // set empty area list to see doamin get converted to area
  tConfig.areas().emplace();
  auto config = Config(tConfig);

  // check to see the link monitor options got converted to an area config with
  // domainName
  auto const& domainNameArea =
      config.getAreas().at(Constants::kDefaultArea.toString());
  EXPECT_FALSE(domainNameArea.shouldDiscoverOnIface("eth0"));

  EXPECT_FALSE(domainNameArea.shouldRedistributeIface("eth0"));
}

TEST(ConfigTest, ToThriftKvStoreConfig) {
  auto tConfig = getBasicOpenrConfig();
  auto config = Config(tConfig);

  EXPECT_NO_THROW(config.toThriftKvStoreConfig());
}

TEST(ConfigTest, NonDefaultVrfConfigGetter) {
  std::string mgmtVrf{"mgmtVrf"};
  thrift::ThriftServerConfig thrift_server_config;
  thrift_server_config.vrf_names() = {mgmtVrf};

  auto tConfig = getBasicOpenrConfig();
  tConfig.thrift_server() = thrift_server_config;

  // no soft-drained flag
  auto config = Config(tConfig);
  const auto vrfs = config.getNonDefaultVrfNames();
  EXPECT_EQ(1, vrfs.size());
  EXPECT_EQ(mgmtVrf, vrfs.front());
}

TEST(ConfigTest, DefaultVrfConfigGetter) {
  std::string defaultVrf{"defaultVrf"};
  thrift::ThriftServerConfig thrift_server_config;
  thrift_server_config.default_vrf_name() = defaultVrf;

  auto tConfig = getBasicOpenrConfig();
  tConfig.thrift_server() = thrift_server_config;

  // no soft-drained flag
  auto config = Config(tConfig);
  const auto vrf = config.getDefaultVrfName();
  EXPECT_EQ(defaultVrf, vrf);
}

TEST(ConfigTest, DefaultVrfConfigGetterUnsetValue) {
  thrift::ThriftServerConfig thrift_server_config;

  auto tConfig = getBasicOpenrConfig();
  tConfig.thrift_server() = thrift_server_config;

  // no soft-drained flag
  auto config = Config(tConfig);
  const auto vrf = config.getDefaultVrfName();
  EXPECT_EQ("", vrf);
}
} // namespace openr
