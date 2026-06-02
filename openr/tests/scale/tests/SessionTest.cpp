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

TEST(SessionTest, DutNotResolvedBeforeStart) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  EXPECT_TRUE(s.dutNodeName().empty())
      << "dutNodeName should not be resolved before start()";
}

TEST(SessionTest, StartPatchesDutIntoTopology) {
  const char* host = std::getenv("OPENR_FAKE_DUT_HOST");
  if (!host) {
    GTEST_SKIP() << "OPENR_FAKE_DUT_HOST not set";
  }
  auto cfg = test::MakeTestConfig();
  cfg.dut()->host() = host;
  Session s(cfg, 0);
  const size_t preRouterCount = s.topology().routers.size();
  s.start();
  // For dutRole=SPINE the patch is a pure +1 router (the DUT itself).
  // listNodesUnlocked excludes the DUT from the public listing.
  EXPECT_FALSE(s.dutNodeName().empty());
  EXPECT_GT(s.topology().routers.count(s.dutNodeName()), 0u)
      << "DUT node not found in topology after start()";
  EXPECT_EQ(s.topology().routers.size(), preRouterCount + 1)
      << "expected +1 router (the DUT) for dutRole=SPINE";
  auto names = s.listNodesUnlocked();
  EXPECT_TRUE(
      std::find(names.begin(), names.end(), s.dutNodeName()) == names.end())
      << "listNodesUnlocked should exclude the DUT";
}

TEST(SessionTest, ConstructorDoesNotCrashWhenFakeKvStoreEnabled) {
  // Verify the FakeKvStoreManager member is constructed cleanly when
  // enableFakeKvStore=true. The kvManager_ ctor is gated on BOTH
  // enableFakeKvStore AND simulateNeighbors (matches legacy behavior),
  // so both must be true to exercise the construction path.
  auto cfg = test::MakeTestConfig();
  cfg.injection()->enableFakeKvStore() = true;
  cfg.injection()->simulateNeighbors() = true;
  cfg.injection()->fakeKvStoreBasePort() = 26300;
  Session s(cfg, 0);
  EXPECT_NO_THROW({ (void)s.listNodesUnlocked(); });
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
