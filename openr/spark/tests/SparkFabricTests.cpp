/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/Constants.h>
#include <openr/config/Config.h>
#include <openr/spark/SparkWrapper.h>
#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

class SparkFabricFixture : public testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider_ = std::make_shared<MockIoProvider>();
    mockIoProviderThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting mockIoProvider thread.";
      mockIoProvider_->start();
      LOG(INFO) << "mockIoProvider thread got stopped.";
    });
    mockIoProvider_->waitUntilRunning();
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping mockIoProvider thread.";
    mockIoProvider_->stop();
    mockIoProviderThread_->join();
  }

  std::shared_ptr<SparkWrapper>
  createSpark(
      std::string const& myNodeName,
      std::shared_ptr<const Config> config = nullptr,
      std::pair<uint32_t, uint32_t> version = std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion)) {
    return std::make_shared<SparkWrapper>(
        myNodeName, version, mockIoProvider_, config);
  }

  openr::thrift::FabricConfig
  makeFabricConfig() {
    openr::thrift::FabricConfig fabricConfig;
    fabricConfig.fabric_name() = fabricName_;
    fabricConfig.fabric_prefixes() = {"1::1/128", "2::/64"};
    fabricConfig.fabric_leaf_regexes() = {"eb01-ld\\d{3}\\.dfw1"};
    fabricConfig.fabric_spine_regexes() = {"eb01-sp\\d{3}\\.dfw1"};
    fabricConfig.fabric_control_regexes() = {"eb01-lc\\d{3}\\.dfw1"};
    fabricConfig.fabric_interface_regexes() = {"port-channel10\\d{3}"};
    return fabricConfig;
  }

  std::shared_ptr<MockIoProvider> mockIoProvider_{nullptr};
  std::unique_ptr<std::thread> mockIoProviderThread_{nullptr};

  const folly::CIDRNetwork ip1V4_ = folly::IPAddress::createNetwork(
      "192.168.0.1", 24, false /* apply mask */);
  const folly::CIDRNetwork ip2V4_ = folly::IPAddress::createNetwork(
      "192.168.0.2", 24, false /* apply mask */);
  const folly::CIDRNetwork ip1V6_ =
      folly::IPAddress::createNetwork("fe80::1/128");
  const folly::CIDRNetwork ip2V6_ =
      folly::IPAddress::createNetwork("fe80::2/128");

  const std::string fabricName_{"bbf01.dfw1"};
  const std::string nodeName1_{"eb01-sp002.dfw1"};
  const std::string nodeName2_{"eb01-ld002.dfw1"};
  const std::string nonFabricNodeName_{"eb01.rva1"};
  const std::string fabricIface1_{"port-channel10001"};
  const std::string fabricIface2_{"port-channel10002"};
  const std::string iface1_{"port-channel1001"};
  const std::string iface2_{"port-channel1002"};

  const int ifIndex1_{1};
  const int ifIndex2_{2};

  std::shared_ptr<Config> config1_;
  std::shared_ptr<Config> config2_;
  std::shared_ptr<SparkWrapper> node1_;
  std::shared_ptr<SparkWrapper> node2_;
};

//
// When two fabric nodes peer over a fabric internal interface
// (matching port-channel10\d{3}), each should see the other's
// real node name as remoteNodeName.
//
TEST_F(SparkFabricFixture, FabricInternalInterface) {
  mockIoProvider_->addIfNameIfIndex(
      {{fabricIface1_, ifIndex1_}, {fabricIface2_, ifIndex2_}});

  ConnectedIfPairs connectedPairs = {
      {fabricIface1_, {{fabricIface2_, 10}}},
      {fabricIface2_, {{fabricIface1_, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  openr::thrift::OpenrConfig tConfig1 = getBasicOpenrConfig(nodeName1_);
  openr::thrift::OpenrConfig tConfig2 = getBasicOpenrConfig(nodeName2_);
  tConfig1.thrift_server()->openr_ctrl_port() = 1;
  tConfig2.thrift_server()->openr_ctrl_port() = 1;
  tConfig1.fabric_config() = makeFabricConfig();
  tConfig2.fabric_config() = makeFabricConfig();
  config1_ = std::make_shared<Config>(tConfig1);
  config2_ = std::make_shared<Config>(tConfig2);

  node1_ = createSpark(nodeName1_, config1_);
  node2_ = createSpark(nodeName2_, config2_);

  node1_->updateInterfaceDb({InterfaceInfo(
      fabricIface1_, true /* isUp */, ifIndex1_, {ip1V4_, ip1V6_})});
  node2_->updateInterfaceDb({InterfaceInfo(
      fabricIface2_, true /* isUp */, ifIndex2_, {ip2V4_, ip2V6_})});

  {
    std::optional<NeighborEvents> events =
        node1_->waitForEvents(NeighborEventType::NEIGHBOR_UP);
    ASSERT_THAT(events.has_value(), ::testing::IsTrue());
    ASSERT_THAT(events.value().size(), ::testing::Eq(1));
    const NeighborEvent& event = events.value().back();
    EXPECT_THAT(event.localIfName, ::testing::Eq(fabricIface1_));
    EXPECT_THAT(event.remoteNodeName, ::testing::Eq(nodeName2_));
  }

  {
    std::optional<NeighborEvents> events =
        node2_->waitForEvents(NeighborEventType::NEIGHBOR_UP);
    ASSERT_THAT(events.has_value(), ::testing::IsTrue());
    ASSERT_THAT(events.value().size(), ::testing::Eq(1));
    const NeighborEvent& event = events.value().back();
    EXPECT_THAT(event.localIfName, ::testing::Eq(fabricIface2_));
    EXPECT_THAT(event.remoteNodeName, ::testing::Eq(nodeName1_));
  }
}

//
// When a fabric node peers with a non-fabric node over a non-fabric
// interface, the fabric node advertises its fabric name while the
// non-fabric node advertises its real node name.
//
TEST_F(SparkFabricFixture, FabricToNonFabricOverExternalInterface) {
  mockIoProvider_->addIfNameIfIndex(
      {{iface1_, ifIndex1_}, {iface2_, ifIndex2_}});

  ConnectedIfPairs connectedPairs = {
      {iface1_, {{iface2_, 10}}},
      {iface2_, {{iface1_, 10}}},
  };
  mockIoProvider_->setConnectedPairs(connectedPairs);

  // node1: fabric node with FabricConfig
  openr::thrift::OpenrConfig tConfig1 = getBasicOpenrConfig(nodeName1_);
  tConfig1.thrift_server()->openr_ctrl_port() = 1;
  tConfig1.fabric_config() = makeFabricConfig();
  config1_ = std::make_shared<Config>(tConfig1);

  // node2: non-fabric node without FabricConfig
  openr::thrift::OpenrConfig tConfig2 = getBasicOpenrConfig(nonFabricNodeName_);
  tConfig2.thrift_server()->openr_ctrl_port() = 1;
  config2_ = std::make_shared<Config>(tConfig2);

  node1_ = createSpark(nodeName1_, config1_);
  node2_ = createSpark(nonFabricNodeName_, config2_);

  node1_->updateInterfaceDb(
      {InterfaceInfo(iface1_, true /* isUp */, ifIndex1_, {ip1V4_, ip1V6_})});
  node2_->updateInterfaceDb(
      {InterfaceInfo(iface2_, true /* isUp */, ifIndex2_, {ip2V4_, ip2V6_})});

  {
    std::optional<NeighborEvents> events =
        node1_->waitForEvents(NeighborEventType::NEIGHBOR_UP);
    ASSERT_THAT(events.has_value(), ::testing::IsTrue());
    ASSERT_THAT(events.value().size(), ::testing::Eq(1));
    const NeighborEvent& event = events.value().back();
    EXPECT_THAT(event.localIfName, ::testing::Eq(iface1_));
    EXPECT_THAT(event.remoteNodeName, ::testing::Eq(nonFabricNodeName_));
  }

  {
    std::optional<NeighborEvents> events =
        node2_->waitForEvents(NeighborEventType::NEIGHBOR_UP);
    ASSERT_THAT(events.has_value(), ::testing::IsTrue());
    ASSERT_THAT(events.value().size(), ::testing::Eq(1));
    const NeighborEvent& event = events.value().back();
    EXPECT_THAT(event.localIfName, ::testing::Eq(iface2_));
    EXPECT_THAT(event.remoteNodeName, ::testing::Eq(fabricName_));
  }
}
