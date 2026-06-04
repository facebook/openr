/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
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

TEST(SessionTest, SimulateNeighborsRequiresInterfaces) {
  auto cfg = test::MakeTestConfig();
  cfg.injection()->simulateNeighbors() = true;
  cfg.injection()->interfaces() = {}; // empty — should fail validation
  Session s(cfg, 0);
  EXPECT_THROW(s.start(), thrift::SetupError);
}

TEST(SessionTest, GetStatusReportsConfigAndNotConnected) {
  // Pre-start: no DUT connection, no spark, no downed state.
  auto cfg = test::MakeTestConfig();
  Session s(cfg, 0);
  auto st = s.getStatus();
  EXPECT_TRUE(*st.running());
  EXPECT_FALSE(*st.dutConnected());
  EXPECT_TRUE(st.downedNodes()->empty());
  EXPECT_TRUE(st.downedLinks()->empty());
  ASSERT_TRUE(st.activeConfig().has_value());
  EXPECT_EQ(*st.activeConfig(), cfg);
  ASSERT_TRUE(st.elapsedSec().has_value());
  EXPECT_GE(*st.elapsedSec(), 0);
  // MakeTestConfig sets simulateNeighbors=false, so per the TestStatus IDL the
  // optional neighborCount must be unset (distinct from a present 0).
  EXPECT_FALSE(st.neighborCount().has_value());
}

TEST(SessionTest, NeighborStatsZeroedWhenNoSimulation) {
  // MakeTestConfig sets simulateNeighbors=false, so sparkFaker_ is null and
  // getNeighborStats returns an all-zero / empty struct.
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto stats = s.getNeighborStats();
  EXPECT_EQ(*stats.totalNeighbors(), 0);
  EXPECT_EQ(*stats.neighborsEstablished(), 0);
  EXPECT_EQ(*stats.hellosSent(), 0);
  EXPECT_TRUE(stats.neighbors()->empty());
}

TEST(SessionTest, BuildFakeKeyValsVersionsAllRouters) {
  // The periodic fake-key bump regenerates numFakeKeysPerNode keys for every
  // router at a new version. Assert the payload shape without needing sinks.
  auto cfg = test::MakeTestConfig();
  cfg.injection()->numFakeKeysPerNode() = 3;
  Session s(cfg, /*basePortOverride=*/0);
  const auto& routers = s.topology().routers;
  ASSERT_FALSE(routers.empty());

  auto kv = s.buildFakeKeyVals(/*version=*/7);
  EXPECT_EQ(kv.size(), 3 * routers.size());

  // Spot-check naming + version for a known router (keys are
  // fakekeys{i}:{node}).
  const auto& nodeName = routers.begin()->first;
  for (int i = 0; i < 3; ++i) {
    const auto key = "fakekeys" + std::to_string(i) + ":" + nodeName;
    auto it = kv.find(key);
    ASSERT_NE(it, kv.end()) << "missing " << key;
    EXPECT_EQ(*it->second.version(), 7);
  }
}

TEST(SessionTest, BuildFakeKeyValsEmptyWhenDisabled) {
  // numFakeKeysPerNode unset (MakeTestConfig default) -> bump is a no-op.
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  EXPECT_TRUE(s.buildFakeKeyVals(/*version=*/5).empty());
}

TEST(SessionTest, VerifyRoutesReturnsZeroWhenDutNotConnected) {
  // start() not called, so injector_ is null and verifyRoutes reports zero
  // counts rather than dereferencing a missing DUT channel.
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto rc = s.verifyRoutes();
  EXPECT_EQ(*rc.unicastRoutes(), 0);
  EXPECT_EQ(*rc.mplsRoutes(), 0);
}

TEST(SessionTest, DownNodeRejectsUnknown) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  EXPECT_THROW(s.downNode("nonexistent-node"), thrift::UnknownNodeError);
}

TEST(SessionTest, UpNodeRejectsNotDowned) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  // Pick a real node that exists but was never downed.
  auto names = s.listNodesUnlocked();
  ASSERT_FALSE(names.empty());
  EXPECT_THROW(s.upNode(names.front()), thrift::UnknownNodeError);
}

TEST(SessionTest, DownNodeMarksAsDowned) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto names = s.listNodesUnlocked();
  ASSERT_FALSE(names.empty());
  const auto& nodeName = names.front();
  s.downNode(nodeName);
  auto st = s.getStatus();
  EXPECT_THAT(*st.downedNodes(), ::testing::Contains(nodeName));
}

TEST(SessionTest, DownNodeRejectsAlreadyDowned) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto names = s.listNodesUnlocked();
  ASSERT_FALSE(names.empty());
  const auto& nodeName = names.front();
  s.downNode(nodeName);
  EXPECT_THROW(s.downNode(nodeName), thrift::UnknownNodeError);
}

TEST(SessionTest, DownUpNodeRoundtripsState) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto names = s.listNodesUnlocked();
  ASSERT_FALSE(names.empty());
  const auto& nodeName = names.front();
  s.downNode(nodeName);
  s.upNode(nodeName);
  auto st = s.getStatus();
  EXPECT_TRUE(st.downedNodes()->empty());
}

TEST(SessionTest, DownUpDownFlapsStayConsistent) {
  /*
   * Repeated down/up/down flaps must keep downedNodes_ consistent and the
   * second down (after an up) must succeed. Regression guard for the down/up
   * version-stream alignment on the fake-KvStore removal path.
   */
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto names = s.listNodesUnlocked();
  ASSERT_FALSE(names.empty());
  const auto& nodeName = names.front();

  s.downNode(nodeName);
  s.upNode(nodeName);
  EXPECT_NO_THROW(s.downNode(nodeName));
  EXPECT_THAT(*s.getStatus().downedNodes(), ::testing::Contains(nodeName));
}

TEST(SessionTest, DownLinkRecordsInDownedLinks) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto [a, b] = test::FindAdjacentPair(s);
  s.downLink(a, b);
  auto st = s.getStatus();
  // Link endpoints are normalized (sorted) before storage; expect the
  // smaller name first.
  thrift::LinkRef expected;
  expected.localNode() = std::min(a, b);
  expected.remoteNode() = std::max(a, b);
  EXPECT_THAT(*st.downedLinks(), ::testing::Contains(expected));
}

TEST(SessionTest, DownLinkUpLinkRoundtripsState) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto [a, b] = test::FindAdjacentPair(s);
  s.downLink(a, b);
  s.upLink(a, b);
  auto st = s.getStatus();
  EXPECT_TRUE(st.downedLinks()->empty());
}

TEST(SessionTest, DownNodesMarksAllDown) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto names = s.listNodesUnlocked();
  ASSERT_GE(names.size(), 2u);
  s.downNodes({names[0], names[1]});
  auto st = s.getStatus();
  EXPECT_THAT(*st.downedNodes(), ::testing::Contains(names[0]));
  EXPECT_THAT(*st.downedNodes(), ::testing::Contains(names[1]));
}

TEST(SessionTest, DownNodesRejectsWholeBatchAtomically) {
  // One valid + one invalid name: the entire batch is rejected up front and
  // nothing is marked down (atomic validation).
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto names = s.listNodesUnlocked();
  ASSERT_FALSE(names.empty());
  EXPECT_THROW(
      s.downNodes({names[0], "nonexistent-node"}), thrift::UnknownNodeError);
  EXPECT_TRUE(s.getStatus().downedNodes()->empty());
}

TEST(SessionTest, DownUpNodesRoundtripsState) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto names = s.listNodesUnlocked();
  ASSERT_GE(names.size(), 2u);
  s.downNodes({names[0], names[1]});
  s.upNodes({names[0], names[1]});
  EXPECT_TRUE(s.getStatus().downedNodes()->empty());
}

TEST(SessionTest, DownLinksRecordsAll) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto [a, b] = test::FindAdjacentPair(s);
  // Find a second distinct neighbor c of a for a second link a<->c.
  std::string c;
  for (const auto& adj : s.topology().getRouter(a).adjacencies) {
    if (adj.remoteRouterName != b &&
        s.topology().routers.count(adj.remoteRouterName) > 0) {
      c = adj.remoteRouterName;
      break;
    }
  }
  ASSERT_FALSE(c.empty()) << "test needs a node with >= 2 distinct neighbors";

  thrift::LinkRef l1;
  l1.localNode() = a;
  l1.remoteNode() = b;
  thrift::LinkRef l2;
  l2.localNode() = a;
  l2.remoteNode() = c;
  s.downLinks({l1, l2});

  auto st = s.getStatus();
  thrift::LinkRef e1;
  e1.localNode() = std::min(a, b);
  e1.remoteNode() = std::max(a, b);
  thrift::LinkRef e2;
  e2.localNode() = std::min(a, c);
  e2.remoteNode() = std::max(a, c);
  EXPECT_THAT(*st.downedLinks(), ::testing::Contains(e1));
  EXPECT_THAT(*st.downedLinks(), ::testing::Contains(e2));
}

TEST(SessionTest, DownLinksRejectsWholeBatchAtomically) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto [a, b] = test::FindAdjacentPair(s);
  thrift::LinkRef good;
  good.localNode() = a;
  good.remoteNode() = b;
  thrift::LinkRef bad;
  bad.localNode() = a;
  bad.remoteNode() = "nonexistent-node";
  EXPECT_THROW(s.downLinks({good, bad}), thrift::UnknownNodeError);
  EXPECT_TRUE(s.getStatus().downedLinks()->empty());
}

TEST(SessionTest, DownUpLinksRoundtripsState) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto [a, b] = test::FindAdjacentPair(s);
  thrift::LinkRef l;
  l.localNode() = a;
  l.remoteNode() = b;
  s.downLinks({l});
  s.upLinks({l});
  EXPECT_TRUE(s.getStatus().downedLinks()->empty());
}

TEST(SessionTest, DownLinkRejectsIfEndpointAlreadyDowned) {
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto [a, b] = test::FindAdjacentPair(s);
  s.downNode(a);
  EXPECT_THROW(s.downLink(a, b), thrift::UnknownAdjacencyError);
}

TEST(SessionTest, NodeFlapPreservesOperatorDownedLink) {
  // Operator downLink() intent persists across a node flap: downing then
  // restoring an endpoint must NOT silently bring an operator-downed link back.
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto [a, b] = test::FindAdjacentPair(s);
  thrift::LinkRef expected;
  expected.localNode() = std::min(a, b);
  expected.remoteNode() = std::max(a, b);

  s.downLink(a, b);
  s.downNode(a);
  // While endpoint a is down, the incident link is subsumed by the downed node
  // and hidden from getStatus() (per the TestStatus contract), though the
  // intent is retained internally.
  EXPECT_THAT(
      *s.getStatus().downedLinks(),
      ::testing::Not(::testing::Contains(expected)));
  EXPECT_THAT(*s.getStatus().downedNodes(), ::testing::Contains(a));

  s.upNode(a);
  // Node is back up, but the operator-downed link must still be reported down.
  auto st = s.getStatus();
  EXPECT_TRUE(st.downedNodes()->empty());
  EXPECT_THAT(*st.downedLinks(), ::testing::Contains(expected));
}

TEST(SessionTest, MultiLinkNodeFlapPreservesNeighborsOtherDownedLink) {
  // Multi-link case: with edges a-b and b-c, downLink(b, c) then a flap of node
  // a must NOT clear (b, c) — node a's down/up rebuilds neighbor b, and b must
  // keep its other operator-downed link (to c) omitted, not silently restored.
  auto cfg = test::MakeTestConfig();
  Session s(cfg, /*basePortOverride=*/0);
  auto [a, b] = test::FindAdjacentPair(s);
  // Find c: a neighbor of b distinct from a.
  std::string c;
  for (const auto& adj : s.topology().getRouter(b).adjacencies) {
    if (adj.remoteRouterName != a &&
        s.topology().routers.count(adj.remoteRouterName) > 0) {
      c = adj.remoteRouterName;
      break;
    }
  }
  ASSERT_FALSE(c.empty()) << "test needs a node b with >= 2 distinct neighbors";

  s.downLink(b, c);
  s.downNode(a);
  s.upNode(a);

  thrift::LinkRef bc;
  bc.localNode() = std::min(b, c);
  bc.remoteNode() = std::max(b, c);
  auto st = s.getStatus();
  EXPECT_TRUE(st.downedNodes()->empty());
  EXPECT_THAT(*st.downedLinks(), ::testing::Contains(bc));
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
