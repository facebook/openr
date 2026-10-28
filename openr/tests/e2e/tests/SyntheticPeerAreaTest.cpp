/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <folly/io/async/AsyncSocket.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/tests/e2e/SyntheticPeer.h>
#include <openr/tests/mocks/MockIoProvider.h>

namespace openr::e2e {
namespace {

TEST(SyntheticPeerAreaTest, ServesSeededTopologyInItsArea) {
  SyntheticPeerConfig config;
  config.nodeName = "r1";
  config.peerName = "s1";
  config.dutIfName = "r1-s1";
  config.peerIfName = "s1-r1";
  config.dutIfIndex = 1;
  config.peerIfIndex = 2;
  config.dutV6Address = "fe80::1";
  config.peerV6Address = "fe80::2";
  config.dutKvStorePort = 42000;
  config.ioProvider = std::make_shared<MockIoProvider>();
  config.area = "pod";
  config.initialKeyVals =
      thrift::KeyVals{{"adj:s1", createThriftValue(1, "s1", "adjacency")}};
  SyntheticPeer peer{std::move(config)};
  peer.start();

  folly::EventBase eventBase;
  auto socket = folly::AsyncSocket::newSocket(
      &eventBase, "::1", peer.kvStorePort(), 5000);
  auto channel =
      apache::thrift::RocketClientChannel::newChannel(std::move(socket));
  apache::thrift::Client<thrift::KvStoreService> client{std::move(channel)};
  thrift::KeyDumpParams filter;
  thrift::Publication publication;
  client.sync_getKvStoreKeyValsFilteredArea(publication, filter, "pod");
  ASSERT_EQ(1, publication.keyVals()->size());
  EXPECT_EQ("adjacency", *publication.keyVals()->at("adj:s1").value());

  client.sync_getKvStoreKeyValsFilteredArea(publication, filter, "plane");
  EXPECT_TRUE(publication.keyVals()->empty());
}

} // namespace
} // namespace openr::e2e
