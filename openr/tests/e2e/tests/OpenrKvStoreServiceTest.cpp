/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <system_error>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <gtest/gtest.h>
#include <openr/tests/e2e/OpenrKvStoreService.h>

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

} // namespace
} // namespace openr::e2e
