/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncTimeout.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AsyncDebounce.h>
#include <openr/common/AsyncThrottle.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/decision/LinkState.h>
#include <openr/decision/PrefixState.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/RibPolicy.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/decision/SpfSolver.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

namespace detail {
/**
 * Keep track of hash for pending SPF calculation because of certain
 * updates in graph.
 * Out of all buffered applications we try to keep the perf events for the
 * oldest appearing event.
 */
class DecisionPendingUpdates {
 public:
  explicit DecisionPendingUpdates(std::string const& myNodeName)
      : myNodeName_(myNodeName) {}

  void
  setNeedsFullRebuild() {
    needsFullRebuild_ = true;
  }

  bool
  needsFullRebuild() const {
    return needsFullRebuild_;
  }

  bool
  needsRouteUpdate() const {
    return needsFullRebuild() || !updatedPrefixes_.empty();
  }

  std::unordered_set<folly::CIDRNetwork> const&
  updatedPrefixes() const {
    return updatedPrefixes_;
  }

  void applyLinkStateChange(
      std::string const& nodeName,
      LinkState::LinkStateChange const& change,
      apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents);

  void applyPrefixStateChange(
      std::unordered_set<folly::CIDRNetwork>&& change,
      apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents);

  void reset();

  void addEvent(std::string const& eventDescription);

  std::optional<thrift::PerfEvents> const&
  perfEvents() const {
    return perfEvents_;
  }

  std::optional<thrift::PerfEvents> moveOutEvents();

  uint32_t
  getCount() const {
    return count_;
  }

 private:
  void addUpdate(
      apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents);

  // tracks how many updates are part of this batch
  uint32_t count_{0};

  // oldest perfEvents list in the batch
  std::optional<thrift::PerfEvents> perfEvents_;

  // set if we need to rebuild all routes
  bool needsFullRebuild_{false};

  // track prefixes that have changed in this batch
  std::unordered_set<folly::CIDRNetwork> updatedPrefixes_;

  // local node name to determine action on linkAttributes change
  std::string myNodeName_;
};

} // namespace detail

/**
 * Decision handles RIB (routes) computation and sends to FIB for programming.
 * RIB computation is triggered in following events,
 * - Link State updates received from KvStore publications
 * - Prefix announcement updates from KvStore publications
 * - Static route updates from other modules (PrefixManager, BgpSpeaker)
 * - RibPolicy updates from API `setRibPolicy`.
 *
 * Decision also provides APIs to retrieve route/adjacency database and
 * RibPolicy.
 *
 * The prefix/adjacency Db markers are used to find the keys in KvStore that
 * correspond to the prefix information or link state information. This way
 * we do not need to try and parse the values to tell that. For example,
 * the key name could be "adj:router1" or "prefix:router2" to tell of
 * the AdjacencyDatabase of router1 and PrefixDatabase of router2
 */

class Decision : public OpenrEventBase {
 public:
  Decision(
      std::shared_ptr<const Config> config,
      // Queue for receiving peer updates
      messaging::RQueue<PeerEvent> peerUpdatesQueue,
      // Queue for receiving KvStore publications
      messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue,
      // Queue for receiving static route updates
      messaging::RQueue<DecisionRouteUpdate> staticRouteUpdatesQueue,
      // Queue for publishing route updates
      messaging::ReplicateQueue<DecisionRouteUpdate>& routeUpdatesQueue);

  ~Decision();

  /**
   * Override stop method of OpenrEventBase
   */
  void stop() override;

  /*
   * Retrieve device drain information for the input node name.
   * If empty nodename specified, will return drained state of its own.
   */
  folly::SemiFuture<std::unique_ptr<thrift::OpenrDrainState>>
  getDecisionDrainState(std::string nodeName = "");

  /*
   * Retrieve routeDb from specified node.
   * If empty nodename specified, will return routeDb of its own
   */
  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>> getDecisionRouteDb(
      std::string nodeName = "");

  /*
   * Retrieve AdjacencyDatabase for all nodes in all areas.
   * DEPRECATED. Perfer getDecisionAreaAdjacenciesFiltered to return the areas
   * as well.
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
  getDecisionAdjacenciesFiltered(thrift::AdjacenciesFilter filter = {});

  /*
   * Retrieve area and AdjacencyDatabase for all nodes in all areas
   */
  folly::SemiFuture<std::unique_ptr<
      std::map<std::string, std::vector<thrift::AdjacencyDatabase>>>>
  getDecisionAreaAdjacenciesFiltered(thrift::AdjacenciesFilter filter = {});

  /*
   * Retrieve received routes along with best route selection output.
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>
  getReceivedRoutesFiltered(thrift::ReceivedRouteFilter filter);

  /*
   * Set new or replace existing RibPolicy. This will trigger the new policy
   * run against computed routes and delta will be published.
   */
  folly::SemiFuture<folly::Unit> setRibPolicy(
      thrift::RibPolicy const& ribPolicy);

  /*
   * Get the current RibPolicy instance. Throws exception if RibPolicy is not
   * set yet.
   */
  folly::SemiFuture<thrift::RibPolicy> getRibPolicy();

  /*
   * Clear the current RibPolicy instance. Throws exception if RibPolicy is not
   * set yet.
   */
  folly::SemiFuture<folly::Unit> clearRibPolicy();

  // periodically called by counterUpdateTimer_, exposed publicly for testing
  void updateGlobalCounters() const;

  void updateCounters(
      std::string key,
      std::chrono::steady_clock::time_point start,
      std::chrono::steady_clock::time_point end) const;

 private:
  Decision(Decision const&) = delete;
  Decision& operator=(Decision const&) = delete;

  // Process peer updates
  void processPeerUpdates(PeerEvent&& event);

  /*
   * [Link-State Database(LSDB) Management]
   *
   * Decision consumes the publication from KvStore for LSDB
   * update(add/delete/etc.) and update its internal database of: 1)
   * PREFIX_DATABASE 2) ADJACENCY_DATABASE
   *
   * It provides multiple util functions to process the updates including:
   *    1) updateKeyInLsdb  - process key adding/updating
   *    2) deleteKeyFromLsdb - process key deletion
   */
  void processPublication(thrift::Publication&& thriftPub);

  void updateKeyInLsdb(
      const std::string& area,
      LinkState& areaLinkState,
      const std::string& key,
      const thrift::Value& rawVal);

  void deleteKeyFromLsdb(
      const std::string& area,
      LinkState& areaLinkState,
      const std::string& key);

  // Process publication from PrefixManager
  void processStaticRoutesUpdate(DecisionRouteUpdate&& routeUpdate);

  /**
   * Update pending adjacency for up peers to unblock initial RIB computation.
   */
  void updatePendingAdjacency(
      const std::string& area, const thrift::AdjacencyDatabase& newAdjacencyDb);

  /*
   * Rebuild all routes and send out update delta. Check current pendingUpdates_
   * to decide which routes need rebuilding, otherwise rebuild all. Use
   * pendingUpdates_.perfEvents() in the sent route delta appended with param
   * event before rebuild and "ROUTE_UPDATE" after.
   */
  void rebuildRoutes(std::string const& event);

  /*
   * Return true if unblocked for self adjacency signal
   */
  bool unblockSelfAdjSync();

  /*
   * Return true if all conditions of initial routes build are fulfilled.
   */
  bool unblockInitialRoutesBuild();

  // Trigger initial route build in OpenR initialization process.
  void triggerInitialBuildRoutes();

  // node to prefix entries database for nodes advertising per prefix keys
  std::optional<thrift::PrefixDatabase> updateNodePrefixDatabase(
      const std::string& key, const thrift::PrefixDatabase& prefixDb);

  // Openr config
  std::shared_ptr<const Config> config_;

  // Save received Rib policy to file
  void saveRibPolicy();

  // Read persisted Rib policy from file
  void readRibPolicy();

  // Force initial routes build if it's not done yet.
  void forceInitialRoutesBuild() noexcept;

  // cached routeDb
  DecisionRouteDb routeDb_;

  // Queue to publish route changes
  messaging::ReplicateQueue<DecisionRouteUpdate>& routeUpdatesQueue_;

  // Pointer to RibPolicy
  std::unique_ptr<RibPolicy> ribPolicy_;

  // Timer associated with RibPolicy. Triggered when ribPolicy is expired. This
  // aims to revert the policy effects on programmed routes.
  std::unique_ptr<folly::AsyncTimeout> ribPolicyTimer_;

  // The pointer to the SPF path calculator
  std::unique_ptr<SpfSolver> spfSolver_;

  // Per area link states
  std::unordered_map<std::string, LinkState> areaLinkStates_;

  // Global prefix state
  PrefixState prefixState_;

  apache::thrift::CompactSerializer serializer_;

  // Base interval to submit to monitor with (jitter will be added)
  std::chrono::seconds monitorSyncInterval_{0};

  // Timer for submitting to monitor periodically
  std::unique_ptr<folly::AsyncTimeout> monitorTimer_{nullptr};

  // Timer for updating and submitting counters periodically
  std::unique_ptr<folly::AsyncTimeout> counterUpdateTimer_{nullptr};

  // this node's name and the key markers
  const std::string myNodeName_;

  // store rebuildRoutes to-do status and perf events
  detail::DecisionPendingUpdates pendingUpdates_;

  /**
   * Debounced trigger for rebuildRoutes invoked by input paths kvstore update
   * queue and static routes update queue
   */
  AsyncDebounce<std::chrono::milliseconds> rebuildRoutesDebounced_;

  /*
   * Baton for synchronization between ProcessPeerUpdates and ProcessPublication
   * fibers.
   * post() when  all initial peers are received in Open/R initialization
   * process.
   */
  folly::fibers::Baton initialPeersReceivedBaton_;

  /*
   * Adjacency not received in each area. OpenR initialization process will be
   * blocked until having received adjacency with all live peers.
   * key: area
   * val: set of unreceived unidirectional adjacency (A->B). For one live peer,
   * both peer->theNode and theNode->peer adj are expected to be received.
   */
  std::unordered_map<
      std::string,
      std::unordered_set<std::pair<std::string, std::string>>>
      areaToPendingAdjacency_;

  /**
   * Debounced trigger for saveRibPolicy invoked by setRibPolicy.
   */
  AsyncDebounce<std::chrono::milliseconds> saveRibPolicyDebounced_;

  /*
   * Boolean flag indicating whether KvStore synced signal is received in OpenR
   * initialization procedure.
   */
  bool initialKvStoreSynced_{false};

  /*
   * Boolean flag indicating whether SelfOriginated Keys synced signal
   * received in OpenR intialization procedure.
   */
  bool initialSelfAdjSynced_{false};
  bool enableInitOptimization_{false};

  /**
   * Boolean flag indicating whether rib policy is read from persisted file in
   * OpenR initialization procedure. Set as true if such file does not exist.
   */
  bool initialRibPolicyRead_{false};

  /* Whether to unblock the initial routes build. See
   * unblockInitialRoutesBuild(). */
  bool unblockInitialRoutes_{false};

  /* Timeout to force initial routes build. */
  const folly::not_null_unique_ptr<folly::AsyncTimeout>
      unblockInitialRoutesTimeout_;

  /*
   * Set of prefix types whose static routes Decision awaits before initial RIB
   * computation in OpenR initialization procedure. This would be populated
   * based on config,
   * - `VIP` is added if VIP plugin is enabled in config.
   * - `CONFIG` is added if config originated prefixes are enabled in config.
   *
   * As we receive the first static routes from these types we remove them from
   * this set. Empty set indicates routes of all expected types are received.
   */
  std::unordered_set<thrift::PrefixType> unreceivedRouteTypes_{};
};

} // namespace openr
