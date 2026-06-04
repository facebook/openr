/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/tests/scale/ScaleTestServerHandler.h>
#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer.h>
#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer_types.h>

namespace openr {

// Spin the handler behind a ScopedServerInterfaceThread so tests exercise
// the real Thrift dispatch path (avoids facebook-thrift-handler-direct-call
// lint by calling generated client methods instead of sync_* directly).
class ScaleTestServerHandlerTest : public ::testing::Test {
 protected:
  void
  SetUp() override {
    auto handler = std::make_shared<ScaleTestServerHandler>();
    runner_ =
        std::make_unique<apache::thrift::ScopedServerInterfaceThread>(handler);
    client_ =
        runner_->newClient<apache::thrift::Client<thrift::ScaleTestServer>>();
  }

  std::unique_ptr<apache::thrift::ScopedServerInterfaceThread> runner_;
  std::unique_ptr<apache::thrift::Client<thrift::ScaleTestServer>> client_;
};

TEST_F(ScaleTestServerHandlerTest, StopBeforeStartThrows) {
  EXPECT_THROW(client_->sync_stopTest(), thrift::NotRunningError);
}

TEST_F(ScaleTestServerHandlerTest, ListNodesBeforeStartThrows) {
  std::vector<std::string> out;
  EXPECT_THROW(client_->sync_listNodes(out), thrift::NotRunningError);
}

TEST_F(ScaleTestServerHandlerTest, GetStatusBeforeStartReportsNotRunning) {
  thrift::TestStatus s;
  client_->sync_getTestStatus(s);
  EXPECT_FALSE(*s.running());
  EXPECT_FALSE(s.elapsedSec().has_value());
  EXPECT_FALSE(s.neighborCount().has_value());
}

TEST_F(ScaleTestServerHandlerTest, DownNodeBeforeStartThrows) {
  EXPECT_THROW(client_->sync_downNode("any"), thrift::NotRunningError);
}

TEST_F(ScaleTestServerHandlerTest, DownLinkBeforeStartThrows) {
  EXPECT_THROW(client_->sync_downLink("a", "b"), thrift::NotRunningError);
}

TEST_F(ScaleTestServerHandlerTest, NeighborStatsBeforeStartThrows) {
  thrift::NeighborStats out;
  EXPECT_THROW(client_->sync_getNeighborStats(out), thrift::NotRunningError);
}

TEST_F(ScaleTestServerHandlerTest, GetDutCountersBeforeStartReturnsEmpty) {
  std::map<std::string, int64_t> out;
  client_->sync_getDutCounters(out, "");
  EXPECT_TRUE(out.empty());
}

TEST_F(ScaleTestServerHandlerTest, StartWithInvalidConfigStaysNotRunning) {
  // A config that fails Session's validateConfig (here: default-constructed, so
  // dut.host is unset) makes the Session ctor throw SetupError inside
  // startTest, BEFORE any session is published. No DUT is needed to exercise
  // this path. The handler must surface the error AND leave session_ null so
  // the operator can retry with a corrected config: getTestStatus reports
  // not-running and per-session ops still throw NotRunningError.
  thrift::ScaleTestConfig invalid; // dut.host unset -> TOPOLOGY_INVALID
  EXPECT_THROW(client_->sync_startTest(invalid), thrift::SetupError);

  thrift::TestStatus s;
  client_->sync_getTestStatus(s);
  EXPECT_FALSE(*s.running());

  std::vector<std::string> nodes;
  EXPECT_THROW(client_->sync_listNodes(nodes), thrift::NotRunningError);

  // A second start attempt must NOT be rejected with AlreadyRunningError: the
  // failed attempt left no residue. It still throws SetupError (config is bad),
  // proving the fast-reject pre-check saw a null session.
  EXPECT_THROW(client_->sync_startTest(invalid), thrift::SetupError);
}

} // namespace openr
