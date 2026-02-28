/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {

/*
 * Test the BBF (Backbone Fabric) topology generation.
 * BBF is Meta's disaggregated backbone with massive ECMP.
 */
class BbfTopologyTest : public ::testing::Test {
 protected:
  void
  SetUp() override {
    LOG(INFO) << "BbfTopologyTest::SetUp()";
  }

  void
  TearDown() override {
    LOG(INFO) << "BbfTopologyTest::TearDown()";
  }
};

/*
 * Test small BBF topology creation.
 */
TEST_F(BbfTopologyTest, SmallBbf) {
  /*
   * Small BBF: 2 pods, 2 planes, 4 spines/plane, 8 leaves/pod
   * Total: 24 routers (8 spines + 16 leaves)
   */
  constexpr int kNumPods = 2;
  constexpr int kNumPlanes = 2;
  constexpr int kSpinesPerPlane = 4;
  constexpr int kLeavesPerPod = 8;
  constexpr int kEcmpWidth = 4;

  auto expectedRouters = TopologyGenerator::calculateBbfRouterCount(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod);

  EXPECT_EQ(expectedRouters, 24);

  auto topo = TopologyGenerator::createBbf(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth, 5);

  EXPECT_EQ(topo.getRouterCount(), 24);

  /*
   * Calculate expected adjacencies:
   * Each leaf connects to all spines with ecmpWidth links
   * Total links = spines * leaves * ecmpWidth
   * Adjacencies = links * 2 (bidirectional)
   */
  auto expectedAdjs = TopologyGenerator::calculateBbfAdjacencyCount(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth);

  EXPECT_EQ(expectedAdjs, 8 * 16 * 4 * 2); // 1024 adjacencies
  EXPECT_EQ(topo.getTotalAdjacencyCount(), expectedAdjs);

  /*
   * Verify spine and leaf naming
   */
  EXPECT_TRUE(topo.routers.count("spine-0-0") > 0);
  EXPECT_TRUE(topo.routers.count("spine-1-3") > 0);
  EXPECT_TRUE(topo.routers.count("leaf-0-0") > 0);
  EXPECT_TRUE(topo.routers.count("leaf-1-7") > 0);
}

/*
 * Test medium BBF topology (300+ nodes).
 */
TEST_F(BbfTopologyTest, MediumBbf) {
  /*
   * Medium BBF: 4 pods, 4 planes, 16 spines/plane, 16 leaves/pod
   * Total: 128 routers (64 spines + 64 leaves)
   * With 8 ECMP links per spine-leaf pair
   */
  constexpr int kNumPods = 4;
  constexpr int kNumPlanes = 4;
  constexpr int kSpinesPerPlane = 16;
  constexpr int kLeavesPerPod = 16;
  constexpr int kEcmpWidth = 8;

  auto expectedRouters = TopologyGenerator::calculateBbfRouterCount(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod);

  EXPECT_EQ(expectedRouters, 128);

  auto topo = TopologyGenerator::createBbf(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth, 10);

  EXPECT_EQ(topo.getRouterCount(), 128);

  /*
   * Adjacencies = 64 spines * 64 leaves * 8 ECMP * 2 directions = 65,536
   */
  auto expectedAdjs = TopologyGenerator::calculateBbfAdjacencyCount(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth);

  EXPECT_EQ(expectedAdjs, 65536);
  EXPECT_EQ(topo.getTotalAdjacencyCount(), expectedAdjs);

  LOG(INFO) << "Medium BBF: " << topo.getRouterCount() << " routers, "
            << topo.getTotalAdjacencyCount() << " adjacencies";
}

/*
 * Test large BBF topology (1000+ nodes).
 */
TEST_F(BbfTopologyTest, LargeBbf) {
  /*
   * Large BBF: 8 pods, 8 planes, 32 spines/plane, 32 leaves/pod
   * Total: 512 routers (256 spines + 256 leaves)
   * With 16 ECMP links per spine-leaf pair
   */
  constexpr int kNumPods = 8;
  constexpr int kNumPlanes = 8;
  constexpr int kSpinesPerPlane = 32;
  constexpr int kLeavesPerPod = 32;
  constexpr int kEcmpWidth = 16;

  auto expectedRouters = TopologyGenerator::calculateBbfRouterCount(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod);

  EXPECT_EQ(expectedRouters, 512);

  auto topo = TopologyGenerator::createBbf(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth, 20);

  EXPECT_EQ(topo.getRouterCount(), 512);

  /*
   * Adjacencies = 256 spines * 256 leaves * 16 ECMP * 2 directions = 2,097,152
   */
  auto expectedAdjs = TopologyGenerator::calculateBbfAdjacencyCount(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth);

  EXPECT_EQ(expectedAdjs, 2097152);
  EXPECT_EQ(topo.getTotalAdjacencyCount(), expectedAdjs);

  LOG(INFO) << "Large BBF: " << topo.getRouterCount() << " routers, "
            << topo.getTotalAdjacencyCount() << " adjacencies (2M+)";
}

/*
 * Test BBF with asymmetric ECMP (different width per tier).
 */
TEST_F(BbfTopologyTest, AsymmetricEcmp) {
  /*
   * Simulate different ECMP widths for testing
   */
  constexpr int kNumPods = 2;
  constexpr int kNumPlanes = 2;
  constexpr int kSpinesPerPlane = 4;
  constexpr int kLeavesPerPod = 4;

  /*
   * Test with different ECMP widths
   */
  for (int ecmpWidth : {1, 2, 4, 8, 16, 32}) {
    auto topo = TopologyGenerator::createBbf(
        kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, ecmpWidth, 5);

    auto expectedAdjs = TopologyGenerator::calculateBbfAdjacencyCount(
        kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, ecmpWidth);

    EXPECT_EQ(topo.getTotalAdjacencyCount(), expectedAdjs)
        << "Failed for ECMP width " << ecmpWidth;

    LOG(INFO) << "ECMP width " << ecmpWidth << ": "
              << topo.getTotalAdjacencyCount() << " adjacencies";
  }
}

/*
 * Test BBF topology structure validation.
 */
TEST_F(BbfTopologyTest, TopologyStructure) {
  constexpr int kNumPods = 2;
  constexpr int kNumPlanes = 2;
  constexpr int kSpinesPerPlane = 2;
  constexpr int kLeavesPerPod = 2;
  constexpr int kEcmpWidth = 2;

  auto topo = TopologyGenerator::createBbf(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth, 5);

  /*
   * Verify each leaf connects to all spines
   */
  for (int pod = 0; pod < kNumPods; ++pod) {
    for (int leaf = 0; leaf < kLeavesPerPod; ++leaf) {
      std::string leafName = fmt::format("leaf-{}-{}", pod, leaf);
      const auto& leafRouter = topo.routers.at(leafName);

      /*
       * Each leaf should have adjacencies to all spines
       * Total adjacencies = numSpines * ecmpWidth
       */
      int expectedAdjCount = kNumPlanes * kSpinesPerPlane * kEcmpWidth;
      EXPECT_EQ(leafRouter.adjacencies.size(), expectedAdjCount)
          << "Leaf " << leafName << " has wrong adjacency count";

      /*
       * Check connectivity to specific spines
       */
      for (int plane = 0; plane < kNumPlanes; ++plane) {
        for (int spine = 0; spine < kSpinesPerPlane; ++spine) {
          std::string spineName = fmt::format("spine-{}-{}", plane, spine);

          /*
           * Count adjacencies to this spine (should be ecmpWidth)
           */
          int adjToSpine = 0;
          for (const auto& adj : leafRouter.adjacencies) {
            if (adj.remoteRouterName == spineName) {
              adjToSpine++;
            }
          }

          EXPECT_EQ(adjToSpine, kEcmpWidth)
              << "Leaf " << leafName << " should have " << kEcmpWidth
              << " adjacencies to spine " << spineName;
        }
      }
    }
  }

  /*
   * Verify each spine connects to all leaves
   */
  for (int plane = 0; plane < kNumPlanes; ++plane) {
    for (int spine = 0; spine < kSpinesPerPlane; ++spine) {
      std::string spineName = fmt::format("spine-{}-{}", plane, spine);
      const auto& spineRouter = topo.routers.at(spineName);

      /*
       * Each spine should have adjacencies to all leaves
       * Total adjacencies = numLeaves * ecmpWidth
       */
      int expectedAdjCount = kNumPods * kLeavesPerPod * kEcmpWidth;
      EXPECT_EQ(spineRouter.adjacencies.size(), expectedAdjCount)
          << "Spine " << spineName << " has wrong adjacency count";
    }
  }
}

/*
 * Test production-scale BBF topology creation.
 * Real BBF: 252 leaves (4 pods × 63), 64 spines (4 planes × 16)
 */
TEST_F(BbfTopologyTest, ProductionScaleBbf) {
  /*
   * Real production BBF from design doc:
   * "252 leafs, 64 spines, 4 hosts"
   */
  constexpr int kNumPods = 4;
  constexpr int kNumPlanes = 4;
  constexpr int kSpinesPerPlane = 16;
  constexpr int kLeavesPerPod = 63;
  constexpr int kEcmpWidth = 8; /* Conservative ECMP for test speed */

  auto expectedRouters = TopologyGenerator::calculateBbfRouterCount(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod);

  EXPECT_EQ(expectedRouters, 316);

  auto topo = TopologyGenerator::createBbf(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth, 5);

  EXPECT_EQ(topo.getRouterCount(), 316);

  /*
   * Adjacencies = 64 spines * 252 leaves * 8 ECMP * 2 directions = 258,048
   */
  auto expectedAdjs = TopologyGenerator::calculateBbfAdjacencyCount(
      kNumPods, kNumPlanes, kSpinesPerPlane, kLeavesPerPod, kEcmpWidth);

  EXPECT_EQ(expectedAdjs, 258048);
  EXPECT_EQ(topo.getTotalAdjacencyCount(), expectedAdjs);

  LOG(INFO) << "Production BBF: " << topo.getRouterCount() << " routers, "
            << topo.getTotalAdjacencyCount() << " adjacencies (258K)";

  /*
   * Verify naming convention
   */
  EXPECT_TRUE(topo.routers.count("spine-0-0") > 0);
  EXPECT_TRUE(topo.routers.count("spine-3-15") > 0);
  EXPECT_TRUE(topo.routers.count("leaf-0-0") > 0);
  EXPECT_TRUE(topo.routers.count("leaf-3-62") > 0);
}

/*
 * Test BBF scale limits.
 * Real BBF: 252 leaves, 64 spines
 * 4x buffer: ~1000 leaves, ~256 spines
 */
TEST_F(BbfTopologyTest, ScaleLimits) {
  /*
   * Real production BBF: 252 leaves, 64 spines
   * From BBF design doc: "252 leafs, 64 spines, 4 hosts"
   */
  constexpr int kRealNumPods = 4; /* 4 pods of 63 leaves each = 252 */
  constexpr int kRealLeavesPerPod = 63;
  constexpr int kRealNumPlanes = 4; /* 4 planes of 16 spines each = 64 */
  constexpr int kRealSpinesPerPlane = 16;

  auto realRouters = TopologyGenerator::calculateBbfRouterCount(
      kRealNumPods, kRealNumPlanes, kRealSpinesPerPlane, kRealLeavesPerPod);

  /* Total routers = 64 spines + 252 leaves = 316 */
  EXPECT_EQ(realRouters, 316);

  LOG(INFO) << "Real BBF (252 leaves, 64 spines): " << realRouters
            << " routers";

  /*
   * 4x buffer for stress testing: ~1000 leaves, ~256 spines
   * 8 pods × 126 leaves = 1008 leaves
   * 8 planes × 32 spines = 256 spines
   * Total: 1264 routers
   */
  constexpr int k4xNumPods = 8;
  constexpr int k4xLeavesPerPod = 126;
  constexpr int k4xNumPlanes = 8;
  constexpr int k4xSpinesPerPlane = 32;
  constexpr int k4xEcmpWidth = 16; /* Realistic ECMP width */

  auto stressRouters = TopologyGenerator::calculateBbfRouterCount(
      k4xNumPods, k4xNumPlanes, k4xSpinesPerPlane, k4xLeavesPerPod);

  /* Total: 256 spines + 1008 leaves = 1264 routers */
  EXPECT_EQ(stressRouters, 1264);

  auto stressAdjs = TopologyGenerator::calculateBbfAdjacencyCount(
      k4xNumPods,
      k4xNumPlanes,
      k4xSpinesPerPlane,
      k4xLeavesPerPod,
      k4xEcmpWidth);

  /*
   * Adjacencies = 256 spines * 1008 leaves * 16 ECMP * 2 directions
   *             = 8,257,536 adjacencies (8M+)
   */
  EXPECT_EQ(stressAdjs, 8257536);

  LOG(INFO) << "4x stress BBF: " << stressRouters << " routers, " << stressAdjs
            << " adjacencies (8M+)";

  /*
   * Extreme scale for future proofing (2030+ requirements):
   * 16 pods × 252 leaves = 4032 leaves
   * 16 planes × 64 spines = 1024 spines
   * Total: 5056 routers with 32 ECMP links
   */
  auto extremeRouters =
      TopologyGenerator::calculateBbfRouterCount(16, 16, 64, 252);
  EXPECT_EQ(extremeRouters, 5056);

  auto extremeAdjs =
      TopologyGenerator::calculateBbfAdjacencyCount(16, 16, 64, 252, 32);

  /*
   * 1024 spines * 4032 leaves * 32 ECMP * 2 = 264,241,152 (264M adjacencies!)
   */
  EXPECT_EQ(extremeAdjs, 264241152);

  LOG(INFO) << "Extreme scale BBF (5k+ nodes): " << extremeRouters
            << " routers, " << extremeAdjs << " adjacencies (264M!)";
}

} // namespace openr

int
main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  return RUN_ALL_TESTS();
}
