/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/fibers/Semaphore.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>

#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/if/gen-cpp2/FibService.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreClientInternal.h>
#include <openr/messaging/Queue.h>
#include <openr/monitor/LogSample.h>

namespace openr {

/**
 * Programs computed routes to the underlying platform (e.g. FBOSS or Linux). It
 * uses thrift interface defined in `Platform.thrift` for programming routes.
 *
 * FIB module subscribes to route updates from Decision module and programs it.
 * It'll take care of re-programming routes under the failure cases. FIB
 * publishes the updates after successful programming.
 */
class Fib final : public OpenrEventBase {
 public:
  Fib(
      // config
      std::shared_ptr<const Config> config,
      // consumer queue
      messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueue,
      messaging::RQueue<DecisionRouteUpdate> staticRouteUpdatesQueue,
      // producer queue
      messaging::ReplicateQueue<DecisionRouteUpdate>& fibRouteUpdatesQueue,
      messaging::ReplicateQueue<LogSample>& logSampleQueue);

  /**
   * Override stop method of OpenrEventBase
   */
  void stop() override;

  /**
   * Utility function to create thrift client connection to SwitchAgent. Can
   * throw exception if it fails to open transport to client on specified port.
   * It will return immediately if healthy client connection already exists.
   */
  static void createFibClient(
      folly::EventBase& evb,
      folly::AsyncSocket*& socket,
      std::unique_ptr<thrift::FibServiceAsyncClient>& client,
      int32_t port);

  /**
   * Perform longest prefix match among all prefixes in route database.
   * @param inputPrefix - a prefix that need to be matched
   * @param unicastRoutes - current unicast routes in RouteDatabase
   *
   * @return the matched CIDRNetwork if prefix matching succeed.
   */
  static std::optional<folly::CIDRNetwork> longestPrefixMatch(
      const folly::CIDRNetwork& inputPrefix,
      const std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>&
          unicastRoutes);

  /**
   * Show unicast routes which are to be added or updated
   */
  static void printUnicastRoutesAddUpdate(
      const std::vector<thrift::UnicastRoute>& unicastRoutesToUpdate);

  /**
  Show MPLS routes which are to be added or updated
  */
  static void printMplsRoutesAddUpdate(
      const std::vector<thrift::MplsRoute>& mplsRoutesToUpdate);

  /**
   * NOTE: DEPRECATED! Use getUnicastRoutes or getMplsRoutes.
   */
  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>> getRouteDb();

  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabaseDetail>>
  getRouteDetailDb();

  /**
   * Retrieve unicast routes for specified prefixes or IP. Returns all if
   * no prefix is specified in filter list.
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
  getUnicastRoutes(std::vector<std::string> prefixes);

  /**
   * Retrieve mpls routes for specified labels. Returns all if no label is
   * specified in filter list.
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::MplsRoute>>>
  getMplsRoutes(std::vector<int32_t> labels);

  /**
   * Retrieve performance related information from FIB module
   */
  folly::SemiFuture<std::unique_ptr<thrift::PerfDatabase>> getPerfDb();

  /**
   * API to get reader for fibUpdatesQueue
   */
  messaging::RQueue<DecisionRouteUpdate> getFibUpdatesReader();

 private:
  // No-copy
  Fib(const Fib&) = delete;
  Fib& operator=(const Fib&) = delete;

  /**
   * Convert local perfDb_ into PerfDataBase
   */
  thrift::PerfDatabase dumpPerfDb() const;

  /**
   * Retrieve unicast routes with specified filters
   */
  std::vector<thrift::UnicastRoute> getUnicastRoutesFiltered(
      std::vector<std::string> prefixes);

  /**
   * Retrieve mpls routes with specified filters
   */
  std::vector<thrift::MplsRoute> getMplsRoutesFiltered(
      std::vector<int32_t> labels);

  /**
   * Process new route updates received from Decision module
   */
  void processDecisionRouteUpdate(DecisionRouteUpdate&& routeUpdate);

  /**
   * Process new route updates received from StaticRoute update queue
   */
  void processStaticRouteUpdate(DecisionRouteUpdate&& routeUpdate);

  /**
   * Incremental route programming. On route programming failure,
   * prefixes/labels are marked dirty and retryRoutesTimer is scheduled.
   * If useDeleteDelay is false, delete routes without putting them in
   * dirtyPrefixes/dirtyLabels (i.e., don't delay programming). Otherwise,
   * delay deletion based on configured duration.
   */
  void updateRoutes(
      DecisionRouteUpdate&& routeUpdate, bool useDeleteDelay = true);

  /**
   * Sync the current RouteState with the switch agent.
   * - On complete failure retry is scheduled
   * - On partial failure, the failed prefixes/labels are marked dirty and
   *   retryRoutesTimer is scheduled.
   */
  void syncRoutes();

  /**
   * Asynchrounsly schedules the retry routes timer call and returns
   * immediately. All APIs should call this function to schedule retry-timer.
   */
  void scheduleRetryRoutesTimer();

  /**
   * Get aliveSince from FibService, and check if Fib restarts
   * If so, push syncFib to FibService
   */
  void keepAliveCheck();

  /**
   * Update flat counter/stats in fb303
   */
  void updateGlobalCounters();

  /**
   * Create, log, and publish Open/R convergence event through LogSampleQueue
   */
  void logPerfEvents(std::optional<thrift::PerfEvents>& perfEvents);

  /**
   * State variables to represent computed and programmed routes.
   */
  struct RouteState {
    // Non modified copy of Unicast and MPLS routes received from Decision
    std::unordered_map<folly::CIDRNetwork, RibUnicastEntry> unicastRoutes;
    std::unordered_map<int32_t, RibMplsEntry> mplsRoutes;

    /**
     * Set of route keys (prefixes & labels) that needs to be updated in HW. Two
     * reasons for dirty marking
     * 1) A new update/delete notification is received for Prefix/Label
     * 2) Prefix/Label experienced a programming failure
     * 3) A delete update needs to be delayed.
     * Along with prefixes and labels, we also store timestamp when routes are
     * received or updated.
     */
    std::unordered_map<
        folly::CIDRNetwork,
        std::chrono::time_point<std::chrono::steady_clock>>
        dirtyPrefixes;
    std::unordered_map<
        uint32_t,
        std::chrono::time_point<std::chrono::steady_clock>>
        dirtyLabels;

    /**
     * Enumeration depicting the route event that may arrive and affect `State`
     */
    enum Event {
      // Route update from Decision
      RIB_UPDATE = 0,
      // FIB Agent connected or re-connected because of process restart
      FIB_CONNECTED = 1,
      // FIB sync is successful
      FIB_SYNCED = 2,
    };

    /**
     * Enumeration depicting the current state of Routes
     */
    enum State {
      // FIB starts in this state. It is awaiting RIB, but meanwhile will
      // program any received static route update.
      AWAITING = 0,
      // Once the first RIB update aka snapshot is received, FIB transitions
      // to syncing state. State may also downgrade to this state from SYNCED
      // on FIB_CONNECTED (FibAgent reconnects).
      SYNCING = 1,
      // After successful SYNC of routes, FIB enters this state and perform
      // only incremental route updates, deletes or retries.
      SYNCED = 2,
    };
    State state{AWAITING}; // We start in AWAITING state

    // Flag to indicate first sync
    bool isInitialSynced{false};

    /**
     * Does current route state needs (re-)programming of routes
     */
    bool
    needsRetry() const {
      return state == SYNCING or dirtyPrefixes.size() or dirtyLabels.size();
    }

    // Util function to convert ENUM State to string
    static std::string toStr(const State state);

    /**
     * 1. Update RouteState with the received route update from Decision or
     * Static RouteUpdates queue. Update - unicastRoutes and mplsRoutes which
     * are similar to intended FIB tables for unicast and mpls routes
     * respectively.
     * 2. Update dirty set of prefixes and labels.
     * 3. Process delete updates and delay deletion if configured to do so.
     */
    void update(const DecisionRouteUpdate& routeUpdate);

    /**
     * Create DecisionRouteUpdate that'll need to be re-programmed & published
     * to users. As a part of this dirty prefixes and labels will be cleared as
     * they'll be captured in this update that would be programmed.
     */
    DecisionRouteUpdate createUpdate();

    /**
     * Update state as a result of PlatformFibUpdateError. This will populate
     * the dirty state.
     */
    void processFibUpdateError(thrift::PlatformFibUpdateError const& fibError);
  };

  bool
  delayedDeletionEnabled() const {
    return routeDeleteDelay_ > std::chrono::milliseconds(0);
  }

  /**
   * Get the next earliest timestamps from those routes which are pending
   * delete.
   */
  std::chrono::milliseconds nextRetryDuration() const;

  /**
   * Helper function for state transition based on the event. Perform special
   * processing if applicable.
   */
  void transitionRouteState(RouteState::Event event);

  // Instantiation of route state
  RouteState routeState_;

  // Events to capture and indicate performance of protocol convergence.
  std::deque<thrift::PerfEvents> perfDb_;

  // Create timestamp of recently logged perf event
  int64_t recentPerfEventCreateTs_{0};

  // Name of node on which OpenR is running
  const std::string myNodeName_;

  // Switch agent thrift server port
  const int32_t thriftPort_{0};

  // Config Knob - In dry run FIB will not invoke route programming
  // APIs, and mimick the whole logic as programming is successful.
  const bool dryrun_{true};

  // Config Knob - Indicates if Segment Routing feature is enabled or not. MPLS
  // routes will be programmed only if segment routing is enabled.
  const bool enableSegmentRouting_{false};

  // Config knob - Minimum delay (in milliseconds) to be incurred before
  // deleting a a route (both unicast and mpls).
  const std::chrono::milliseconds routeDeleteDelay_{0};

  // Thrift client connection to switch FIB Agent using which we actually
  // manipulate routes.
  folly::EventBase evb_;
  folly::AsyncSocket* socket_{nullptr};
  std::unique_ptr<thrift::FibServiceAsyncClient> client_{nullptr};

  // Callback timer to program routes to SwitchAgent. The updates to agent
  // would be based on RouteState. It'll handle all cases for route update e.g.
  // - Sync initial route database
  // - Program newly received route update
  // - Retry static routes
  // - Retry failed route updates
  // - Program delete updats which are in pending state
  // We trigger this timer with ExponentialBackoff to ease up things if
  // programming keeps failing.
  std::unique_ptr<folly::AsyncTimeout> retryRoutesTimer_{nullptr};
  ExponentialBackoff<std::chrono::milliseconds> retryRoutesExpBackoff_;

  // periodically send alive msg to switch agent
  std::unique_ptr<folly::AsyncTimeout> keepAliveTimer_{nullptr};

  // Queues to publish programmed incremental IP/label routes or those from Fib
  // sync. (Fib streaming)
  messaging::ReplicateQueue<DecisionRouteUpdate>& fibRouteUpdatesQueue_;

  // Latest aliveSince heard from FibService. If the next one is different then
  // it means that FibAgent has restarted and we need to perform sync.
  int64_t latestAliveSince_{0};

  // Open/R ClientID for programming routes
  const int16_t kFibId_{static_cast<int16_t>(thrift::FibClient::OPENR)};

  // Semaphore to serialize route programming across multiple fibers & async
  // timers. e.g. static route updates queue, decision route updates queue and
  // route programming retry timers
  // NOTE: We initialize with a single slot for exclusive locking
  folly::fibers::Semaphore updateRoutesSemaphore_{1};

  // Queue to publish the event log
  messaging::ReplicateQueue<LogSample>& logSampleQueue_;
};

} // namespace openr
