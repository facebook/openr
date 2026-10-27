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
#include <optional>
#include <string>
#include <vector>

#include <fb303/QuantileStat.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <thrift/lib/cpp2/server/Cpp2ConnContext.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/transport/rocket/server/IRocketServerConnection.h>
#include <thrift/lib/cpp2/transport/rocket/server/RocketRoutingHandler.h>

#include <openr/kvstore/KvStoreIngressObserver.h>

namespace openr {

/*
 * Open/R's backpressure settings for the dedicated KvStore peer Thrift server.
 * Set once at startup from the kOpenrIngressKvStore* constants. Byte values
 * are compared against pending-merge and queued bytes (see
 * OpenrIngressKvStoreBackpressure).
 */
struct OpenrIngressKvStoreLimits {
  /*
   * Pause reading a peer connection, once its writes have drained, while the
   * pending-merge plus queued bytes of all peers together are above this.
   */
  size_t pauseAboveGlobalBytes{0};
  /*
   * Resume every paused connection once the pending-merge plus queued bytes of
   * all peers together drop below this. Must not be above
   * pauseAboveGlobalBytes.
   */
  size_t resumeBelowGlobalBytes{0};
  /*
   * Resume a paused connection after this long even if bytes are still high,
   * so the peer's RPCs finish before its timeout.
   */
  std::chrono::milliseconds maxPauseDuration{0};
  /*
   * Per peer: once a peer's queued bytes (requests waiting for the Thrift CPU
   * worker) reach this, its new requests are rejected with LOADSHEDDING. The
   * request that crosses it is still admitted.
   */
  size_t queuedPeerLimitBytes{0};
};

/*
 * TCP backpressure for the dedicated KvStore peer Thrift server, applied
 * before the server's ingress-memory limit closes peer connections.
 *
 * Pending-merge bytes are what peer updates hold until KvStore has merged
 * them, attributed to the peer that sent them: a request kept as received is
 * counted at its received (wire) size, which is compressed when the peer
 * compresses, and updates merged while KvStore is busy are counted at their
 * decoded size (KvStoreIngressBatch).
 *
 * While the pending-merge bytes of all peers together are above
 * pauseAboveGlobalBytes, a connection whose writes have drained stops being
 * read if its peer holds more than a fair share of pauseAboveGlobalBytes
 * (pauseAboveGlobalBytes divided by the number of peers holding bytes): its
 * kernel receive buffer fills and TCP flow control stops the peer sending,
 * while peers holding less keep flowing. Reads resume once the bytes drop
 * below resumeBelowGlobalBytes, or after maxPauseDuration so the peer's RPCs
 * still finish within kServiceProcTimeout.
 *
 * Queued bytes are requests read off a peer's connection and still waiting
 * for the Thrift CPU worker, counted at their received (compressed) size,
 * which is how they are held there. They count toward the pause like the
 * pending-merge bytes, and once a peer's queued bytes reach
 * queuedPeerLimitBytes its new requests are rejected: a pause only takes
 * effect after the peer's next reply, and nothing else bounds that queue in
 * bytes.
 *
 * Call stopPausing() before stopping the server: a paused connection counts as
 * busy and holds the server's drain until it is resumed.
 */
class OpenrIngressKvStoreBackpressure
    : public KvStoreIngressObserver,
      public std::enable_shared_from_this<OpenrIngressKvStoreBackpressure> {
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
        std::string peerName,
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
     * Peer that sent the request, from its KeySetParams senderId. The bytes are
     * added to and removed from this peer's total.
     */
    const std::string peerName_;
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
  trackOpenrIngressKvStorePendingMergeBytes(
      std::string peerName, size_t requestWireBytes);

  /*
   * KvStoreIngressObserver: `bytes` from `peerName` are now pending merge.
   * Called by guards for requests kept as received, and by KvStore for updates
   * it coalesces while busy.
   */
  void addOpenrIngressKvStorePendingMergeBytes(
      const std::string& peerName, size_t bytes) override;

  /*
   * KvStoreIngressObserver: `bytes` from `peerName` are no longer pending
   * merge. May resume paused connections once the total drops below
   * resumeBelowGlobalBytes.
   */
  void releaseOpenrIngressKvStorePendingMergeBytes(
      const std::string& peerName, size_t bytes) override;

  /*
   * Give a new peer connection the slot that records which peer it is.
   * Called on the connection's EventBase before it serves any request. Does
   * nothing if the connection already has one, so a second call cannot drop
   * the queued bytes counted in it.
   */
  static void initConnection(apache::thrift::Cpp2ConnContext& connection);

  /*
   * Record the peer a connection belongs to, from its requests' senderId. The
   * connection's queued bytes move to that peer's total.
   */
  void recordSenderId(
      apache::thrift::Cpp2ConnContext& connection, const std::string& senderId);

  /*
   * A request of requestWireBytes read off `connection` and waiting for the
   * CPU worker. Returns false, counting nothing, when its peer already holds
   * at least queuedPeerLimitBytes in queued requests; the request that takes
   * the peer over the limit is still admitted.
   */
  bool admitOpenrIngressKvStoreQueuedRequest(
      apache::thrift::Cpp2ConnContext& connection, size_t requestWireBytes);

  /*
   * The request was taken by the CPU worker, or dropped. Releases at most the
   * bytes the connection has queued, so a release without a matching admit
   * cannot wrap the totals.
   */
  void releaseOpenrIngressKvStoreQueuedRequest(
      apache::thrift::Cpp2ConnContext& connection, size_t requestWireBytes);

  /*
   * Serve `server`'s requests from a request pile that admits and releases
   * them through admitOpenrIngressKvStoreQueuedRequest() and
   * releaseOpenrIngressKvStoreQueuedRequest(), on one CPU worker thread.
   * Call before serve().
   */
  void installRequestPile(apache::thrift::ThriftServer& server);

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

  // Pending-merge bytes of one peer; 0 if it holds none.
  size_t getOpenrIngressKvStorePendingMergePeerBytes(
      const std::string& peerName) const;

  /*
   * Queued bytes of all peers together: requests read off peer connections and
   * still waiting for the Thrift CPU worker.
   */
  size_t
  getOpenrIngressKvStoreQueuedGlobalBytes() const {
    return openrIngressKvStoreQueuedGlobalBytes_.load();
  }

  /*
   * Number of peers holding pending-merge or queued bytes, counting each
   * connection whose peer is not known yet as a separate peer: the divisor of
   * the fair share. Kept up to date on every add and release, so this only
   * takes the lock and reads it. Used by tests and for debugging.
   */
  size_t getNumPeersHoldingBytes() const;

  // Number of connections paused right now.
  size_t getPausedConnectionCount() const;

 private:
  /*
   * Bytes held per peer, updated on every add and release so that
   * isOverFairShare() is one lookup and a division. The lock also guards each
   * connection's PeerSlot.
   */
  struct PeerBytes {
    /*
     * Pending-merge bytes per peer name. Kept in memory only: one ODS counter
     * per peer would cost too much quota.
     */
    folly::F14FastMap<std::string, size_t> pendingMergePeerBytes;
    /*
     * Pending-merge plus queued bytes per identified peer. A peer is removed
     * when its bytes reach 0, so the map size is the number of identified peers
     * holding bytes.
     */
    folly::F14FastMap<std::string, size_t> heldPeerBytes;
    // Connections whose peer is not known yet that hold queued bytes.
    size_t numUnidentifiedConnectionsHoldingBytes{0};

    /*
     * Pending-merge plus queued bytes held by `peerName`; 0 if it holds none.
     * isOverFairShare() compares this against the fair share.
     */
    size_t getHeldBytes(const std::string& peerName) const;
    /*
     * Number of peers the fair share is split across: identified peers holding
     * bytes plus connections not identified yet that hold queued bytes, each
     * counted as its own peer. For example, peers "a" and "b" holding bytes
     * plus one unidentified connection with a queued request give 3.
     */
    size_t getNumPeersHoldingBytes() const;
    /*
     * Count `bytes` more against `peerName`, adding the peer if it held
     * nothing. Called for pending-merge bytes and for queued bytes of
     * identified connections.
     */
    void addHeldBytes(const std::string& peerName, size_t bytes);
    /*
     * Count `bytes` fewer against `peerName`, removing the peer once it holds
     * nothing so it no longer counts in getNumPeersHoldingBytes(). Never
     * subtracts more than the peer holds, so a mismatched release cannot wrap
     * the total.
     */
    void releaseHeldBytes(const std::string& peerName, size_t bytes);
    /*
     * Count `bytes` more queued on a connection whose peer is `senderId`, or
     * not known yet when unset. `queuedBytes` is what the connection held
     * before this request. An identified connection adds to its peer's total;
     * an unidentified one starts counting as a peer when it goes from 0 to
     * holding bytes. Call before adding `bytes` to the connection's own count.
     */
    void addQueuedBytes(
        const std::optional<std::string>& senderId,
        size_t queuedBytes,
        size_t bytes);
    /*
     * Count `bytes` fewer queued on a connection whose peer is `senderId`, or
     * not known yet when unset. `queuedBytes` is what the connection holds
     * including these `bytes`. The reverse of addQueuedBytes(): an
     * unidentified connection stops counting as a peer when it releases all it
     * holds. Call before subtracting `bytes` from the connection's own count.
     * recordSenderId() releases all of a connection's queued bytes here and
     * adds them back under its peer.
     */
    void releaseQueuedBytes(
        const std::optional<std::string>& senderId,
        size_t queuedBytes,
        size_t bytes);
  };

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
    // When the pause started; reported through the pause_ms stat on resume.
    std::chrono::steady_clock::time_point pausedAt;
  };

  /*
   * True if the connection's peer holds more than its fair share of
   * pauseAboveGlobalBytes: pauseAboveGlobalBytes divided by the number of peers
   * holding bytes. A peer's bytes are its pending-merge bytes plus queued
   * bytes. Connections whose sender is not known yet each count as a separate
   * peer. Reads the totals in peerBytes_, so it costs one lookup.
   */
  bool isOverFairShare(apache::thrift::Cpp2ConnContext& connection) const;

  /*
   * Resume all paused connections if pending-merge plus queued bytes are below
   * resumeBelowGlobalBytes.
   */
  void resumeAllIfBelowResumeBelowGlobalBytes();

  void resume(uint64_t pauseId);

  void resumeOnEventBase(
      std::unique_ptr<PausedConnection> pausedConnection, bool timedOut) const;

  /*
   * Record pending-merge plus queued bytes in heldGlobalBytesStat_. Called
   * after every add: a release only lowers the total, so sampling on adds
   * catches every peak.
   */
  void recordHeldGlobalBytes() const;

  // Settings, fixed at construction.
  const OpenrIngressKvStoreLimits openrIngressKvStoreLimits_;

  /*
   * Pending-merge bytes of all peers together: requests kept as received (at
   * their received size) plus updates KvStore coalesced while busy (at their
   * decoded size). Equal to the sum of peerBytes_.pendingMergePeerBytes. Read
   * on the IO thread in onWriteQuiescence().
   */
  std::atomic<size_t> openrIngressKvStorePendingMergeGlobalBytes_{0};
  /*
   * Queued bytes of all peers together, at their received size. Added when the
   * request pile admits a request and removed when the CPU worker takes it or
   * it is dropped.
   */
  std::atomic<size_t> openrIngressKvStoreQueuedGlobalBytes_{0};
  folly::Synchronized<PeerBytes> peerBytes_;
  /*
   * Rolling-window stats, resolved once so the request path skips the fb303
   * name lookup. pauseMsStat_ gets one sample per pause, in milliseconds.
   * heldGlobalBytesStat_ gets one sample per add, in bytes.
   */
  const std::shared_ptr<facebook::fb303::QuantileStat> pauseMsStat_;
  const std::shared_ptr<facebook::fb303::QuantileStat> heldGlobalBytesStat_;
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
