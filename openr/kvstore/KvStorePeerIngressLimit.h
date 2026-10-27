/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include <folly/Synchronized.h>
#include <thrift/lib/cpp2/transport/rocket/server/IRocketServerConnection.h>
#include <thrift/lib/cpp2/transport/rocket/server/RocketRoutingHandler.h>

namespace openr {

/*
 * Open/R's backpressure settings for the dedicated KvStore peer Thrift server.
 * Set once at startup from the kOpenrIngressKvStore* constants. Byte values
 * are compared against pending-merge bytes (see
 * OpenrIngressKvStoreBackpressure).
 */
struct OpenrIngressKvStoreLimits {
  /*
   * Pause reading a peer connection, once its writes have drained, while the
   * pending-merge bytes of all peers together are above this.
   */
  size_t pauseAboveGlobalBytes{0};
  /*
   * Resume every paused connection once the pending-merge bytes of all peers
   * together drop below this. Must not be above pauseAboveGlobalBytes.
   */
  size_t resumeBelowGlobalBytes{0};
  /*
   * Resume a paused connection after this long even if bytes are still high,
   * so the peer's RPCs finish before its timeout.
   */
  std::chrono::milliseconds maxPauseDuration{0};
};

/*
 * TCP backpressure for the dedicated KvStore peer Thrift server, applied
 * before the server's ingress-memory limit closes peer connections.
 *
 * Each peer request's received bytes count as pending merge from the moment
 * its handler starts until KvStore has merged it. While the pending-merge
 * bytes of all peers together are above pauseAboveGlobalBytes, a connection
 * whose writes have drained stops being read: its kernel receive buffer fills
 * and TCP flow control stops the peer sending. Reads resume once the bytes
 * drop below resumeBelowGlobalBytes, or after maxPauseDuration so the peer's
 * RPCs still finish within kServiceProcTimeout.
 *
 * Call stopPausing() before stopping the server: a paused connection counts as
 * busy and holds the server's drain until it is resumed.
 */
class OpenrIngressKvStoreBackpressure
    : public std::enable_shared_from_this<OpenrIngressKvStoreBackpressure> {
 public:
  /*
   * Given by Rocket when a connection's writes have drained; pause() on it
   * stops reading and returns a ReadResumableHandle.
   */
  using ReadPausableHandle =
      apache::thrift::rocket::IRocketServerConnection::ReadPausableHandle;
  /*
   * Held while a connection is paused; resume() on it turns reading back on. If
   * dropped, its destructor resumes the connection too.
   */
  using ReadResumableHandle =
      apache::thrift::rocket::IRocketServerConnection::ReadResumableHandle;

  /*
   * Counts one peer request's received bytes as pending merge for as long as
   * it is alive. Keep it alive until KvStore has merged the request.
   */
  class OpenrIngressKvStorePendingMergeGuard {
   public:
    OpenrIngressKvStorePendingMergeGuard(
        std::shared_ptr<OpenrIngressKvStoreBackpressure> backpressure,
        size_t requestWireBytes);
    ~OpenrIngressKvStorePendingMergeGuard();
    OpenrIngressKvStorePendingMergeGuard(
        const OpenrIngressKvStorePendingMergeGuard&) = delete;
    OpenrIngressKvStorePendingMergeGuard& operator=(
        const OpenrIngressKvStorePendingMergeGuard&) = delete;
    OpenrIngressKvStorePendingMergeGuard(
        OpenrIngressKvStorePendingMergeGuard&&) = delete;
    OpenrIngressKvStorePendingMergeGuard& operator=(
        OpenrIngressKvStorePendingMergeGuard&&) = delete;

   private:
    /*
     * The shared object whose totals this guard adds to. Also keeps it alive
     * until the guard is destroyed.
     */
    const std::shared_ptr<OpenrIngressKvStoreBackpressure> backpressure_;
    /*
     * Received size of this one request, compressed if the peer compresses.
     * Added to the totals when the guard is created and removed when it is
     * destroyed.
     */
    const size_t requestWireBytes_;
  };

  explicit OpenrIngressKvStoreBackpressure(
      const OpenrIngressKvStoreLimits& openrIngressKvStoreLimits);

  /*
   * Count one request's received bytes as pending merge. The bytes stay counted
   * until the returned guard is destroyed, so keep it alive until KvStore has
   * merged the request.
   */
  std::unique_ptr<OpenrIngressKvStorePendingMergeGuard>
  trackOpenrIngressKvStorePendingMergeBytes(size_t requestWireBytes);

  /*
   * Called by Rocket on the connection's EventBase once its writes have
   * drained. Pauses the connection when pending-merge bytes are above
   * pauseAboveGlobalBytes, unless it is already paused or stopPausing() was
   * called.
   */
  void onWriteQuiescence(ReadPausableHandle handle);

  /*
   * Resume every paused connection now. Each resume runs on that connection's
   * EventBase.
   */
  void resumeAll();

  /*
   * Stops pausing connections and resumes every paused one, so the server can
   * drain and close them. Call before stopping the server.
   */
  void stopPausing();

  size_t
  getOpenrIngressKvStorePendingMergeGlobalBytes() const {
    return openrIngressKvStorePendingMergeGlobalBytes_.load();
  }

  // Number of connections paused right now.
  size_t getPausedConnectionCount() const;

 private:
  struct PausedConnection {
    /*
     * Matches the maxPauseDuration timer started for this pause, so the timer
     * resumes this pause only, not a later pause of the same connection.
     */
    uint64_t pauseId{0};
    /*
     * Turns reading back on. Must be released on the connection's EventBase
     * while the connection is still alive.
     */
    ReadResumableHandle resumeHandle;
    // When the pause started; reported through the pause_ms counter on resume.
    std::chrono::steady_clock::time_point pausedAt;
  };

  void addOpenrIngressKvStorePendingMergeBytes(size_t requestWireBytes);

  void releaseOpenrIngressKvStorePendingMergeBytes(size_t requestWireBytes);

  void resume(uint64_t pauseId);

  static void resumeOnEventBase(
      std::unique_ptr<PausedConnection> pausedConnection, bool timedOut);

  // Settings, fixed at construction.
  const OpenrIngressKvStoreLimits openrIngressKvStoreLimits_;

  /*
   * Pending-merge bytes of all peers together: the sum of requestWireBytes_ of
   * every live OpenrIngressKvStorePendingMergeGuard. Written from the threads
   * that create and destroy guards (Thrift CPU worker, KvStore) and read on the
   * IO thread in onWriteQuiescence().
   */
  std::atomic<size_t> openrIngressKvStorePendingMergeGlobalBytes_{0};
  /*
   * pauseId for the next pause. Every pause gets a new one, even for a
   * connection paused before.
   */
  std::atomic<uint64_t> nextPauseId_{0};
  /*
   * Set by stopPausing() at shutdown. Once set, onWriteQuiescence() no longer
   * pauses connections, so the server can drain them.
   */
  std::atomic<bool> isPausingStopped_{false};
  /*
   * Connections paused right now, with what is needed to resume each. A
   * connection appears at most once. Locked because pauses are added on the IO
   * thread while resumes can come from any thread.
   */
  folly::Synchronized<std::vector<std::unique_ptr<PausedConnection>>>
      pausedConnections_;
};

/*
 * Rocket routing handler for the KvStore peer Thrift server that hands each
 * connection's write quiescence to OpenrIngressKvStoreBackpressure.
 */
class KvStorePeerRoutingHandler : public apache::thrift::RocketRoutingHandler {
 public:
  KvStorePeerRoutingHandler(
      apache::thrift::ThriftServer& server,
      std::shared_ptr<OpenrIngressKvStoreBackpressure>
          openrIngressKvStoreBackpressure);

 protected:
  void onConnection(
      apache::thrift::rocket::IRocketServerConnection& connection) override;

 private:
  // Shared with the handler, which creates the pending-merge guards.
  const std::shared_ptr<OpenrIngressKvStoreBackpressure>
      openrIngressKvStoreBackpressure_;
};

} // namespace openr
