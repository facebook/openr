/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <openr/decision/LinkState.h>
#include <openr/decision/PrefixState.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/RouteUpdate.h>

namespace openr {

using StaticMplsRoutes = std::unordered_map<int32_t, RibMplsEntry>;
using StaticUnicastRoutes =
    std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>;
using Metric = openr::LinkStateMetric;
using BestNextHopMetrics = std::pair<
    Metric /* minimum metric to destination */,
    std::unordered_map<
        std::string /* nextHopNodeName */,
        Metric /* the distance from the nexthop to the dest */>>;

/**
 * Captures the route selection result. Especially highlights
 * - Best announcing `pair<Node, Area>`
 * - All selected entries `list<pair<Node, Area>>`
 */
struct RouteSelectionResult {
  // Representing the selected set of `<Node, Area>`.
  // NOTE: Using `std::set` helps ensuring uniqueness and ease code for electing
  // unique `<Node, Area>` in some-cases.
  std::set<NodeAndArea> allNodeAreas;

  // The `pair<Node, Area>` with best metrics. This should be used for
  // redistribution across areas.
  NodeAndArea bestNodeArea;

  /*
   * This is the flag to indicate is the best node selected is DRAINED(soft or
   * hard). The flag will be used to set the `drain_metric` inside
   * `thrift::PrefixEntry`.
   */
  bool isBestNodeDrained{false};

  /*
   * Function to check if provide node is one of the selected nodes.
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
      tRouteDb.unicastRoutes()->emplace_back(entry.toThrift());
    }
    // mpls routes
    for (const auto& [_, entry] : mplsRoutes) {
      tRouteDb.mplsRoutes()->emplace_back(entry.toThrift());
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

// The class to compute shortest-paths using Dijkstra algorithm
class SpfSolver {
 public:
  // these need to be defined in the .cpp so they can refer
  // to the actual implementation of SpfSolverImpl
  SpfSolver(
      const std::string& myNodeName,
      bool enableV4,
      bool enableBestRouteSelection = false,
      bool v4OverV6Nexthop = false);
  ~SpfSolver();

  //
  // util function to update IP static route
  //

  void updateStaticUnicastRoutes(
      const std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>&
          unicastRoutesToUpdate,
      const std::vector<folly::CIDRNetwork>& unicastRoutesToDelete);

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
      folly::CIDRNetwork const& prefix);

  std::unordered_map<folly::CIDRNetwork, RouteSelectionResult> const&
  getBestRoutesCache() const {
    return bestRoutesCache_;
  }

 private:
  // no-copy
  SpfSolver(SpfSolver const&) = delete;
  SpfSolver& operator=(SpfSolver const&) = delete;

  /*
   * Structure which holds the results of per area spf next-hop selection
   * for a single prefix
   */
  struct SpfAreaResults {
    // metric of the shortest path within the area
    LinkStateMetric bestMetric{0};
    // selected next-hops within the area
    std::unordered_set<thrift::NextHopThrift> nextHops;
  };

  /*
   * [Route Selection]:
   *
   * Performs best route selection from received route announcements of the
   * destination prefix.
   *
   * ATTN: there are various tie-breaking order for route selection. High level
   * speaking, we will perform:
   *  - 1st tie-breaker : drain_metric - prefer lower;
   *  - 2nd tie-breaker: path_preference - prefer higher;
   *  - 3rd tie-breaker: source_preference - prefer higher;
   */
  RouteSelectionResult selectBestRoutes(
      std::string const& myNodeName,
      folly::CIDRNetwork const& prefix,
      PrefixEntries& prefixEntries,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  /*
   * [Route Calculation]: shortest path forwarding
   *
   * Given the best route selection result, aka, the best node/nodes annoucing
   * the prefix, this util function will use SPF algorithm to find the shortest
   * paths(maybe ECMP) towards the destination prefix.
   */
  SpfAreaResults selectBestPathsSpf(
      std::string const& myNodeName,
      folly::CIDRNetwork const& prefix,
      RouteSelectionResult const& routeSelectionResult,
      const std::string& area,
      const LinkState& linkState);

  std::optional<RibUnicastEntry> addBestPaths(
      const std::string& myNodeName,
      const folly::CIDRNetwork& prefix,
      const RouteSelectionResult& routeSelectionResult,
      const PrefixEntries& prefixEntries,
      std::unordered_set<thrift::NextHopThrift>&& nextHops,
      const openr::LinkStateMetric shortestMetric,
      const bool localPrefixConsidered);

  std::optional<RibUnicastEntry> createRouteForPrefix(
      const std::string& myNodeName,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState,
      folly::CIDRNetwork const& prefix);

  // helper to get min nexthop for a prefix, used in selectKsp2
  std::optional<int64_t> getMinNextHopThreshold(
      RouteSelectionResult nodes, PrefixEntries const& prefixEntries);

  // [hard-drain]
  PrefixEntries filterHardDrainedNodes(
      PrefixEntries& prefixes,
      std::unordered_map<std::string, LinkState> const& areaLinkStates) const;

  // [soft-drain]
  std::unordered_set<NodeAndArea> getSoftDrainedNodes(
      PrefixEntries& prefixes,
      std::unordered_map<std::string, LinkState> const& areaLinkStates) const;

  bool isNodeDrained(
      const NodeAndArea& nodeArea,
      std::unordered_map<std::string, LinkState> const& areaLinkStates) const;

  // Give source node-name and dstNodeNames, this function returns the set of
  // nexthops towards these set of dstNodeNames
  BestNextHopMetrics getNextHopsWithMetric(
      const std::string& srcNodeName,
      const std::set<NodeAndArea>& dstNodeAreas,
      const LinkState& linkState);

  // This function converts best nexthop nodes to best nexthop adjacencies
  // which can then be passed to FIB for programming. It considers and
  // parallel link logic (tested by our UT)
  // If swap label is provided then it will be used to associate SWAP or PHP
  // mpls action
  std::unordered_set<thrift::NextHopThrift> getNextHopsThrift(
      const std::string& myNodeName,
      const std::set<NodeAndArea>& dstNodeAreas,
      bool isV4,
      const BestNextHopMetrics& bestNextHopMetrics,
      std::optional<int32_t> swapLabel,
      const std::string& area,
      const LinkState& linkState) const;

  // Collection to store static IP/MPLS routes
  StaticMplsRoutes staticMplsRoutes_;
  StaticUnicastRoutes staticUnicastRoutes_;

  // Cache of best route selection.
  // - Cleared when topology changes
  // - Updated for the prefix whenever a route is created for it
  std::unordered_map<folly::CIDRNetwork, RouteSelectionResult> bestRoutesCache_;

  const std::string myNodeName_;

  // is v4 enabled. If yes then Decision will forward v4 prefixes with v4
  // nexthops to Fib module for programming. Else it will just drop them.
  const bool enableV4_{false};

  const bool enableNodeSegmentLabel_{true};

  const bool enableBestRouteSelection_{false};

  // is v4 over v6 nexthop enabled. If yes then Decision will forward v4
  // prefixes with v6 nexthops to Fib module for programming. Else it will just
  // use v4 over v4 nexthop.
  const bool v4OverV6Nexthop_{false};
};
} // namespace openr
