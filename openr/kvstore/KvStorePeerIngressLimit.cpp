/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/kvstore/KvStorePeerIngressLimit.h>

#include <algorithm>
#include <array>
#include <utility>

#include <fb303/ServiceData.h>
#include <folly/Memory.h>
#include <folly/Range.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/server/ParallelConcurrencyController.h>
#include <thrift/lib/cpp2/server/RoundRobinRequestPile.h>

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
/*
 * Quantile stat: how long each pause lasted, in milliseconds. Exports
 * `pause_ms.{sum,count,avg,p50,p99,p100}.{60,600,3600}`, e.g. `sum.60` is the
 * total time connections were paused in the last minute and `p100.60` the
 * longest pause.
 */
constexpr folly::StringPiece kPauseMsCounter{"kvstore.peer_ingress.pause_ms"};
constexpr std::array<double, 3> kPauseMsQuantiles{{0.5, 0.99, 1.0}};
/*
 * Quantile stat: pending-merge plus queued bytes of all peers together, the
 * total compared against pauseAboveGlobalBytes. Exports
 * `openr_held_global_bytes.p100.{60,600}`, the peak in each window, which the
 * gauges above miss unless ODS reads them at that moment.
 */
constexpr folly::StringPiece kHeldGlobalBytesCounter{
    "kvstore.peer_ingress.openr_held_global_bytes"};
constexpr std::array<double, 1> kHeldGlobalBytesQuantiles{{1.0}};
// Gauge: queued bytes of all peers together.
constexpr folly::StringPiece kQueuedGlobalBytesCounter{
    "kvstore.peer_ingress.openr_queued_global_bytes"};
// Count: requests rejected because their peer reached queuedPeerLimitBytes.
constexpr folly::StringPiece kNumRejectedCounter{
    "kvstore.peer_ingress.num_rejected"};

/*
 * Per-connection user data. Created on the connection's EventBase before any
 * request. Its fields are guarded by OpenrIngressKvStoreBackpressure's
 * peerBytes_ lock.
 */
struct PeerSlot {
  /*
   * Peer the connection belongs to, from its requests' KeySetParams senderId.
   * Unset until the first request reaches the handler.
   */
  std::optional<std::string> senderId;
  /*
   * Received bytes of this connection's requests waiting for the Thrift CPU
   * worker. Compared against queuedPeerLimitBytes to reject the peer.
   */
  size_t queuedBytes{0};
};

PeerSlot*
getPeerSlot(apache::thrift::Cpp2ConnContext* connection) {
  return connection ? static_cast<PeerSlot*>(connection->getUserData())
                    : nullptr;
}

apache::thrift::Cpp2ConnContext*
getConnection(const apache::thrift::ServerRequest& request) {
  const auto* requestContext = request.requestContext();
  return requestContext ? requestContext->getConnectionContext() : nullptr;
}

size_t
getRequestWireBytes(const apache::thrift::ServerRequest& request) {
  const auto* requestContext = request.requestContext();
  return requestContext ? requestContext->getWiredRequestBytes() : 0;
}

} // namespace

size_t
OpenrIngressKvStoreBackpressure::PeerBytes::getHeldBytes(
    const std::string& peerName) const {
  const auto it = heldPeerBytes.find(peerName);
  return it == heldPeerBytes.end() ? 0 : it->second;
}

size_t
OpenrIngressKvStoreBackpressure::PeerBytes::getNumPeersHoldingBytes() const {
  return heldPeerBytes.size() + numUnidentifiedConnectionsHoldingBytes;
}

void
OpenrIngressKvStoreBackpressure::PeerBytes::addHeldBytes(
    const std::string& peerName, size_t bytes) {
  heldPeerBytes[peerName] += bytes;
}

void
OpenrIngressKvStoreBackpressure::PeerBytes::releaseHeldBytes(
    const std::string& peerName, size_t bytes) {
  auto it = heldPeerBytes.find(peerName);
  XDCHECK(it != heldPeerBytes.end());
  if (it == heldPeerBytes.end()) {
    return;
  }
  it->second -= std::min(bytes, it->second);
  if (it->second == 0) {
    heldPeerBytes.erase(it);
  }
}

void
OpenrIngressKvStoreBackpressure::PeerBytes::addQueuedBytes(
    const std::optional<std::string>& senderId,
    size_t queuedBytes,
    size_t bytes) {
  if (bytes == 0) {
    return;
  }
  if (senderId) {
    addHeldBytes(*senderId, bytes);
  } else if (queuedBytes == 0) {
    ++numUnidentifiedConnectionsHoldingBytes;
  }
}

void
OpenrIngressKvStoreBackpressure::PeerBytes::releaseQueuedBytes(
    const std::optional<std::string>& senderId,
    size_t queuedBytes,
    size_t bytes) {
  if (bytes == 0) {
    return;
  }
  if (senderId) {
    releaseHeldBytes(*senderId, bytes);
  } else if (queuedBytes == bytes) {
    --numUnidentifiedConnectionsHoldingBytes;
  }
}

OpenrIngressKvStoreBackpressure::OpenrIngressKvStorePendingMergeGuard::
    OpenrIngressKvStorePendingMergeGuard(
        std::shared_ptr<OpenrIngressKvStoreBackpressure> backpressure,
        std::string peerName,
        size_t requestWireBytes)
    : backpressure_(std::move(backpressure)),
      peerName_(std::move(peerName)),
      requestWireBytes_(requestWireBytes) {
  backpressure_->addOpenrIngressKvStorePendingMergeBytes(
      peerName_, requestWireBytes_);
}

OpenrIngressKvStoreBackpressure::OpenrIngressKvStorePendingMergeGuard::
    ~OpenrIngressKvStorePendingMergeGuard() {
  backpressure_->releaseOpenrIngressKvStorePendingMergeBytes(
      peerName_, requestWireBytes_);
}

OpenrIngressKvStoreBackpressure::OpenrIngressKvStoreBackpressure(
    const OpenrIngressKvStoreLimits& openrIngressKvStoreLimits)
    : openrIngressKvStoreLimits_(openrIngressKvStoreLimits),
      pauseMsStat_(
          fb303::fbData->getQuantileStat(
              kPauseMsCounter,
              fb303::ExportTypeConsts::kSumCountAvg,
              kPauseMsQuantiles,
              fb303::SlidingWindowPeriodConsts::kOneMinTenMinHour)),
      heldGlobalBytesStat_(
          fb303::fbData->getQuantileStat(
              kHeldGlobalBytesCounter,
              fb303::ExportTypeConsts::kNone,
              kHeldGlobalBytesQuantiles,
              fb303::SlidingWindowPeriodConsts::kOneMinTenMin)) {
  fb303::fbData->addStatExportType(kNumPausesCounter, fb303::COUNT);
  fb303::fbData->addStatExportType(kNumPauseTimeoutsCounter, fb303::COUNT);
  fb303::fbData->addStatExportType(kNumRejectedCounter, fb303::COUNT);
  fb303::fbData->setCounter(kPendingMergeGlobalBytesCounter, 0);
  fb303::fbData->setCounter(kQueuedGlobalBytesCounter, 0);
  fb303::fbData->setCounter(kPausedCounter, 0);
}

std::unique_ptr<
    OpenrIngressKvStoreBackpressure::OpenrIngressKvStorePendingMergeGuard>
OpenrIngressKvStoreBackpressure::trackOpenrIngressKvStorePendingMergeBytes(
    std::string peerName, size_t requestWireBytes) {
  return std::make_unique<OpenrIngressKvStorePendingMergeGuard>(
      shared_from_this(), std::move(peerName), requestWireBytes);
}

void
OpenrIngressKvStoreBackpressure::initConnection(
    apache::thrift::Cpp2ConnContext& connection) {
  if (connection.getUserData()) {
    return;
  }
  connection.setUserData(folly::make_erased_unique<PeerSlot>());
}

void
OpenrIngressKvStoreBackpressure::recordSenderId(
    apache::thrift::Cpp2ConnContext& connection, const std::string& senderId) {
  auto* slot = getPeerSlot(&connection);
  if (!slot || peerBytes_.withRLock([&](const auto&) {
        return slot->senderId == senderId;
      })) {
    return;
  }
  peerBytes_.withWLock([&](auto& peerBytes) {
    peerBytes.releaseQueuedBytes(
        slot->senderId, slot->queuedBytes, slot->queuedBytes);
    slot->senderId = senderId;
    peerBytes.addQueuedBytes(slot->senderId, 0, slot->queuedBytes);
  });
}

bool
OpenrIngressKvStoreBackpressure::admitOpenrIngressKvStoreQueuedRequest(
    apache::thrift::Cpp2ConnContext& connection, size_t requestWireBytes) {
  auto* slot = getPeerSlot(&connection);
  if (!slot) {
    return true;
  }
  const bool isAdmitted = peerBytes_.withWLock([&](auto& peerBytes) {
    /*
     * Reject only once the peer has already reached the limit. The request
     * that takes it over is admitted, so a request larger than the limit gets
     * in once the peer's queue drains instead of being rejected every time.
     */
    if (slot->queuedBytes >= openrIngressKvStoreLimits_.queuedPeerLimitBytes) {
      return false;
    }
    peerBytes.addQueuedBytes(
        slot->senderId, slot->queuedBytes, requestWireBytes);
    slot->queuedBytes += requestWireBytes;
    return true;
  });
  if (!isAdmitted) {
    fb303::fbData->addStatValue(kNumRejectedCounter, 1, fb303::COUNT);
    return false;
  }
  fb303::fbData->setCounter(
      kQueuedGlobalBytesCounter,
      openrIngressKvStoreQueuedGlobalBytes_.fetch_add(requestWireBytes) +
          requestWireBytes);
  recordHeldGlobalBytes();
  return true;
}

void
OpenrIngressKvStoreBackpressure::releaseOpenrIngressKvStoreQueuedRequest(
    apache::thrift::Cpp2ConnContext& connection, size_t requestWireBytes) {
  auto* slot = getPeerSlot(&connection);
  if (!slot) {
    return;
  }
  const size_t releasedBytes = peerBytes_.withWLock([&](auto& peerBytes) {
    const size_t bytes = std::min(requestWireBytes, slot->queuedBytes);
    peerBytes.releaseQueuedBytes(slot->senderId, slot->queuedBytes, bytes);
    slot->queuedBytes -= bytes;
    return bytes;
  });
  if (releasedBytes != requestWireBytes) {
    XLOGF_EVERY_MS(
        ERR,
        10000,
        "Released {} queued bytes but the connection held only {}",
        requestWireBytes,
        releasedBytes);
  }
  if (releasedBytes == 0) {
    return;
  }
  fb303::fbData->setCounter(
      kQueuedGlobalBytesCounter,
      openrIngressKvStoreQueuedGlobalBytes_.fetch_sub(releasedBytes) -
          releasedBytes);
  resumeAllIfBelowResumeBelowGlobalBytes();
}

void
OpenrIngressKvStoreBackpressure::installRequestPile(
    apache::thrift::ThriftServer& server) {
  apache::thrift::RoundRobinRequestPile::Options options;
  options.setPreEnqueueFilter(
      [self = shared_from_this()](const apache::thrift::ServerRequest& request)
          -> std::optional<apache::thrift::ServerRequestRejection> {
        auto* connection = getConnection(request);
        if (!connection ||
            self->admitOpenrIngressKvStoreQueuedRequest(
                *connection, getRequestWireBytes(request))) {
          return std::nullopt;
        }
        return apache::thrift::ServerRequestRejection(
            apache::thrift::TApplicationException(
                apache::thrift::TApplicationException::LOADSHEDDING,
                "KvStore peer has too many bytes waiting to be processed"));
      });
  /*
   * No request limit is set (numMaxRequests stays 0), so a request the filter
   * admits is always enqueued, and the dequeue observer below releases it
   * whether the CPU worker takes it or it expires in the pile. The pile does
   * not tell the observer about a request it rejects after the filter, so
   * setting a limit would leave that request's bytes counted.
   */
  auto pile = std::make_unique<apache::thrift::RoundRobinRequestPile>(
      std::move(options));
  pile->setDequeueObserver([self = shared_from_this()](
                               const apache::thrift::ServerRequest& request) {
    if (auto* connection = getConnection(request)) {
      self->releaseOpenrIngressKvStoreQueuedRequest(
          *connection, getRequestWireBytes(request));
    }
  });
  // One worker, as setUpThriftServer() configures for every Open/R server.
  auto executor = std::make_shared<folly::CPUThreadPoolExecutor>(
      1, std::make_shared<folly::NamedThreadFactory>("openr-KvStorePeer"));
  auto concurrencyController =
      std::make_unique<apache::thrift::ParallelConcurrencyController>(
          *pile, *executor);
  server.requireResourcePools();
  server.resourcePoolSet().setResourcePool(
      apache::thrift::ResourcePoolHandle::defaultAsync(),
      std::move(pile),
      std::move(executor),
      std::move(concurrencyController),
      apache::thrift::concurrency::NORMAL);
}

bool
OpenrIngressKvStoreBackpressure::isOverFairShare(
    apache::thrift::Cpp2ConnContext& connection) const {
  const auto* slot = getPeerSlot(&connection);
  if (!slot) {
    return false;
  }
  return peerBytes_.withRLock([&](const auto& peerBytes) {
    const size_t heldBytes = slot->senderId
        ? peerBytes.getHeldBytes(*slot->senderId)
        : slot->queuedBytes;
    if (heldBytes == 0) {
      return false;
    }
    // The divisor is at least 1: the peer holding heldBytes is counted.
    return heldBytes > openrIngressKvStoreLimits_.pauseAboveGlobalBytes /
        peerBytes.getNumPeersHoldingBytes();
  });
}

void
OpenrIngressKvStoreBackpressure::onWriteQuiescence(ReadPausableHandle handle) {
  if (isPausingStopped_.load() ||
      openrIngressKvStorePendingMergeGlobalBytes_.load() +
              openrIngressKvStoreQueuedGlobalBytes_.load() <=
          openrIngressKvStoreLimits_.pauseAboveGlobalBytes) {
    return;
  }
  auto* connection = handle.getCpp2ConnContext();
  if (!getPeerSlot(connection) || !isOverFairShare(*connection)) {
    return;
  }

  /*
   * A paused connection still drains its in-flight requests and reaches
   * write quiescence again. Rocket counts pause handles, but resume() turns
   * reading back on without checking for other held pauses, so the first
   * resume would undo a second pause. Pause each connection at most once.
   */
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
  if (isPausingStopped_.load()) {
    resumeAll();
    return;
  }
  resumeAllIfBelowResumeBelowGlobalBytes();
}

void
OpenrIngressKvStoreBackpressure::resumeAllIfBelowResumeBelowGlobalBytes() {
  if (openrIngressKvStorePendingMergeGlobalBytes_.load() +
              openrIngressKvStoreQueuedGlobalBytes_.load() <
          openrIngressKvStoreLimits_.resumeBelowGlobalBytes &&
      getPausedConnectionCount() > 0) {
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

size_t
OpenrIngressKvStoreBackpressure::getOpenrIngressKvStorePendingMergePeerBytes(
    const std::string& peerName) const {
  return peerBytes_.withRLock([&](const auto& peerBytes) {
    const auto it = peerBytes.pendingMergePeerBytes.find(peerName);
    return it == peerBytes.pendingMergePeerBytes.end() ? 0 : it->second;
  });
}

size_t
OpenrIngressKvStoreBackpressure::getNumPeersHoldingBytes() const {
  return peerBytes_.rlock()->getNumPeersHoldingBytes();
}

void
OpenrIngressKvStoreBackpressure::addOpenrIngressKvStorePendingMergeBytes(
    const std::string& peerName, size_t bytes) {
  if (bytes == 0) {
    return;
  }
  peerBytes_.withWLock([&](auto& peerBytes) {
    peerBytes.pendingMergePeerBytes[peerName] += bytes;
    peerBytes.addHeldBytes(peerName, bytes);
  });
  fb303::fbData->setCounter(
      kPendingMergeGlobalBytesCounter,
      openrIngressKvStorePendingMergeGlobalBytes_.fetch_add(bytes) + bytes);
  recordHeldGlobalBytes();
}

void
OpenrIngressKvStoreBackpressure::recordHeldGlobalBytes() const {
  heldGlobalBytesStat_->addValue(
      openrIngressKvStorePendingMergeGlobalBytes_.load() +
      openrIngressKvStoreQueuedGlobalBytes_.load());
}

void
OpenrIngressKvStoreBackpressure::releaseOpenrIngressKvStorePendingMergeBytes(
    const std::string& peerName, size_t bytes) {
  if (bytes == 0) {
    return;
  }
  const size_t releasedBytes =
      peerBytes_.withWLock([&](auto& peerBytes) -> size_t {
        auto it = peerBytes.pendingMergePeerBytes.find(peerName);
        if (it == peerBytes.pendingMergePeerBytes.end()) {
          return 0;
        }
        const size_t released = std::min(bytes, it->second);
        it->second -= released;
        if (it->second == 0) {
          peerBytes.pendingMergePeerBytes.erase(it);
        }
        peerBytes.releaseHeldBytes(peerName, released);
        return released;
      });
  /*
   * Every release follows an add of the same bytes under the same peer, so
   * the peer is always present and holds at least `bytes` here. If not, only
   * what the peer holds is released, from the per-peer and global totals
   * alike, so they stay equal and cannot wrap.
   */
  XDCHECK_EQ(bytes, releasedBytes);
  if (releasedBytes == 0) {
    return;
  }
  fb303::fbData->setCounter(
      kPendingMergeGlobalBytesCounter,
      openrIngressKvStorePendingMergeGlobalBytes_.fetch_sub(releasedBytes) -
          releasedBytes);
  resumeAllIfBelowResumeBelowGlobalBytes();
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
    std::unique_ptr<PausedConnection> pausedConnection, bool timedOut) const {
  auto& evb = pausedConnection->resumeHandle.getEventBase();
  evb.runInEventBaseThread([pausedConnection = std::move(pausedConnection),
                            timedOut,
                            pauseMsStat = pauseMsStat_]() mutable {
    const auto pauseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - pausedConnection->pausedAt);
    pauseMsStat->addValue(pauseMs.count());
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
  if (auto* connContext = connection.getCpp2ConnContext()) {
    OpenrIngressKvStoreBackpressure::initConnection(*connContext);
  }
  connection.setOnWriteQuiescenceCallback(
      [backpressure = openrIngressKvStoreBackpressure_](
          OpenrIngressKvStoreBackpressure::ReadPausableHandle handle) {
        backpressure->onWriteQuiescence(std::move(handle));
      });
}

} // namespace openr
