/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/tests/SessionTestUtil.h>

namespace openr::test {

namespace {

constexpr int kTestDutPort = 2018;
constexpr int kTestNumSpines = 4;
constexpr int kTestNumLeaves = 8;
constexpr int kTestNumPods = 2;

} // namespace

thrift::ScaleTestConfig
MakeTestConfig() {
  thrift::ScaleTestConfig cfg;
  cfg.dut()->host() = "::1";
  cfg.dut()->port() = kTestDutPort;
  cfg.injection()->injectTopology() = false;
  cfg.injection()->simulateNeighbors() = false;
  cfg.injection()->enableFakeKvStore() = false;
  cfg.topology()->numSpines() = kTestNumSpines;
  cfg.topology()->numLeaves() = kTestNumLeaves;
  cfg.topology()->numSuperSpines() = 0;
  cfg.topology()->numPods() = kTestNumPods;
  cfg.topology()->numSites() = 0;
  cfg.topology()->numPrefixesPerNode() = 1;
  cfg.topology()->dutRole() = thrift::DutRole::SPINE;
  cfg.topology()->ecmpWidth() = 2;
  return cfg;
}

} // namespace openr::test
