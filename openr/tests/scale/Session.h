/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <folly/executors/CPUThreadPoolExecutor.h>
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

  // Bulk mutations: apply to a SET of nodes/links as one coherent KvStore
  // update wave (one DUT convergence event). Validation is atomic — any invalid
  // member rejects the whole batch with nothing applied. Each affected neighbor
  // adj DB is rebuilt exactly once against the final downed state.
  void downNodes(const std::vector<std::string>& names);
  void upNodes(const std::vector<std::string>& names);
  void downLinks(const std::vector<thrift::LinkRef>& links);
  void upLinks(const std::vector<thrift::LinkRef>& links);

  // Fire-and-forget link flap: validate synchronously, then run
  // downLink(s) -> wait intervalMs -> upLink(s) for `cycles` iterations on a
  // background worker and return immediately. flapLink is the single-link
  // convenience wrapper over flapLinks. cycles <= 0 / empty links is a no-op;
  // a negative intervalMs is treated as 0.
  void flapLink(
      const std::string& a, const std::string& b, int cycles, int intervalMs);
  void flapLinks(
      const std::vector<thrift::LinkRef>& links, int cycles, int intervalMs);

  // Reads.
  std::vector<std::string> listNodes() const;
  std::vector<std::string> listNodesUnlocked() const; // for tests
  thrift::TestStatus getStatus() const;
  // Structured Spark-neighbor report (aggregate stats + per-neighbor table).
  // All-zero / empty when neighbor simulation is disabled (sparkFaker_ null).
  thrift::NeighborStats getNeighborStats() const;
  // Counts from the DUT's currently computed route database. Zero counts when
  // the injector channel to the DUT is not connected.
  thrift::RouteCounts verifyRoutes() const;
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
  // Builds the per-router fake-key KeyVals at the given version (the payload
  // the periodic bump pushes to both sinks). Empty when numFakeKeysPerNode is
  // unset / <= 0. Reads topology_ lock-free (immutable after start()). Used by
  // bumpFakeKeys(); exposed so tests can assert the key set without sinks.
  thrift::KeyVals buildFakeKeyVals(int64_t version) const;

  // Test hook: invoked once each background flap worker finishes (all cycles or
  // abort), so tests can wait on a baton/future instead of sleep-polling for
  // the async flap. No-op in production (unset).
  void setFlapDoneCallbackForTest(std::function<void()> cb);

 private:
  // start() phases, in order. Each performs one step of side-effecting init and
  // throws thrift::SetupError on failure.
  void validateSparkInterfaces() const; // pre-flight guard for SparkFaker
  void connectToDut(); // connect injector_/dutMonitor_, resolve dutNodeName_
  std::vector<std::string> patchDut(); // splice DUT in, return its neighbors
  void setupFakeKvStore(const std::vector<std::string>& dutNeighborNames);
  void setupSparkFaker(const std::vector<std::string>& dutNeighborNames);
  void injectInitialTopology(); // initial bulk KvStore injection into the DUT

  // Starts the periodic fake-key-version bump scheduler iff both
  // numFakeKeysPerNode and fakeKeyVersionBumpIntervalSec are > 0. Called at the
  // end of start().
  void maybeStartFakeKeyBump();
  void onTimerTick();
  void bumpFakeKeys();

  // Neighbors to omit from `node`'s adj DB given the current downed state: the
  // union of its operator-downed links (downedLinks_) and any adjacent downed
  // nodes (downedNodes_). Caller must hold mutationMutex_. This is the single
  // source of truth the bulk ops use to rebuild an up node's adjacencies once
  // against the final state.
  std::set<std::string> omitSetFor(const std::string& node) const;

  // Inject `kv` into the DUT and throw if the write was partial. injectKeyVals
  // returns a short count (it does NOT throw) on disconnect/RPC failure, so the
  // bulk ops route their DUT write through this to avoid reporting a partial
  // write as success. No rollback — a partial bulk write is recovered with
  // stopTest + startTest (the connection is already broken when this fires).
  void injectAllOrThrow(const thrift::KeyVals& kv, const char* op);

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
  std::atomic<int64_t> fakeKeyVersion_{1};

  std::unique_ptr<KvStoreThriftInjector> injector_;
  std::shared_ptr<DutMonitor> dutMonitor_;
  std::unique_ptr<FakeKvStoreManager> kvManager_;
  // shared_ptr: SparkFaker ctor takes shared_ptr<SparkIoInterface>.
  std::shared_ptr<RealSparkIo> sparkIo_;
  std::shared_ptr<SparkFaker> sparkFaker_;
  std::unique_ptr<folly::FunctionScheduler> scheduler_;

  // Background fire-and-forget link-flap workers. Flaps run on a recycled
  // thread pool (created lazily on first flap) so completed flaps don't
  // accumulate threads over a long session. flapMutex_ guards flapStop_;
  // flapCv_ makes the inter-toggle waits interruptible so ~Session() can signal
  // + join the pool promptly (before injector_/kvManager_ are torn down).
  std::mutex flapMutex_;
  std::condition_variable flapCv_;
  bool flapStop_{false};
  std::unique_ptr<folly::CPUThreadPoolExecutor> flapExecutor_;
  std::function<void()> flapDoneCb_; // test-only completion hook (see setter)
};

} // namespace openr
