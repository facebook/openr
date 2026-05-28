/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <openr/tests/scale/Session.h>
#include <openr/tests/scale/tests/SessionTestUtil.h>

namespace openr {

TEST(SessionTest, ConstructorPopulatesTopology) {
  auto cfg = test::MakeTestConfig();
  Session session(cfg, /*basePortOverride=*/0);
  // start() not called; verify the ctor populated the topology and
  // listNodesUnlocked() returns a non-empty set of node names.
  auto names = session.listNodesUnlocked();
  EXPECT_FALSE(names.empty());
}

TEST(SessionTest, StartThrowsDutUnreachable) {
  thrift::ScaleTestConfig cfg = test::MakeTestConfig();
  // Use a port that reliably refuses connection so injector_->connect() fails.
  cfg.dut()->host() = "::1";
  cfg.dut()->port() = 1;
  Session s(cfg, /*basePortOverride=*/0);
  try {
    s.start();
    FAIL() << "expected SetupError";
  } catch (const thrift::SetupError& e) {
    EXPECT_EQ(*e.reason(), thrift::SetupErrorReason::DUT_UNREACHABLE);
  }
}

} // namespace openr
