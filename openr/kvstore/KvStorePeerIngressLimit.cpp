/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/kvstore/KvStorePeerIngressLimit.h>

#include <algorithm>
#include <utility>

#include <fb303/ServiceData.h>
#include <folly/Range.h>
#include <folly/io/async/EventBase.h>

namespace fb303 = facebook::fb303;

namespace openr {

namespace {

// Gauge: pending-merge bytes of all peers together.
constexpr folly::StringPiece kPendingMergeGlobalBytesCounter{
    "kvstore.peer_ingress.openr_pending_merge_global_bytes"};
// Gauge: connections paused right now.
constexpr folly::StringPiece kPausedCounter{
    "kvstore.peer_ingress.paused_connections"};
// Count: pauses started.
constexpr folly::StringPiece kNumPausesCounter{
    "kvstore.peer_ingress.num_pauses"};
/*
 * Count: pauses ended by maxPauseDuration instead of bytes dropping below
 * resumeBelowGlobalBytes.
 */
constexpr folly::StringPiece kNumPauseTimeoutsCounter{
    "kvstore.peer_ingress.num_pause_timeouts"};
// Average: how long each pause lasted, in milliseconds.
constexpr folly::StringPiece kPauseMsCounter{"kvstore.peer_ingress.pause_ms"};

} // namespace

OpenrIngressKvStoreBackpressure::OpenrIngressKvStorePendingMergeGuard::
    OpenrIngressKvStorePendingMergeGuard(
        std::shared_ptr<OpenrIngressKvStoreBackpressure> backpressure,
        size_t requestWireBytes)
    : backpressure_(std::move(backpressure)),
      requestWireBytes_(requestWireBytes) {
  backpressure_->addOpenrIngressKvStorePendingMergeBytes(requestWireBytes_);
}

OpenrIngressKvStoreBackpressure::OpenrIngressKvStorePendingMergeGuard::
    ~OpenrIngressKvStorePendingMergeGuard() {
  backpressure_->releaseOpenrIngressKvStorePendingMergeBytes(requestWireBytes_);
}

OpenrIngressKvStoreBackpressure::OpenrIngressKvStoreBackpressure(
    const OpenrIngressKvStoreLimits& openrIngressKvStoreLimits)
    : openrIngressKvStoreLimits_(openrIngressKvStoreLimits) {
  fb303::fbData->addStatExportType(kNumPausesCounter, fb303::COUNT);
  fb303::fbData->addStatExportType(kNumPauseTimeoutsCounter, fb303::COUNT);
  fb303::fbData->addStatExportType(kPauseMsCounter, fb303::AVG);
  fb303::fbData->setCounter(kPendingMergeGlobalBytesCounter, 0);
  fb303::fbData->setCounter(kPausedCounter, 0);
}

std::unique_ptr<
    OpenrIngressKvStoreBackpressure::OpenrIngressKvStorePendingMergeGuard>
OpenrIngressKvStoreBackpressure::trackOpenrIngressKvStorePendingMergeBytes(
    size_t requestWireBytes) {
  return std::make_unique<OpenrIngressKvStorePendingMergeGuard>(
      shared_from_this(), requestWireBytes);
}

void
OpenrIngressKvStoreBackpressure::onWriteQuiescence(ReadPausableHandle handle) {
  if (isPausingStopped_.load() ||
      openrIngressKvStorePendingMergeGlobalBytes_.load() <=
          openrIngressKvStoreLimits_.pauseAboveGlobalBytes) {
    return;
  }

  /*
   * A paused connection still drains its in-flight requests and reaches
   * write quiescence again. Rocket counts pause handles, but resume() turns
   * reading back on without checking for other held pauses, so the first
   * resume would undo a second pause. Pause each connection at most once.
   */
  auto* connection = handle.getCpp2ConnContext();
  auto& evb = handle.getEventBase();
  const uint64_t pauseId = nextPauseId_++;
  const bool isPaused = pausedConnections_.withWLock([&](auto& paused) {
    const bool isAlreadyPaused =
        std::any_of(paused.begin(), paused.end(), [connection](const auto& p) {
          return p->resumeHandle.getCpp2ConnContext() == connection;
        });
    if (isAlreadyPaused) {
      return false;
    }
    paused.push_back(
        std::make_unique<PausedConnection>(PausedConnection{
            .pauseId = pauseId,
            .resumeHandle = std::move(handle).pause(),
            .pausedAt = std::chrono::steady_clock::now()}));
    fb303::fbData->setCounter(kPausedCounter, paused.size());
    return true;
  });
  if (!isPaused) {
    return;
  }
  fb303::fbData->addStatValue(kNumPausesCounter, 1, fb303::COUNT);

  /*
   * Not cancelled on resume: a timer whose connection was already resumed
   * finds no matching pauseId and does nothing.
   */
  evb.runAfterDelay(
      [weak = weak_from_this(), pauseId]() {
        if (auto self = weak.lock()) {
          self->resume(pauseId);
        }
      },
      static_cast<uint32_t>(
          openrIngressKvStoreLimits_.maxPauseDuration.count()));

  /*
   * Bytes may have dropped below resumeBelowGlobalBytes on another thread, or
   * stopPausing() may have run, before this connection was added. Nothing
   * else would resume it then.
   */
  if (isPausingStopped_.load() ||
      openrIngressKvStorePendingMergeGlobalBytes_.load() <
          openrIngressKvStoreLimits_.resumeBelowGlobalBytes) {
    resumeAll();
  }
}

void
OpenrIngressKvStoreBackpressure::resumeAll() {
  std::vector<std::unique_ptr<PausedConnection>> resumed;
  pausedConnections_.withWLock([&](auto& paused) {
    resumed.swap(paused);
    fb303::fbData->setCounter(kPausedCounter, 0);
  });
  for (auto& pausedConnection : resumed) {
    resumeOnEventBase(std::move(pausedConnection), /*timedOut=*/false);
  }
}

void
OpenrIngressKvStoreBackpressure::stopPausing() {
  isPausingStopped_.store(true);
  resumeAll();
}

size_t
OpenrIngressKvStoreBackpressure::getPausedConnectionCount() const {
  return pausedConnections_.rlock()->size();
}

void
OpenrIngressKvStoreBackpressure::addOpenrIngressKvStorePendingMergeBytes(
    size_t requestWireBytes) {
  fb303::fbData->setCounter(
      kPendingMergeGlobalBytesCounter,
      openrIngressKvStorePendingMergeGlobalBytes_.fetch_add(requestWireBytes) +
          requestWireBytes);
}

void
OpenrIngressKvStoreBackpressure::releaseOpenrIngressKvStorePendingMergeBytes(
    size_t requestWireBytes) {
  const size_t remaining =
      openrIngressKvStorePendingMergeGlobalBytes_.fetch_sub(requestWireBytes) -
      requestWireBytes;
  fb303::fbData->setCounter(kPendingMergeGlobalBytesCounter, remaining);
  if (remaining < openrIngressKvStoreLimits_.resumeBelowGlobalBytes &&
      getPausedConnectionCount() > 0) {
    resumeAll();
  }
}

void
OpenrIngressKvStoreBackpressure::resume(uint64_t pauseId) {
  std::unique_ptr<PausedConnection> resumed;
  pausedConnections_.withWLock([&](auto& paused) {
    auto it = std::find_if(paused.begin(), paused.end(), [pauseId](auto& p) {
      return p->pauseId == pauseId;
    });
    if (it == paused.end()) {
      return;
    }
    resumed = std::move(*it);
    paused.erase(it);
    fb303::fbData->setCounter(kPausedCounter, paused.size());
  });
  if (resumed) {
    resumeOnEventBase(std::move(resumed), /*timedOut=*/true);
  }
}

void
OpenrIngressKvStoreBackpressure::resumeOnEventBase(
    std::unique_ptr<PausedConnection> pausedConnection, bool timedOut) {
  auto& evb = pausedConnection->resumeHandle.getEventBase();
  evb.runInEventBaseThread([pausedConnection = std::move(pausedConnection),
                            timedOut]() mutable {
    const auto pauseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - pausedConnection->pausedAt);
    fb303::fbData->addStatValue(kPauseMsCounter, pauseMs.count(), fb303::AVG);
    if (timedOut) {
      fb303::fbData->addStatValue(kNumPauseTimeoutsCounter, 1, fb303::COUNT);
    }
    std::move(pausedConnection->resumeHandle).resume();
  });
}

KvStorePeerRoutingHandler::KvStorePeerRoutingHandler(
    apache::thrift::ThriftServer& server,
    std::shared_ptr<OpenrIngressKvStoreBackpressure>
        openrIngressKvStoreBackpressure)
    : apache::thrift::RocketRoutingHandler(server),
      openrIngressKvStoreBackpressure_(
          std::move(openrIngressKvStoreBackpressure)) {}

void
KvStorePeerRoutingHandler::onConnection(
    apache::thrift::rocket::IRocketServerConnection& connection) {
  connection.setOnWriteQuiescenceCallback(
      [backpressure = openrIngressKvStoreBackpressure_](
          OpenrIngressKvStoreBackpressure::ReadPausableHandle handle) {
        backpressure->onWriteQuiescence(std::move(handle));
      });
}

} // namespace openr
