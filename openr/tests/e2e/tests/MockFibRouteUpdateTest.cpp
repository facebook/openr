/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/FibServiceAsyncClient.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/tests/e2e/TestPlatform.h>

namespace openr::e2e {
namespace {

TEST(MockFibRouteUpdateTest, ReplacesNextHopsForAnExistingPrefix) {
  messaging::ReplicateQueue<fbnl::NetlinkEvent> netlinkEvents;
  TestPlatform platform("dut", netlinkEvents);
  platform.start();

  folly::EventBase eventBase;
  auto socket = folly::AsyncSocket::newSocket(
      &eventBase, "::1", platform.fibPort(), 1000);
  auto channel =
      apache::thrift::RocketClientChannel::newChannel(std::move(socket));
  apache::thrift::Client<thrift::FibService> client(std::move(channel));

  constexpr auto kFibClient = static_cast<int16_t>(thrift::FibClient::OPENR);
  const auto prefix = folly::IPAddress::createNetwork("2001:db8:30::/64");
  const auto makeRoute = [&](const std::string& interfaceName,
                             const std::string& address) {
    thrift::NextHopThrift nextHop;
    nextHop.address() = toBinaryAddress(folly::IPAddress{address});
    nextHop.address()->ifName() = interfaceName;

    thrift::UnicastRoute route;
    route.dest() = toIpPrefix(prefix);
    route.nextHops() = std::vector<thrift::NextHopThrift>{std::move(nextHop)};
    return route;
  };

  client.sync_addUnicastRoutes(
      kFibClient, {makeRoute("e2e-dut-1", "fe80::201")});
  std::vector<thrift::UnicastRoute> accepted;
  client.sync_getRouteTableByClient(accepted, kFibClient);
  ASSERT_EQ(1, accepted.size());
  ASSERT_EQ(1, accepted.front().nextHops()->size());
  EXPECT_EQ(
      folly::IPAddress{"fe80::201"},
      toIPAddress(*accepted.front().nextHops()->front().address()));

  client.sync_addUnicastRoutes(
      kFibClient, {makeRoute("e2e-dut-2", "fe80::202")});
  client.sync_getRouteTableByClient(accepted, kFibClient);
  ASSERT_EQ(1, accepted.size());
  EXPECT_EQ(prefix, toIPNetwork(*accepted.front().dest()));
  ASSERT_EQ(1, accepted.front().nextHops()->size());
  EXPECT_EQ(
      folly::IPAddress{"fe80::202"},
      toIPAddress(*accepted.front().nextHops()->front().address()));
  EXPECT_EQ(
      "e2e-dut-2",
      accepted.front().nextHops()->front().address()->ifName().value_or(""));

  netlinkEvents.close();
  platform.stop();
}

} // namespace
} // namespace openr::e2e
