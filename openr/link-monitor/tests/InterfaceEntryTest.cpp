/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <re2/re2.h>
#include <re2/set.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/link-monitor/InterfaceEntry.h>

namespace openr {

std::unordered_set<folly::CIDRNetwork>
toCIDRNetworkSet(std::vector<folly::CIDRNetwork> const& prefixes) {
  return std::unordered_set<folly::CIDRNetwork>{
      prefixes.begin(), prefixes.end()};
}

/**
 * Basic test for verifying get/set behaviors
 * - Create InterfaceEntry object
 * - Push some updates and verify them via get attributes
 * - Verify timers/throttles
 */
TEST(InterfaceEntry, GetSetTest) {
  OpenrEventBase evl;
  AsyncThrottle throttle(evl.getEvb(), std::chrono::milliseconds(1), []() {});
  auto timeout = folly::AsyncTimeout::make(*evl.getEvb(), []() noexcept {});
  InterfaceEntry interface(
      "iface1",
      std::chrono::milliseconds(1),
      std::chrono::milliseconds(64),
      throttle,
      *timeout);

  EXPECT_EQ("iface1", interface.getIfName());

  // Update attributes
  EXPECT_TRUE(interface.updateAttrs(1, true));
  EXPECT_EQ(1, interface.getIfIndex());
  EXPECT_TRUE(interface.isUp());
  EXPECT_TRUE(interface.isActive());
  EXPECT_EQ(std::chrono::milliseconds(0), interface.getBackoffDuration());
  EXPECT_TRUE(throttle.isActive());
  EXPECT_FALSE(timeout->isScheduled());
  throttle.cancel();

  // Add and validate addresses
  std::unordered_set<folly::CIDRNetwork> addresses = {
      folly::IPAddress::createNetwork(
          "169.254.0.1/16", -1, false), // link-local
      folly::IPAddress::createNetwork("1.2.3.4/24", -1, false),
      folly::IPAddress::createNetwork("232.0.0.1/8", -1, false), // multicast
      folly::IPAddress::createNetwork("fe80::1/64", -1, false), // link-local
      folly::IPAddress::createNetwork("ff02::1/64", -1, false), // multicast
      folly::IPAddress::createNetwork(
          "24:db:21:6048:face:0:1b:0/64", -1, false)};
  for (auto& addr : addresses) {
    EXPECT_TRUE(interface.updateAddr(addr, true));
  }
  EXPECT_TRUE(throttle.isActive());
  EXPECT_FALSE(timeout->isScheduled());
  throttle.cancel();

  // Validate v4 addresses
  std::set<folly::CIDRNetwork> v4Addrs = {
      folly::IPAddress::createNetwork("169.254.0.1", 16, false), // link-local
      folly::IPAddress::createNetwork("1.2.3.4", 24, false),
      folly::IPAddress::createNetwork("232.0.0.1", 8, false), // multicast
  };
  EXPECT_EQ(v4Addrs, interface.getInterfaceInfo().getSortedV4Addrs());

  // Validate v6 link-local addresses
  std::set<folly::CIDRNetwork> v6LinkLocalAddrs = {
      folly::IPAddress::createNetwork("fe80::1", 64, false), // link-local
  };
  EXPECT_EQ(
      v6LinkLocalAddrs,
      interface.getInterfaceInfo().getSortedV6LinkLocalAddrs());

  // Validate redistriubte prefixes (link-local and multicast addrs will
  // be ignored)
  std::unordered_set<folly::CIDRNetwork> redistAddrsAll = {
      folly::IPAddress::createNetwork("1.2.3.4/24"),
      folly::IPAddress::createNetwork("24:db:21:6048:face:0:1b:0/64")};
  std::unordered_set<folly::CIDRNetwork> redistAddrsV6 = {
      folly::IPAddress::createNetwork("24:db:21:6048:face:0:1b:0/64")};
  EXPECT_EQ(
      redistAddrsAll,
      toCIDRNetworkSet(interface.getGlobalUnicastNetworks(true)));
  EXPECT_EQ(
      redistAddrsV6,
      toCIDRNetworkSet(interface.getGlobalUnicastNetworks(false)));
}

/**
 * Test exponential backoff functionality of InterfaceEntry
 */
TEST(InterfaceEntry, BackoffTest) {
  OpenrEventBase evl;
  AsyncThrottle throttle(evl.getEvb(), std::chrono::milliseconds(1), []() {});
  auto timeout = folly::AsyncTimeout::make(*evl.getEvb(), []() noexcept {});
  InterfaceEntry interface(
      "iface1",
      std::chrono::milliseconds(8),
      std::chrono::milliseconds(512),
      throttle,
      *timeout);
  std::chrono::milliseconds backoff{0};

  // 1. Set interface to UP
  EXPECT_TRUE(interface.updateAttrs(1, true));
  EXPECT_TRUE(interface.isUp());
  EXPECT_TRUE(interface.isActive());
  EXPECT_EQ(std::chrono::milliseconds(0), interface.getBackoffDuration());
  EXPECT_TRUE(throttle.isActive());
  EXPECT_FALSE(timeout->isScheduled());
  throttle.cancel();

  // 2. Set interface to DOWN (backoff = 8ms)
  // NOTE: Ensure timeout gets scheduled
  EXPECT_TRUE(interface.updateAttrs(1, false));
  EXPECT_FALSE(interface.isUp());
  EXPECT_FALSE(interface.isActive());
  backoff = interface.getBackoffDuration();
  EXPECT_GE(std::chrono::milliseconds(8), backoff);
  EXPECT_LE(std::chrono::milliseconds(0), backoff);
  EXPECT_TRUE(throttle.isActive());
  EXPECT_TRUE(timeout->isScheduled());
  throttle.cancel();
  timeout->cancelTimeout();

  // 3. Set interface to up
  EXPECT_TRUE(interface.updateAttrs(1, true));
  EXPECT_TRUE(interface.isUp());
  EXPECT_FALSE(interface.isActive());

  // 4. Wait for interface to become active
  /* sleep override */
  std::this_thread::sleep_for(backoff);
  EXPECT_TRUE(interface.isUp());
  EXPECT_TRUE(interface.isActive());
  backoff = interface.getBackoffDuration();
  EXPECT_EQ(std::chrono::milliseconds(0), backoff);
  EXPECT_TRUE(throttle.isActive());
  EXPECT_FALSE(timeout->isScheduled());
  throttle.cancel();

  // 5. Bring down interface again (backoff = 16ms)
  // NOTE: Ensure timeout gets scheduled
  EXPECT_TRUE(interface.updateAttrs(1, false));
  EXPECT_FALSE(interface.isUp());
  EXPECT_FALSE(interface.isActive());
  backoff = interface.getBackoffDuration();
  EXPECT_GE(std::chrono::milliseconds(16), backoff);
  EXPECT_LE(std::chrono::milliseconds(8), backoff);
  EXPECT_TRUE(throttle.isActive());
  EXPECT_TRUE(timeout->isScheduled());
  throttle.cancel();
  timeout->cancelTimeout();

  // 6. Set interface to up but it remains inactive
  EXPECT_TRUE(interface.updateAttrs(1, true));
  EXPECT_TRUE(interface.isUp());
  EXPECT_FALSE(interface.isActive());

  // 7. Wait for interface to become active
  /* sleep override */
  std::this_thread::sleep_for(backoff);
  EXPECT_TRUE(interface.isUp());
  EXPECT_TRUE(interface.isActive());
  backoff = interface.getBackoffDuration();
  EXPECT_EQ(std::chrono::milliseconds(0), backoff);
  EXPECT_TRUE(throttle.isActive());
  EXPECT_FALSE(timeout->isScheduled());
  throttle.cancel();

  // 8. Bring down interface 3 times and see backoff becomes 128ms
  EXPECT_TRUE(interface.updateAttrs(1, false));
  EXPECT_TRUE(interface.updateAttrs(1, true));
  EXPECT_TRUE(interface.updateAttrs(1, false));
  EXPECT_TRUE(interface.updateAttrs(1, true));
  EXPECT_TRUE(interface.updateAttrs(1, false));
  EXPECT_TRUE(interface.updateAttrs(1, true));
  EXPECT_TRUE(interface.isUp());
  EXPECT_FALSE(interface.isActive());
  backoff = interface.getBackoffDuration();
  EXPECT_GE(std::chrono::milliseconds(128), backoff);
  EXPECT_LE(std::chrono::milliseconds(64), backoff);
  EXPECT_TRUE(throttle.isActive());
  EXPECT_TRUE(timeout->isScheduled());
  throttle.cancel();
  timeout->cancelTimeout();

  // 9. Wait for maxBackoff for backoff to clear
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(512));
  EXPECT_TRUE(interface.isUp());
  EXPECT_TRUE(interface.isActive());
  backoff = interface.getBackoffDuration();
  EXPECT_EQ(std::chrono::milliseconds(0), backoff);

  // 10. Trigger down event and ensure backoff=8ms (starts fresh)
  EXPECT_TRUE(interface.updateAttrs(1, false));
  EXPECT_FALSE(interface.isUp());
  EXPECT_FALSE(interface.isActive());
  backoff = interface.getBackoffDuration();
  EXPECT_GE(std::chrono::milliseconds(8), backoff);
  EXPECT_LE(std::chrono::milliseconds(0), backoff);
  EXPECT_TRUE(throttle.isActive());
  EXPECT_TRUE(timeout->isScheduled());
  throttle.cancel();
  timeout->cancelTimeout();
}

} // namespace openr

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  testing::InitGoogleMock(&argc, argv);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  auto rc = RUN_ALL_TESTS();

  // Run the tests
  return rc;
}
