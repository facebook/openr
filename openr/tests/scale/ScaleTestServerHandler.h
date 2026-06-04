/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <folly/Synchronized.h>

#include <openr/tests/scale/Session.h>
#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer.h>

namespace openr {

/*
 * Thrift handler for ScaleTestServer.
 *
 * Owns at most one in-flight Session at a time, held in a
 * folly::Synchronized<shared_ptr<Session>>. Uses the snapshot pattern: copy the
 * shared_ptr out under session_'s read lock, release the lock, then operate on
 * the snapshot (for both reads and mutations — Session's own methods are
 * internally locked). This keeps long-running RPCs (e.g. getDutCounters) and
 * mutations from blocking startTest / stopTest, and keeps the Session alive for
 * the duration of an in-flight op even if stopTest retires it concurrently.
 */
class ScaleTestServerHandler
    : public apache::thrift::ServiceHandler<thrift::ScaleTestServer> {
 public:
  ScaleTestServerHandler() = default;

  void sync_startTest(std::unique_ptr<thrift::ScaleTestConfig> config) override;
  void sync_stopTest() override;
  void sync_getTestStatus(thrift::TestStatus& out) override;
  void sync_listNodes(std::vector<std::string>& out) override;
  void sync_downNode(std::unique_ptr<std::string> nodeName) override;
  void sync_upNode(std::unique_ptr<std::string> nodeName) override;
  void sync_downLink(
      std::unique_ptr<std::string> localNode,
      std::unique_ptr<std::string> remoteNode) override;
  void sync_upLink(
      std::unique_ptr<std::string> localNode,
      std::unique_ptr<std::string> remoteNode) override;
  void sync_downNodes(
      std::unique_ptr<std::vector<std::string>> nodeNames) override;
  void sync_upNodes(
      std::unique_ptr<std::vector<std::string>> nodeNames) override;
  void sync_downLinks(
      std::unique_ptr<std::vector<thrift::LinkRef>> links) override;
  void sync_upLinks(
      std::unique_ptr<std::vector<thrift::LinkRef>> links) override;
  void sync_flapLink(
      std::unique_ptr<std::string> localNode,
      std::unique_ptr<std::string> remoteNode,
      int32_t cycles,
      int32_t intervalMs) override;
  void sync_flapLinks(
      std::unique_ptr<std::vector<thrift::LinkRef>> links,
      int32_t cycles,
      int32_t intervalMs) override;
  void sync_getDutCounters(
      std::map<std::string, int64_t>& out,
      std::unique_ptr<std::string> regexFilter) override;
  void sync_getNeighborStats(thrift::NeighborStats& out) override;
  void sync_verifyRoutes(thrift::RouteCounts& out) override;

 private:
  // Snapshot the current session under rlock, then release.
  std::shared_ptr<Session> snapshot() const;

  // Compute how many ports a session will reserve. Conservative flat
  // advance avoids TIME_WAIT collisions across rapid stop/start cycles.
  static int portsPerSession();

  folly::Synchronized<std::shared_ptr<Session>> session_;
  std::atomic<int> nextBasePort_{16300};
};

} // namespace openr
