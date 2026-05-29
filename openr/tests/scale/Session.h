/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <folly/executors/FunctionScheduler.h>

#include <openr/tests/scale/DutMonitor.h>
#include <openr/tests/scale/FakeKvStoreManager.h>
#include <openr/tests/scale/KvStoreThriftInjector.h>
#include <openr/tests/scale/RealSparkIo.h>
#include <openr/tests/scale/SparkFaker.h>
#include <openr/tests/scale/TopologyGenerator.h>
#include <openr/tests/scale/VirtualRouter.h>

#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer_types.h>

namespace openr {

/*
 * Session owns the in-process object graph for one running scale test.
 *
 * Lifecycle: construct -> start() -> (mutations / reads) -> destruct.
 * Only the dtor stops the running scheduler / fake kvstore / spark faker.
 *
 * Thread-safety: mutation methods take mutationMutex_; read methods either
 * take the same lock (listNodes, getStatus) or run lock-free over the
 * topology_ (listNodesUnlocked, for tests).
 */
class Session {
 public:
  Session(const thrift::ScaleTestConfig& cfg, int basePortOverride);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Side-effecting init. May throw thrift::SetupError.
  void start();

  // Mutations.
  void downNode(const std::string& name);
  void upNode(const std::string& name);
  void downLink(const std::string& a, const std::string& b);
  void upLink(const std::string& a, const std::string& b);

  // Reads.
  std::vector<std::string> listNodes() const;
  std::vector<std::string> listNodesUnlocked() const; // for tests
  thrift::TestStatus getStatus() const;
  std::shared_ptr<DutMonitor>
  getDutMonitor() const {
    return dutMonitor_;
  }

  // For tests only. In real code use getStatus() or listNodes().

  // The simulated fabric topology, with the DUT patched in by start().
  // Pre-start: matches the topology built from config. Post-start: also
  // contains the DUT as a router with neighbor->DUT adjacencies.
  const Topology&
  topology() const {
    return topology_;
  }
  // The DUT's node name. Empty until start() succeeds.
  const std::string&
  dutNodeName() const {
    return dutNodeName_;
  }

 private:
  void onTimerTick();
  void bumpFakeKeys();

  const thrift::ScaleTestConfig config_;
  std::string dutNodeName_; // resolved during start() via injector_->connect()
  // Built from config in the ctor; mutated EXACTLY ONCE during start() to
  // splice in the DUT (DutPatcher::patchDutIntoTopology). Never written
  // again after start() returns.
  Topology topology_;
  const std::chrono::steady_clock::time_point startedAt_;

  mutable std::mutex mutationMutex_;
  std::set<std::string> downedNodes_;
  std::set<std::pair<std::string, std::string>> downedLinks_;
  std::atomic<int64_t> cmdVersion_{1};
  std::atomic<int64_t> lastFakeKeyBumpSec_{0};
  std::atomic<int64_t> fakeKeyVersion_{1};

  std::unique_ptr<KvStoreThriftInjector> injector_;
  std::shared_ptr<DutMonitor> dutMonitor_;
  std::unique_ptr<FakeKvStoreManager> kvManager_;
  // shared_ptr: SparkFaker ctor takes shared_ptr<SparkIoInterface>.
  std::shared_ptr<RealSparkIo> sparkIo_;
  std::shared_ptr<SparkFaker> sparkFaker_;
  std::unique_ptr<folly::FunctionScheduler> scheduler_;
};

} // namespace openr
