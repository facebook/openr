/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/coro/GtestHelpers.h>
#include <gtest/gtest.h>

#include <openr/tests/utils/Utils.h>

namespace openr {

CO_TEST(UtilsCoroTest, EmptyStoresConverge) {
  folly::F14FastMap<std::string, thrift::Value> events;
  std::vector<std::unique_ptr<
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>>>
      stores;

  CO_ASSERT_NO_THROW(co_await co_waitForConvergence(events, stores));
}

} // namespace openr
