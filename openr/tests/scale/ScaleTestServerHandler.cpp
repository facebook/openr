/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/ScaleTestServerHandler.h>

#include <utility>

namespace openr {

namespace {

// Default fb303 counter regex used by the legacy ScaleTestServer.cpp tick
// loop. Surfaced when callers do not supply a regexFilter.
constexpr auto kDefaultCounterRegex =
    "process\\.cpu\\.peak_pct|process\\.cpu\\.pct|"
    "process\\.memory\\.rss|"
    "kvstore\\.received_key_vals\\.sum|"
    "kvstore\\.updated_key_vals\\.sum|"
    "kvstore\\.thrift\\.num_flood_pub_success\\.count|"
    "kvstore\\.thrift\\.flood_pub_duration_ms\\.avg|"
    "kvstore\\.thrift\\.finalized_sync_duration_ms\\.avg|"
    "kvstore\\.thrift\\.full_sync_duration_ms\\.avg|"
    "decision\\.route_build_ms\\.avg|"
    "fib\\.route_programming\\.time_ms\\.avg|"
    "decision\\.spf_ms\\.avg|"
    "decision\\.path_build_ms\\.avg|"
    "fib\\.route_sync\\.time_ms|"
    "initialization\\.KVSTORE_SYNCED\\.duration_ms|"
    "initialization\\.INITIALIZED\\.duration_ms|"
    "decision\\.linkstate\\.up\\.propagation_time_ms|"
    "decision\\.linkstate\\.down\\.propagation_time_ms";

thrift::NotRunningError
makeNotRunning() {
  thrift::NotRunningError e;
  e.message() = "No session is currently running";
  return e;
}

thrift::AlreadyRunningError
makeAlreadyRunning() {
  thrift::AlreadyRunningError e;
  e.message() = "A session is already running; stopTest first";
  return e;
}

} // namespace

int
ScaleTestServerHandler::portsPerSession() {
  // Flat advance to dodge TIME_WAIT collisions across rapid stop/start cycles.
  return 1000;
}

std::shared_ptr<Session>
ScaleTestServerHandler::snapshot() const {
  return *session_.rlock();
}

void
ScaleTestServerHandler::sync_startTest(
    std::unique_ptr<thrift::ScaleTestConfig> config) {
  // Fast reject BEFORE reserving a port or doing any DUT-visible work.
  // Session::start() is not a dry run — it connects to and injects KvStore keys
  // into the real DUT and binds ports. Without this pre-check, a second/stray
  // startTest would run that full setup against the SAME DUT (corrupting the
  // active session's view) and leak its reserved port, only to then throw
  // AlreadyRunningError. Re-checked under wlock at publish below to close the
  // (single-operator-unlikely) start/start race.
  if (*session_.rlock()) {
    throw makeAlreadyRunning();
  }

  // Reserve a unique base port for this candidate.
  const int basePort =
      nextBasePort_.fetch_add(portsPerSession(), std::memory_order_relaxed);

  // Build + start the candidate OUTSIDE the daemon lock. start() may throw
  // thrift::SetupError, which propagates back to the caller untouched; the
  // candidate is then destroyed without ever being published.
  auto candidate = std::make_shared<Session>(*config, basePort);
  candidate->start();

  // Publish under wlock, re-checking in case another startTest won the race
  // after our fast read above.
  auto wlocked = session_.wlock();
  if (*wlocked) {
    throw makeAlreadyRunning();
  }
  *wlocked = std::move(candidate);
}

void
ScaleTestServerHandler::sync_stopTest() {
  // Move the shared_ptr out under wlock, release the lock, THEN drop the
  // local. This lets the Session dtor (which may block on scheduler/spark
  // shutdown) run without holding the daemon lock.
  //
  // Note: stopTest is NOT a hard barrier. An operation that already took a
  // snapshot() before this call keeps the Session alive via the refcount and
  // may still complete against it after stopTest returns (no use-after-free —
  // Session's per-op methods are internally locked). This is acceptable for the
  // single-operator scale harness; if strict stop semantics are ever needed,
  // add a "stopping" flag that rejects new ops once stopTest begins.
  std::shared_ptr<Session> victim;
  {
    auto wlocked = session_.wlock();
    if (!*wlocked) {
      throw makeNotRunning();
    }
    victim = std::move(*wlocked); // moved-from shared_ptr is left empty
  }
  // victim destructed here, lock-free.
}

void
ScaleTestServerHandler::sync_getTestStatus(thrift::TestStatus& out) {
  auto snap = snapshot();
  if (!snap) {
    out.running() = false;
    return;
  }
  out = snap->getStatus();
}

void
ScaleTestServerHandler::sync_listNodes(std::vector<std::string>& out) {
  auto snap = snapshot();
  if (!snap) {
    throw makeNotRunning();
  }
  out = snap->listNodes();
}

void
ScaleTestServerHandler::sync_downNode(std::unique_ptr<std::string> nodeName) {
  auto snap = snapshot();
  if (!snap) {
    throw makeNotRunning();
  }
  snap->downNode(*nodeName);
}

void
ScaleTestServerHandler::sync_upNode(std::unique_ptr<std::string> nodeName) {
  auto snap = snapshot();
  if (!snap) {
    throw makeNotRunning();
  }
  snap->upNode(*nodeName);
}

void
ScaleTestServerHandler::sync_downLink(
    std::unique_ptr<std::string> localNode,
    std::unique_ptr<std::string> remoteNode) {
  auto snap = snapshot();
  if (!snap) {
    throw makeNotRunning();
  }
  snap->downLink(*localNode, *remoteNode);
}

void
ScaleTestServerHandler::sync_upLink(
    std::unique_ptr<std::string> localNode,
    std::unique_ptr<std::string> remoteNode) {
  auto snap = snapshot();
  if (!snap) {
    throw makeNotRunning();
  }
  snap->upLink(*localNode, *remoteNode);
}

void
ScaleTestServerHandler::sync_getDutCounters(
    std::map<std::string, int64_t>& out,
    std::unique_ptr<std::string> regexFilter) {
  auto snap = snapshot();
  if (!snap) {
    // Empty map for "not running" — counters are best-effort observability.
    return;
  }
  auto monitor = snap->getDutMonitor();
  // snap goes out of scope here; we hold only `monitor` for the (potentially
  // long) blocking thrift call below. Daemon lock was never held.
  if (!monitor) {
    return;
  }
  out = monitor->getRegexCounters(
      (regexFilter && !regexFilter->empty())
          ? *regexFilter
          : std::string(kDefaultCounterRegex));
}

} // namespace openr
