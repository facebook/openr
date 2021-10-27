/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncTimeout.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AsyncDebounce.h>
#include <openr/common/AsyncThrottle.h>
#include <openr/common/MplsUtil.h>
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
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>
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

//
// The decision thread announces FIB updates for myNodeName every time
// there is a change in LSDB. The announcements are made on a PUB socket. At
// the same time, it listens on a REP socket to respond with the recent
// FIB state if requested by clients.
//
// On the "client" side of things, it uses REQ socket to request a full dump
// of link-state information from KvStore, and before that it subscribes to
// the PUB address of the KvStore to receive ongoing LSDB updates from KvStore.
//
// The prefix/adjacency Db markers are used to find the keys in KvStore that
// correspond to the prefix information or link state information. This way
// we do not need to try and parse the values to tell that. For example,
// the key name could be "adj:router1" or "prefix:router2" to tell of
// the AdjacencyDatabase of router1 and PrefixDatabase of router2
//

class Decision : public OpenrEventBase {
 public:
  Decision(
      std::shared_ptr<const Config> config,
      messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue,
      messaging::RQueue<DecisionRouteUpdate> staticRouteUpdatesQueue,
      messaging::ReplicateQueue<DecisionRouteUpdate>& routeUpdatesQueue);

  virtual ~Decision() = default;

  /*
   * Retrieve routeDb from specified node.
   * If empty nodename specified, will return routeDb of its own
   */
  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>> getDecisionRouteDb(
      std::string nodeName);

  /*
   * Retrieve AdjacencyDatabase for all nodes in all areas
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
  getDecisionAdjacenciesFiltered(thrift::AdjacenciesFilter filter = {});

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

  // Process thrift publication from KvStore
  void processPublication(thrift::Publication&& thriftPub);

  // Process publication from PrefixManager
  void processStaticRoutesUpdate(DecisionRouteUpdate&& routeUpdate);

  // Openr config
  std::shared_ptr<const Config> config_;

  // Callback timer used on startup to publish routes after
  // gracefulRestartDuration
  std::unique_ptr<folly::AsyncTimeout> coldStartTimer_{nullptr};

  /**
   * Rebuild all routes and send out update delta. Check current pendingUpdates_
   * to decide which routes need rebuilding, otherwise rebuild all. Use
   * pendingUpdates_.perfEvents() in the sent route delta appended with param
   * event before rebuild and "ROUTE_UPDATE" after.
   */
  void rebuildRoutes(std::string const& event);

  // Trigger initial route build in OpenR initialization process.
  void triggerInitialBuildRoutes();

  void sendRouteUpdate(
      DecisionRouteDb&& routeDb,
      std::optional<thrift::PerfEvents>&& perfEvents);

  // node to prefix entries database for nodes advertising per prefix keys
  std::optional<thrift::PrefixDatabase> updateNodePrefixDatabase(
      const std::string& key, const thrift::PrefixDatabase& prefixDb);

  // cached routeDb
  DecisionRouteDb routeDb_;

  // Queue to publish route changes
  messaging::ReplicateQueue<DecisionRouteUpdate>& routeUpdatesQueue_;

  // Pointer to RibPolicy
  std::unique_ptr<RibPolicy> ribPolicy_;

  // Timer associated with RibPolicy. Triggered when ribPolicy is expired. This
  // aims to revert the policy effects on programmed routes.
  std::unique_ptr<folly::AsyncTimeout> ribPolicyTimer_;

  // the pointer to the SPF path calculator
  std::unique_ptr<SpfSolver> spfSolver_;

  // per area link states
  std::unordered_map<std::string, LinkState> areaLinkStates_;

  // global prefix state
  PrefixState prefixState_;

  apache::thrift::CompactSerializer serializer_;

  // base interval to submit to monitor with (jitter will be added)
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

  // Boolean flag indicating whether KvStore synced signal is received in OpenR
  // initialization procedure.
  bool initialKvStoreSynced_{false};

  /*
   * Set of prefix types whose static routes Decision awaits before initial RIB
   * computation in OpenR initialization procedure. This would be populated
   * based on config,
   * - `VIP` is added if VIP plugin is enabled in config.
   * - `BGP` is added if BgpRib genereates static MPLS label routes, aka, BGP
   *   peering enable, AddPath feature enabled, and segment routing enabled.
   * - `CONFIG` is added if config originated prefixes are enabled in config.
   *
   * As we receive the first static routes from these types we remove them from
   * this set. Empty set indicates routes of all expected types are received.
   */
  std::unordered_set<thrift::PrefixType> unreceivedRouteTypes_{};
};

} // namespace openr
