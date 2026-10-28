/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <stdexcept>

#include <openr/config/Config.h>
#include <openr/tests/e2e/OpenrE2eConfig.h>

namespace openr::e2e {
namespace {

TEST(OpenrE2eConfigTest, UsesExternalServicePortsAndSingleArea) {
  const auto config =
      makeOpenrE2eConfig("router", 42001, 42002, "/tmp/router-test.bin");

  EXPECT_FALSE(*config.dryrun());
  EXPECT_FALSE(*config.enable_v4());
  EXPECT_EQ(42001, *config.fib_port());
  EXPECT_EQ(42002, *config.thrift_server()->kvstore_peer_port());
  ASSERT_EQ(1, config.areas()->size());
  EXPECT_EQ("0", *config.areas()->at(0).area_id());
  EXPECT_FALSE(*config.enable_watchdog());
  EXPECT_EQ(42002, Config(config).getKvStorePeerPort());
}

TEST(OpenrE2eConfigTest, RejectsMissingExternalBoundary) {
  EXPECT_THROW(
      makeOpenrE2eConfig("", 42001, 42002, "/tmp/state"),
      std::invalid_argument);
  EXPECT_THROW(
      makeOpenrE2eConfig("router", 0, 42002, "/tmp/state"),
      std::invalid_argument);
  EXPECT_THROW(
      makeOpenrE2eConfig("router", 42001, 0, "/tmp/state"),
      std::invalid_argument);
  EXPECT_THROW(
      makeOpenrE2eConfig("router", 42001, 42002, ""), std::invalid_argument);
}

TEST(OpenrE2eConfigTest, PreservesAreaSpecificNeighborAndInterfacePatterns) {
  thrift::AreaConfig pod;
  pod.area_id() = "pod";
  pod.include_interface_regexes() = {"^if_r1_s1$"};
  pod.neighbor_regexes() = {"^s1$"};
  thrift::AreaConfig plane;
  plane.area_id() = "plane";
  plane.include_interface_regexes() = {"^if_r1_s2$"};
  plane.neighbor_regexes() = {"^s2$"};

  const auto config =
      makeOpenrE2eConfig("r1", 42001, 42002, "/tmp/r1-test.bin", {pod, plane});

  ASSERT_EQ(2, config.areas()->size());
  EXPECT_EQ("pod", *config.areas()->at(0).area_id());
  EXPECT_EQ(
      std::vector<std::string>{"^if_r1_s1$"},
      *config.areas()->at(0).include_interface_regexes());
  EXPECT_EQ(
      std::vector<std::string>{"^s2$"},
      *config.areas()->at(1).neighbor_regexes());
}

} // namespace
} // namespace openr::e2e
