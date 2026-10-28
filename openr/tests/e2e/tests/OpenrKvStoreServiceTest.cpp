/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <gtest/gtest.h>
#include <openr/tests/e2e/OpenrKvStoreService.h>
#include <openr/tests/e2e/SyntheticPeer.h>
#include <openr/tests/mocks/MockIoProvider.h>

namespace openr::e2e {
namespace {

TEST(OpenrKvStoreServiceTest, ReservedPortRejectsAnotherServer) {
  OpenrKvStoreService service;
  folly::AsyncServerSocket::UniquePtr other{new folly::AsyncServerSocket()};

  EXPECT_THROW(
      other->bind(
          std::vector<folly::IPAddress>{folly::IPAddress{"::1"}},
          service.port()),
      std::system_error);
}

TEST(SyntheticPeerTest, ReservedPortRejectsAnotherServer) {
  OpenrKvStoreService dut;
  SyntheticPeerConfig config;
  config.nodeName = "dut";
  config.peerName = "spine";
  config.dutIfName = "dut-if";
  config.peerIfName = "peer-if";
  config.dutIfIndex = 101;
  config.peerIfIndex = 102;
  config.dutV6Address = "fe80::1";
  config.peerV6Address = "fe80::2";
  config.dutKvStorePort = dut.port();
  config.ioProvider = std::make_shared<MockIoProvider>();
  SyntheticPeer peer{std::move(config)};
  folly::AsyncServerSocket::UniquePtr other{new folly::AsyncServerSocket()};

  EXPECT_THROW(
      other->bind(
          std::vector<folly::IPAddress>{folly::IPAddress{"::1"}},
          peer.kvStorePort()),
      std::system_error);
}

} // namespace
} // namespace openr::e2e
