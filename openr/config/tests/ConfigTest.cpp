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

#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {

class ConfigTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    thrift::LinkMonitorConfig linkMonitorConfig;
    linkMonitorConfig.include_interface_regexes =
        std::vector<std::string>{"et[0-9].*"};
    linkMonitorConfig.exclude_interface_regexes =
        std::vector<std::string>{"eth0"};
    linkMonitorConfig.redistribute_interface_regexes =
        std::vector<std::string>{"lo1"};

    thrift::KvstoreConfig kvstoreConfig;
    kvstoreConfig.enable_flood_optimization = true;
    kvstoreConfig.use_flood_optimization = true;
    kvstoreConfig.is_flood_root = true;

    thrift::SparkConfig sparkConfig;
    sparkConfig.graceful_restart_time_s = 60;

    thrift::WatchdogConfig watchdogConfig;

    validConfig_.node_name = "node";
    validConfig_.domain = "domain";
    validConfig_.enable_v4 = true;
    validConfig_.enable_netlink_system_handler = true;
    validConfig_.kvstore_config = kvstoreConfig;
    validConfig_.link_monitor_config = linkMonitorConfig;
    validConfig_.spark_config = sparkConfig;
    validConfig_.watchdog_config = watchdogConfig;

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

TEST_F(ConfigTestFixture, LoadConfigTest) {
  EXPECT_NO_THROW(Config(validConfigFile_.path().string()));

  // wrong mandatory fields
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

} // namespace openr
