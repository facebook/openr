/**
 * Copyright (c) 2014-present, Facebook, Inc.
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
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/decision/LinkState.h>
#include <openr/decision/PrefixState.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/RibPolicy.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

using StaticMplsRoutes =
    std::unordered_map<int32_t, std::vector<thrift::NextHopThrift>>;
using StaticUnicastRoutes =
    std::unordered_map<thrift::IpPrefix, std::vector<thrift::NextHopThrift>>;

/**
 * Captures the best route selection result. Especially highlights
 * - Best announcing `pair<area, node>`
 * - All best entries `list<pair<area, node>>`
 */
struct BestRouteSelectionResult {
  // TODO: Remove once we move to metrics selection
  bool success{false};

  // Representing all `<Node, Area>` pair announcing the best-metrics
  // NOTE: Using `std::set` helps ensuring uniqueness and ease code for electing
  // the best entry in some-cases.
  std::set<NodeAndArea> allNodeAreas;

  // The best entry among all entries with best-metrics. This should be used
  // for re-distributing across areas.
  NodeAndArea bestNodeArea;

  /**
   * Function to check if provide node is one of the best node
   */
  bool
  hasNode(const std::string& node) const {
    for (auto& [bestNode, _] : allNodeAreas) {
      if (node == bestNode) {
        return true;
      }
    }
    return false;
  }
};

class DecisionRouteDb {
 public:
  std::unordered_map<folly::CIDRNetwork /* prefix */, RibUnicastEntry>
      unicastRoutes;
  std::unordered_map<int32_t /* label */, RibMplsEntry> mplsRoutes;

  // calculate the delta between this and newDb. Note, this method is const;
  // We are not actually updating here. We may mutate the DecisionRouteUpdate in
  // some way before calling update with it
  DecisionRouteUpdate calculateUpdate(DecisionRouteDb&& newDb) const;

  // update the state of this with the DecisionRouteUpdate passed
  void update(DecisionRouteUpdate const& update);

  thrift::RouteDatabase
  toThrift() const {
    thrift::RouteDatabase tRouteDb;
    // unicast routes
    for (const auto& [_, entry] : unicastRoutes) {
      tRouteDb.unicastRoutes_ref()->emplace_back(entry.toThrift());
    }
    // mpls routes
    for (const auto& [_, entry] : mplsRoutes) {
      tRouteDb.mplsRoutes_ref()->emplace_back(entry.toThrift());
    }
    return tRouteDb;
  }

  // Assertion: no route for this prefix may already exist in the db
  void
  addUnicastRoute(RibUnicastEntry&& entry) {
    auto key = entry.prefix;
    CHECK(unicastRoutes.emplace(key, std::move(entry)).second);
  }

  // Assertion: no route for this label may already exist in the db
  void
  addMplsRoute(RibMplsEntry&& entry) {
    auto key = entry.label;
    CHECK(mplsRoutes.emplace(key, std::move(entry)).second);
  }
};

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

  std::unordered_set<thrift::IpPrefix> const&
  updatedPrefixes() const {
    return updatedPrefixes_;
  }

  void applyLinkStateChange(
      std::string const& nodeName,
      LinkState::LinkStateChange const& change,
      apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents);

  void applyPrefixStateChange(
      std::unordered_set<thrift::IpPrefix>&& change,
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
  std::unordered_set<thrift::IpPrefix> updatedPrefixes_;

  // local node name to determine action on linkAttributes change
  std::string myNodeName_;
};

} // namespace detail

// The class to compute shortest-paths using Dijkstra algorithm
class SpfSolver {
 public:
  // these need to be defined in the .cpp so they can refer
  // to the actual implementation of SpfSolverImpl
  SpfSolver(
      const std::string& myNodeName,
      bool enableV4,
      bool enableOrderedFib = false,
      bool bgpDryRun = false,
      bool enableBestRouteSelection = false);
  ~SpfSolver();

  //
  // The following methods talk to implementation so need to
  // be defined in the .cpp
  //

  void updateStaticUnicastRoutes(
      const std::vector<thrift::UnicastRoute>& unicastRoutesToUpdate,
      const std::vector<thrift::IpPrefix>& unicastRoutesToDelete);

  void updateStaticMplsRoutes(
      const std::vector<thrift::MplsRoute>& mplsRoutesToUpdate,
      const std::vector<int32_t>& mplsRoutesToDelete);

  // Build route database using given prefix and link states for a given
  // router, myNodeName
  // Returns std::nullopt if myNodeName doesn't have any prefix database
  std::optional<DecisionRouteDb> buildRouteDb(
      const std::string& myNodeName,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState);

  std::optional<RibUnicastEntry> createRouteForPrefixOrGetStaticRoute(
      const std::string& myNodeName,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState,
      thrift::IpPrefix const& prefix);

  std::unordered_map<thrift::IpPrefix, BestRouteSelectionResult> const&
  getBestRoutesCache() const;

 private:
  // no-copy
  SpfSolver(SpfSolver const&) = delete;
  SpfSolver& operator=(SpfSolver const&) = delete;

  // pointer to implementation class
  class SpfSolverImpl;
  std::unique_ptr<SpfSolverImpl> impl_;
};

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
      bool bgpDryRun,
      std::chrono::milliseconds debounceMinDur,
      std::chrono::milliseconds debounceMaxDur,
      messaging::RQueue<thrift::Publication> kvStoreUpdatesQueue,
      messaging::RQueue<thrift::RouteDatabaseDelta> staticRoutesUpdateQueue,
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

  // process publication from KvStore
  void processPublication(thrift::Publication&& thriftPub);

  // process publication from PrefixManager
  void processStaticRoutesUpdate(
      thrift::RouteDatabaseDelta&& staticRoutesDelta);

  // openr config
  std::shared_ptr<const Config> config_;

  // callback timer used on startup to publish routes after
  // gracefulRestartDuration
  std::unique_ptr<folly::AsyncTimeout> coldStartTimer_{nullptr};

  /**
   * Rebuild all routes and send out update delta. Check current pendingUpdates_
   * to decide which routes need rebuilding, otherwise rebuild all. Use
   * pendingUpdates_.perfEvents() in the sent route delta appended with param
   * event before rebuild and "ROUTE_UPDATE" after.
   */
  void rebuildRoutes(std::string const& event);

  // decremnts holds and send any resulting output, returns true if any
  // linkstate has remaining holds
  bool decrementOrderedFibHolds();

  void sendRouteUpdate(
      DecisionRouteDb&& routeDb,
      std::optional<thrift::PerfEvents>&& perfEvents);

  std::chrono::milliseconds getMaxFib();

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

  // For orderedFib prgramming, we keep track of the fib programming times
  // across the network
  std::unordered_map<std::string, std::chrono::milliseconds> fibTimes_;

  apache::thrift::CompactSerializer serializer_;

  // base interval to submit to monitor with (jitter will be added)
  std::chrono::seconds monitorSyncInterval_{0};

  // Timer for submitting to monitor periodically
  std::unique_ptr<folly::AsyncTimeout> monitorTimer_{nullptr};

  // Timer for decrementing link holds for ordered fib programming
  std::unique_ptr<folly::AsyncTimeout> orderedFibTimer_{nullptr};

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
};

} // namespace openr
