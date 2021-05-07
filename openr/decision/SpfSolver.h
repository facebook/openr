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

#include <openr/decision/LinkState.h>
#include <openr/decision/PrefixState.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/RouteUpdate.h>

namespace openr {

using StaticMplsRoutes =
    std::unordered_map<int32_t, std::vector<thrift::NextHopThrift>>;
using StaticUnicastRoutes =
    std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>;

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

// The class to compute shortest-paths using Dijkstra algorithm
class SpfSolver {
 public:
  // these need to be defined in the .cpp so they can refer
  // to the actual implementation of SpfSolverImpl
  SpfSolver(
      const std::string& myNodeName,
      bool enableV4,
      bool enableNodeSegmentLabel,
      bool enableAdjacencyLabels,
      bool bgpDryRun = false,
      bool enableBestRouteSelection = false,
      bool v4OverV6Nexthop = false);
  ~SpfSolver();

  //
  // util function to update IP/MPLS static route
  //

  void updateStaticUnicastRoutes(
      const std::vector<RibUnicastEntry>& unicastRoutesToUpdate,
      const std::vector<folly::CIDRNetwork>& unicastRoutesToDelete);

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
      folly::CIDRNetwork const& prefix);

  std::unordered_map<folly::CIDRNetwork, BestRouteSelectionResult> const&
  getBestRoutesCache() const {
    return bestRoutesCache_;
  }

 private:
  // no-copy
  SpfSolver(SpfSolver const&) = delete;
  SpfSolver& operator=(SpfSolver const&) = delete;

  std::optional<RibUnicastEntry> createRouteForPrefix(
      const std::string& myNodeName,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState,
      folly::CIDRNetwork const& prefix);

  static std::pair<openr::LinkStateMetric, std::unordered_set<std::string>>
  getMinCostNodes(
      const LinkState::SpfResult& spfResult,
      const std::set<NodeAndArea>& dstNodeAreas);

  // Given prefixes and the nodes who announce it, get the ecmp routes.
  std::optional<RibUnicastEntry> selectBestPathsSpf(
      std::string const& myNodeName,
      folly::CIDRNetwork const& prefix,
      BestRouteSelectionResult const& bestRouteSelectionResult,
      PrefixEntries const& prefixEntries,
      bool const isBgp,
      thrift::PrefixForwardingType const& forwardingType,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState);

  // Given prefixes and the nodes who announce it, get the kspf2 routes, aka,
  // shortest paths and second shortest paths.
  std::optional<RibUnicastEntry> selectBestPathsKsp2(
      const std::string& myNodeName,
      const folly::CIDRNetwork& prefix,
      BestRouteSelectionResult const& bestRouteSelectionResult,
      PrefixEntries const& prefixEntries,
      bool isBgp,
      thrift::PrefixForwardingType const& forwardingType,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState);

  std::optional<RibUnicastEntry> addBestPaths(
      const std::string& myNodeName,
      const folly::CIDRNetwork& prefixThrift,
      const BestRouteSelectionResult& bestRouteSelectionResult,
      const PrefixEntries& prefixEntries,
      const bool isBgp,
      std::unordered_set<thrift::NextHopThrift>&& nextHops);

  // helper function to find the nodes for the nexthop for bgp route
  BestRouteSelectionResult runBestPathSelectionBgp(
      folly::CIDRNetwork const& prefix,
      PrefixEntries const& prefixEntries,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  /**
   * Performs best route selection from received route announcements
   */
  BestRouteSelectionResult selectBestRoutes(
      std::string const& myNodeName,
      folly::CIDRNetwork const& prefix,
      PrefixEntries const& prefixEntries,
      bool const hasBgp,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  // helper to get min nexthop for a prefix, used in selectKsp2
  std::optional<int64_t> getMinNextHopThreshold(
      BestRouteSelectionResult nodes, PrefixEntries const& prefixEntries);

  // Helper to filter overloaded nodes for anycast addresses
  //
  // TODO: This should go away, once Open/R policy is in place. The overloaded
  // nodes will stop advertising specific prefixes if they're overloaded
  BestRouteSelectionResult maybeFilterDrainedNodes(
      BestRouteSelectionResult&& result,
      std::unordered_map<std::string, LinkState> const& areaLinkStates) const;

  // Give source node-name and dstNodeNames, this function returns the set of
  // nexthops towards these set of dstNodeNames
  std::pair<
      openr::LinkStateMetric /* minimum metric to destination */,
      std::unordered_map<
          std::pair<std::string /* nextHopNodeName */, std::string /* dest */>,
          openr::
              LinkStateMetric /* the distance from the nexthop to the dest */>>
  getNextHopsWithMetric(
      const std::string& srcNodeName,
      const std::set<NodeAndArea>& dstNodeAreas,
      bool perDestination,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  // This function converts best nexthop nodes to best nexthop adjacencies
  // which can then be passed to FIB for programming. It considers and
  // parallel link logic (tested by our UT)
  // If swap label is provided then it will be used to associate SWAP or PHP
  // mpls action
  std::unordered_set<thrift::NextHopThrift> getNextHopsThrift(
      const std::string& myNodeName,
      const std::set<NodeAndArea>& dstNodeAreas,
      bool isV4,
      bool v4OverV6Nexthop,
      bool perDestination,
      const openr::LinkStateMetric minMetric,
      std::unordered_map<
          std::pair<std::string, std::string>,
          openr::LinkStateMetric> nextHopNodes,
      std::optional<int32_t> swapLabel,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixEntries const& prefixEntries = {}) const;

  // Collection to store static IP/MPLS routes
  StaticMplsRoutes staticMplsRoutes_;
  StaticUnicastRoutes staticUnicastRoutes_;

  // Cache of best route selection.
  // - Cleared when topology changes
  // - Updated for the prefix whenever a route is created for it
  std::unordered_map<folly::CIDRNetwork, BestRouteSelectionResult>
      bestRoutesCache_;

  const std::string myNodeName_;

  // is v4 enabled. If yes then Decision will forward v4 prefixes with v4
  // nexthops to Fib module for programming. Else it will just drop them.
  const bool enableV4_{false};

  const bool enableNodeSegmentLabel_{true};

  const bool enableAdjacencyLabels_{true};

  const bool bgpDryRun_{false};

  const bool enableBestRouteSelection_{false};

  // is v4 over v6 nexthop enabled. If yes then Decision will forward v4
  // prefixes with v6 nexthops to Fib module for programming. Else it will just
  // use v4 over v4 nexthop.
  const bool v4OverV6Nexthop_{false};
};
} // namespace openr
