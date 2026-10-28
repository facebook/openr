/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <utility>

#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include <openr/if/gen-cpp2/FibServiceAsyncClient.h>
#include <openr/tests/e2e/TestPlatform.h>

namespace openr::e2e {
namespace {

TEST(TestPlatformTest, FibServiceRespondsThroughARealThriftSocket) {
  messaging::ReplicateQueue<fbnl::NetlinkEvent> netlinkEvents;
  TestPlatform platform("router", netlinkEvents);
  platform.start();

  folly::EventBase eventBase;
  auto socket = folly::AsyncSocket::newSocket(
      &eventBase, "::1", platform.fibPort(), 1000);
  auto channel =
      apache::thrift::RocketClientChannel::newChannel(std::move(socket));
  apache::thrift::Client<thrift::FibService> client(std::move(channel));

  EXPECT_FALSE(platform.waitForFibServiceKeepaliveCalls(
      2, std::chrono::milliseconds{0}));
  EXPECT_EQ(1, client.sync_aliveSince());
  EXPECT_FALSE(platform.waitForFibServiceKeepaliveCalls(
      2, std::chrono::milliseconds{0}));
  EXPECT_EQ(1, client.sync_aliveSince());
  EXPECT_TRUE(platform.waitForFibServiceKeepaliveCalls(
      2, std::chrono::milliseconds{0}));
  EXPECT_TRUE(platform.fibRoutes().empty());
  EXPECT_EQ(0, platform.fibUpdateCounts().addedRoutes);

  netlinkEvents.close();
  platform.stop();
}

TEST(TestPlatformTest, FibServiceOutageRestoresTheSamePort) {
  messaging::ReplicateQueue<fbnl::NetlinkEvent> netlinkEvents;
  TestPlatform platform("router", netlinkEvents);
  platform.start();

  const auto port = platform.fibPort();
  folly::EventBase eventBase;
  auto socket = folly::AsyncSocket::newSocket(&eventBase, "::1", port, 1000);
  auto channel =
      apache::thrift::RocketClientChannel::newChannel(std::move(socket));
  apache::thrift::Client<thrift::FibService> client(std::move(channel));
  EXPECT_EQ(1, client.sync_aliveSince());

  platform.setFibServiceAvailable(false);
  EXPECT_THROW(client.sync_aliveSince(), std::exception);
  EXPECT_EQ(port, platform.fibPort());

  platform.setFibServiceAvailable(true);
  auto restoredSocket =
      folly::AsyncSocket::newSocket(&eventBase, "::1", port, 1000);
  auto restoredChannel = apache::thrift::RocketClientChannel::newChannel(
      std::move(restoredSocket));
  apache::thrift::Client<thrift::FibService> restoredClient(
      std::move(restoredChannel));
  EXPECT_EQ(1, restoredClient.sync_aliveSince());

  netlinkEvents.close();
  platform.stop();
}

} // namespace
} // namespace openr::e2e
