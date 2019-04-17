/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sys/types.h>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <openr/common/Constants.h>
#include <openr/fbmeshd/openr-metric-manager/OpenRMetricManager.h>

using namespace openr::fbmeshd;

const std::string linkMonitorGlobalCmdUrl{folly::sformat(
    "tcp://localhost:{}", openr::Constants::kLinkMonitorCmdPort)};
const openr::MonitorSubmitUrl monitorSubmitUrl{
    folly::sformat("tcp://localhost:{}", openr::Constants::kMonitorRepPort)};
const folly::MacAddress macA{"11:22:33:44:55:66"};
const folly::MacAddress macB{"11:22:33:44:55:22"};
const folly::MacAddress macC{"11:22:33:44:55:44"};

// The fixture for testing class OpenRMetricManager class.
class OpenRMetricManagerTest : public ::testing::Test {
 protected:
  fbzmq::Context zmqContext_;
  fbzmq::ZmqEventLoop zmqLoop_;

  // nlHandler is not initialized to avoid netlink socket
  // initializetion, which requires root priviledges
  //
  // We just define it to have a pointer to pass to OpenRMetricManager
  // constructor, but this means that we cannot test any functions in
  // OpenRMetricManager that require a functional nlHandler
  //
  // TODO: Write a mock class for nlHandler so we can test those functions
  openr::fbmeshd::Nl80211Handler* nlHandler_{nullptr};
  OpenRMetricManager openRMetricManager_{zmqLoop_,
                                         nlHandler_,
                                         "mesh0",
                                         linkMonitorGlobalCmdUrl,
                                         monitorSubmitUrl,
                                         zmqContext_};
};

TEST_F(OpenRMetricManagerTest, SetGetMetricOverride) {
  openRMetricManager_.setMetricOverride(macA, 99);
  openRMetricManager_.setMetricOverride(macA, 33);
  openRMetricManager_.setMetricOverride(macB, 11);
  openRMetricManager_.setMetricOverride(macC, 22);

  ASSERT_EQ(openRMetricManager_.getMetricOverride(macA), 33);
  ASSERT_EQ(openRMetricManager_.getMetricOverride(macC), 22);
  ASSERT_EQ(openRMetricManager_.getMetricOverride(macB), 11);
}

TEST_F(OpenRMetricManagerTest, ClearMetricOverride) {
  openRMetricManager_.setMetricOverride(macA, 33);
  openRMetricManager_.setMetricOverride(macB, 11);
  openRMetricManager_.setMetricOverride(macC, 22);

  ASSERT_EQ(openRMetricManager_.clearMetricOverride(macA), 1);
  ASSERT_EQ(openRMetricManager_.clearMetricOverride(macA), 0);

  ASSERT_EQ(openRMetricManager_.getMetricOverride(macA), 0);
  ASSERT_EQ(openRMetricManager_.getMetricOverride(macC), 22);
  ASSERT_EQ(openRMetricManager_.getMetricOverride(macB), 11);

  ASSERT_EQ(openRMetricManager_.clearMetricOverride(), 2);
  ASSERT_EQ(openRMetricManager_.getMetricOverride(macC), 0);
  ASSERT_EQ(openRMetricManager_.getMetricOverride(macB), 0);
}

int
main(int argc, char** argv) {
  ::google::InitGoogleLogging(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
