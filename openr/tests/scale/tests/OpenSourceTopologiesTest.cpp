/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <gtest/gtest.h>

#include <openr/tests/scale/TopologyFactory.h>
#include <openr/tests/scale/TopologyGenerator.h>

/*
 * Exercises the open-source createScaleTopology() seam
 * (OpenSourceTopologies.cpp): the generic topology types (fabric/ring/grid)
 * build, and any other type is rejected with a SetupError.
 */

namespace openr {

namespace {

thrift::ScaleTestConfig
makeConfig(const std::string& type) {
  thrift::ScaleTestConfig cfg;
  cfg.topology()->type() = type;
  return cfg;
}

} // namespace

TEST(OpenSourceTopologiesTest, RingBuildsRequestedNumberOfRouters) {
  auto cfg = makeConfig("ring");
  cfg.topology()->numSpines() = 6;

  auto topology = createScaleTopology(cfg);

  EXPECT_EQ(topology.getRouterCount(), 6);
}

TEST(OpenSourceTopologiesTest, GridBuildsNbyNRouters) {
  auto cfg = makeConfig("grid");
  cfg.topology()->numSpines() = 4;

  auto topology = createScaleTopology(cfg);

  // createGrid(n) builds an n x n mesh.
  EXPECT_EQ(topology.getRouterCount(), 16);
}

TEST(OpenSourceTopologiesTest, FabricBuildsExpectedRouterCount) {
  auto cfg = makeConfig("fabric");
  cfg.topology()->numPods() = 2; // numPods
  cfg.topology()->numSpines() = 2; // numPlanes
  cfg.topology()->numSuperSpines() = 3; // numSswsPerPlane
  cfg.topology()->numLeaves() = 4; // numRswsPerPod

  auto topology = createScaleTopology(cfg);

  const size_t expectedRouters = TopologyGenerator::calculateFabricRouterCount(
      /*numPods=*/2,
      /*numPlanes=*/2,
      /*numSswsPerPlane=*/3,
      /*numRswsPerPod=*/4);
  EXPECT_EQ(topology.getRouterCount(), expectedRouters);
}

TEST(OpenSourceTopologiesTest, FabricMissingRequiredFieldThrows) {
  // numPods is required for fabric; leaving it unset must fail validation.
  auto cfg = makeConfig("fabric");
  cfg.topology()->numSpines() = 2;

  EXPECT_THROW(createScaleTopology(cfg), thrift::SetupError);
}

TEST(OpenSourceTopologiesTest, UnsupportedTypeThrows) {
  // A type this build does not support must be rejected, not silently built.
  auto cfg = makeConfig("not-a-generic-type");
  cfg.topology()->numSpines() = 4;

  EXPECT_THROW(createScaleTopology(cfg), thrift::SetupError);
}

TEST(OpenSourceTopologiesTest, FromParamsRingBuilds) {
  ScaleTopologyParams params;
  params.type = "ring";
  params.numSpines = 5;

  auto topology = createScaleTopologyFromParams(params);

  ASSERT_TRUE(topology.has_value());
  EXPECT_EQ(topology->getRouterCount(), 5);
}

TEST(OpenSourceTopologiesTest, FromParamsUnsupportedReturnsNullopt) {
  ScaleTopologyParams params;
  params.type = "not-a-generic-type";

  EXPECT_FALSE(createScaleTopologyFromParams(params).has_value());
}

} // namespace openr

int
main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv);
  return RUN_ALL_TESTS();
}
