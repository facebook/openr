/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Decision.h"

#include <chrono>
#include <set>
#include <string>
#include <unordered_set>

#include <fb303/ServiceData.h>
#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#if FOLLY_USE_SYMBOLIZER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/PrefixState.h>
#include <openr/decision/RibEntry.h>

namespace fb303 = facebook::fb303;

using apache::thrift::can_throw;
using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;
using Metric = openr::LinkStateMetric;
using SpfResult = openr::LinkState::SpfResult;

namespace openr {

namespace detail {

void
DecisionPendingUpdates::applyLinkStateChange(
    std::string const& nodeName,
    LinkState::LinkStateChange const& change,
    apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents) {
  needsFullRebuild_ |=
      (change.topologyChanged || change.nodeLabelChanged ||
       // we only need a full rebuild if link attributes change locally
       // this would be a nexthop or link label change
       (change.linkAttributesChanged && nodeName == myNodeName_));
  addUpdate(perfEvents);
}

void
DecisionPendingUpdates::applyPrefixStateChange(
    std::unordered_set<thrift::IpPrefix>&& change,
    apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents) {
  updatedPrefixes_.merge(std::move(change));
  addUpdate(perfEvents);
}

void
DecisionPendingUpdates::reset() {
  count_ = 0;
  perfEvents_ = std::nullopt;
  needsFullRebuild_ = false;
  updatedPrefixes_.clear();
}

void
DecisionPendingUpdates::addEvent(std::string const& eventDescription) {
  if (perfEvents_) {
    addPerfEvent(*perfEvents_, myNodeName_, eventDescription);
  }
}

std::optional<thrift::PerfEvents>
DecisionPendingUpdates::moveOutEvents() {
  std::optional<thrift::PerfEvents> events = std::move(perfEvents_);
  perfEvents_ = std::nullopt;
  return events;
}
void
DecisionPendingUpdates::addUpdate(
    apache::thrift::optional_field_ref<thrift::PerfEvents const&> perfEvents) {
  ++count_;

  // Update local copy of perf evens if it is newer than the one to be added
  // We do debounce (batch updates) for recomputing routes and in order to
  // measure convergence performance, it is better to use event which is
  // oldest.
  if (!perfEvents_ ||
      (perfEvents &&
       *perfEvents_->events_ref()->front().unixTs_ref() >
           *perfEvents->events_ref()->front().unixTs_ref())) {
    // if we don't have any perf events for this batch and this update also
    // doesn't have anything, let's start building the event list from now
    perfEvents_ = perfEvents ? *perfEvents : thrift::PerfEvents{};
    addPerfEvent(*perfEvents_, myNodeName_, "DECISION_RECEIVED");
  }
}
} // namespace detail

DecisionRouteUpdate
DecisionRouteDb::calculateUpdate(DecisionRouteDb&& newDb) const {
  DecisionRouteUpdate delta;

  // unicastRoutesToUpdate
  for (auto& [prefix, entry] : newDb.unicastRoutes) {
    const auto& search = unicastRoutes.find(prefix);
    if (search == unicastRoutes.end() || search->second != entry) {
      // new prefix, or prefix entry changed
      delta.addRouteToUpdate(std::move(entry));
    }
  }

  // unicastRoutesToDelete
  for (auto& [prefix, _] : unicastRoutes) {
    if (!newDb.unicastRoutes.count(prefix)) {
      delta.unicastRoutesToDelete.emplace_back(prefix);
    }
  }

  // mplsRoutesToUpdate
  for (const auto& [label, entry] : newDb.mplsRoutes) {
    const auto& search = mplsRoutes.find(label);
    if (search == mplsRoutes.end() || search->second != entry) {
      delta.mplsRoutesToUpdate.emplace_back(entry);
    }
  }

  // mplsRoutesToDelete
  for (auto const& [label, _] : mplsRoutes) {
    if (!newDb.mplsRoutes.count(label)) {
      delta.mplsRoutesToDelete.emplace_back(label);
    }
  }
  return delta;
}

void
DecisionRouteDb::update(DecisionRouteUpdate const& update) {
  for (auto const& prefix : update.unicastRoutesToDelete) {
    unicastRoutes.erase(prefix);
  }
  for (auto const& [_, entry] : update.unicastRoutesToUpdate) {
    unicastRoutes.insert_or_assign(entry.prefix, entry);
  }
  for (auto const& label : update.mplsRoutesToDelete) {
    mplsRoutes.erase(label);
  }
  for (auto const& entry : update.mplsRoutesToUpdate) {
    mplsRoutes.insert_or_assign(entry.label, entry);
  }
}

/**
 * Private implementation of the SpfSolver
 */
class SpfSolver::SpfSolverImpl {
 public:
  SpfSolverImpl(
      const std::string& myNodeName,
      bool enableV4,
      bool computeLfaPaths,
      bool enableOrderedFib,
      bool bgpDryRun,
      bool enableBestRouteSelection)
      : myNodeName_(myNodeName),
        enableV4_(enableV4),
        computeLfaPaths_(computeLfaPaths),
        enableOrderedFib_(enableOrderedFib),
        bgpDryRun_(bgpDryRun),
        enableBestRouteSelection_(enableBestRouteSelection) {
    // Initialize stat keys
    fb303::fbData->addStatExportType("decision.adj_db_update", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.incompatible_forwarding_type", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.no_route_to_label", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.no_route_to_prefix", fb303::COUNT);
    fb303::fbData->addStatExportType("decision.path_build_ms", fb303::AVG);
    fb303::fbData->addStatExportType("decision.prefix_db_update", fb303::COUNT);
    fb303::fbData->addStatExportType("decision.route_build_ms", fb303::AVG);
    fb303::fbData->addStatExportType("decision.route_build_runs", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.get_route_for_prefix", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.skipped_mpls_route", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.duplicate_node_label", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.skipped_unicast_route", fb303::COUNT);
    fb303::fbData->addStatExportType("decision.spf_ms", fb303::AVG);
    fb303::fbData->addStatExportType("decision.spf_runs", fb303::COUNT);
    fb303::fbData->addStatExportType("decision.errors", fb303::COUNT);
  }

  ~SpfSolverImpl() = default;

  //
  // util function to update IP/MPLS static route
  //

  void updateStaticUnicastRoutes(
      const std::vector<thrift::UnicastRoute>& unicastRoutesToUpdate,
      const std::vector<thrift::IpPrefix>& unicastRoutesToDelete);

  void updateStaticMplsRoutes(
      const std::vector<thrift::MplsRoute>& mplsRoutesToUpdate,
      const std::vector<int32_t>& mplsRoutesToDelete);

  //
  // best path calculation
  //

  // Build route database using global prefix database and cached SPF
  // computation from perspective of a given router.
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

  std::optional<RibUnicastEntry> createRouteForPrefix(
      const std::string& myNodeName,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState,
      thrift::IpPrefix const& prefix);

  std::unordered_map<thrift::IpPrefix, BestRouteSelectionResult> const&
  getBestRoutesCache() const {
    return bestRoutesCache_;
  }

  // helpers used in best path calculation
  static std::pair<Metric, std::unordered_set<std::string>> getMinCostNodes(
      const SpfResult& spfResult, const std::set<NodeAndArea>& dstNodeAreas);

  // spf counters
  void updateGlobalCounters();

 private:
  // no copy
  SpfSolverImpl(SpfSolverImpl const&) = delete;
  SpfSolverImpl& operator=(SpfSolverImpl const&) = delete;

  // Given prefixes and the nodes who announce it, get the ecmp routes.
  std::optional<RibUnicastEntry> selectBestPathsSpf(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      BestRouteSelectionResult const& bestRouteSelectionResult,
      PrefixEntries const& prefixEntries,
      bool const isBgp,
      thrift::PrefixForwardingType const& forwardingType,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState);

  // Given prefixes and the nodes who announce it, get the kspf routes.
  std::optional<RibUnicastEntry> selectBestPathsKsp2(
      const std::string& myNodeName,
      const thrift::IpPrefix& prefix,
      BestRouteSelectionResult const& bestRouteSelectionResult,
      PrefixEntries const& prefixEntries,
      bool isBgp,
      thrift::PrefixForwardingType const& forwardingType,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState);

  std::optional<RibUnicastEntry> addBestPaths(
      const std::string& myNodeName,
      const thrift::IpPrefix& prefixThrift,
      const BestRouteSelectionResult& bestRouteSelectionResult,
      const PrefixEntries& prefixEntries,
      const PrefixState& prefixState,
      const bool isBgp,
      std::unordered_set<thrift::NextHopThrift>&& nextHops);

  // helper function to find the nodes for the nexthop for bgp route
  BestRouteSelectionResult runBestPathSelectionBgp(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      PrefixEntries const& prefixEntries,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  /**
   * Performs best route selection from received route announcements
   */
  BestRouteSelectionResult selectBestRoutes(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
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
  // nexthops (along with LFA if enabled) towards these set of dstNodeNames
  std::pair<
      Metric /* minimum metric to destination */,
      std::unordered_map<
          std::pair<std::string /* nextHopNodeName */, std::string /* dest */>,
          Metric /* the distance from the nexthop to the dest */>>
  getNextHopsWithMetric(
      const std::string& srcNodeName,
      const std::set<NodeAndArea>& dstNodeAreas,
      bool perDestination,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  // This function converts best nexthop nodes to best nexthop adjacencies
  // which can then be passed to FIB for programming. It considers LFA and
  // parallel link logic (tested by our UT)
  // If swap label is provided then it will be used to associate SWAP or PHP
  // mpls action
  std::unordered_set<thrift::NextHopThrift> getNextHopsThrift(
      const std::string& myNodeName,
      const std::set<NodeAndArea>& dstNodeAreas,
      bool isV4,
      bool perDestination,
      const Metric minMetric,
      std::unordered_map<std::pair<std::string, std::string>, Metric>
          nextHopNodes,
      std::optional<int32_t> swapLabel,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixEntries const& prefixEntries = {}) const;

  // Collection to store static IP/MPLS routes
  StaticMplsRoutes staticMplsRoutes_;
  StaticUnicastRoutes staticUnicastRoutes_;

  // Cache of best route selection.
  // - Cleared when topology changes
  // - Updated for the prefix whenever a route is created for it
  std::unordered_map<thrift::IpPrefix, BestRouteSelectionResult>
      bestRoutesCache_;

  const std::string myNodeName_;

  // is v4 enabled. If yes then Decision will forward v4 prefixes with v4
  // nexthops to Fib module for programming. Else it will just drop them.
  const bool enableV4_{false};

  const bool computeLfaPaths_{false};

  const bool enableOrderedFib_{false};

  const bool bgpDryRun_{false};

  const bool enableBestRouteSelection_{false};
};

void
SpfSolver::SpfSolverImpl::updateStaticUnicastRoutes(
    const std::vector<thrift::UnicastRoute>& unicastRoutesToUpdate,
    const std::vector<thrift::IpPrefix>& unicastRoutesToDelete) {
  // Process IP routes to add or update
  for (const auto& unicastRoute : unicastRoutesToUpdate) {
    const auto prefix = *unicastRoute.dest_ref();
    staticUnicastRoutes_.insert_or_assign(prefix, *unicastRoute.nextHops_ref());

    VLOG(1)
        << "[STATIC UNICAST ROUTE ADVERTISEMENT] Prefix = " << toString(prefix)
        << ", NextHopsCount = " << unicastRoute.nextHops_ref()->size();
    for (auto const& nh : *unicastRoute.nextHops_ref()) {
      VLOG(2) << " via " << toString(nh);
    }
  }

  for (const auto& prefix : unicastRoutesToDelete) {
    // mark unicast entry to be deleted
    staticUnicastRoutes_.erase(prefix);

    VLOG(1) << "[STATIC UNICAST ROUTE WITHDRAW] Prefix = " << toString(prefix);
  }
}

void
SpfSolver::SpfSolverImpl::updateStaticMplsRoutes(
    const std::vector<thrift::MplsRoute>& mplsRoutesToUpdate,
    const std::vector<int32_t>& mplsRoutesToDelete) {
  // Process MPLS routes to add or update
  for (const auto& mplsRoute : mplsRoutesToUpdate) {
    const auto topLabel = *mplsRoute.topLabel_ref();
    staticMplsRoutes_.insert_or_assign(topLabel, *mplsRoute.nextHops_ref());

    VLOG(1) << "[STATIC MPLS ROUTE ADVERTISEMENT] Prefix = "
            << std::to_string(topLabel)
            << ", NextHopsCount = " << mplsRoute.nextHops_ref()->size();
    for (auto const& nh : *mplsRoute.nextHops_ref()) {
      VLOG(2) << " via " << toString(nh);
    }
  }

  for (const auto& topLabel : mplsRoutesToDelete) {
    staticMplsRoutes_.erase(topLabel);

    VLOG(1) << "[STATIC MPLS ROUTE WITHDRAW] Prefix = "
            << std::to_string(topLabel);
  }
}

std::optional<RibUnicastEntry>
SpfSolver::SpfSolverImpl::createRouteForPrefixOrGetStaticRoute(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState,
    thrift::IpPrefix const& prefix) {
  // route output from `PrefixState` has higher priority over
  // static unicast routes
  if (auto maybeRoute = createRouteForPrefix(
          myNodeName, areaLinkStates, prefixState, prefix)) {
    return maybeRoute;
  }

  // check `staticUnicastRoutes_`
  auto it = staticUnicastRoutes_.find(prefix);
  if (it != staticUnicastRoutes_.end()) {
    return RibUnicastEntry(
        toIPNetwork(it->first),
        std::unordered_set<thrift::NextHopThrift>{it->second.begin(),
                                                  it->second.end()});
  }
  return std::nullopt;
}

std::optional<RibUnicastEntry>
SpfSolver::SpfSolverImpl::createRouteForPrefix(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState,
    thrift::IpPrefix const& prefix) {
  fb303::fbData->addStatValue("decision.get_route_for_prefix", 1, fb303::COUNT);

  auto search = prefixState.prefixes().find(prefix);
  if (search == prefixState.prefixes().end()) {
    return std::nullopt;
  }
  auto const& allPrefixEntries = search->second;

  // Clear best route selection in prefix state
  bestRoutesCache_.erase(prefix);

  //
  // Create list of prefix-entries from reachable nodes only
  // NOTE: We're copying prefix-entries and it can be expensive. Using
  // pointers for storing prefix information can be efficient (CPU & Memory)
  //
  auto prefixEntries = folly::copy(allPrefixEntries);
  for (auto& [area, linkState] : areaLinkStates) {
    auto const& mySpfResult = linkState.getSpfResult(myNodeName);

    // Delete entries of unreachable nodes from prefixEntries
    for (auto it = prefixEntries.begin(); it != prefixEntries.end();) {
      const auto& [prefixNode, prefixArea] = it->first;
      if (area != prefixArea || mySpfResult.count(prefixNode)) {
        ++it; // retain
      } else {
        it = prefixEntries.erase(it); // erase the unreachable prefix entry
      }
    }
  }

  // Skip if no valid prefixes
  if (prefixEntries.empty()) {
    VLOG(3) << "Skipping route to " << toString(prefix)
            << " with no reachable node.";
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  // Sanity check for V4 prefixes
  const bool isV4Prefix = prefix.prefixAddress_ref()->addr_ref()->size() ==
      folly::IPAddressV4::byteCount();
  if (isV4Prefix && !enableV4_) {
    LOG(WARNING) << "Received v4 prefix " << toString(prefix)
                 << " while v4 is not enabled.";
    fb303::fbData->addStatValue(
        "decision.skipped_unicast_route", 1, fb303::COUNT);
    return std::nullopt;
  }

  // TODO: These variables are deprecated and will go away soon
  bool hasBGP = false, hasNonBGP = false, missingMv = false;

  // Does the current node advertises the prefix entry with prepend label
  bool hasSelfPrependLabel{true};

  // TODO: With new PrefixMetrics we no longer treat routes differently based
  // on their origin source aka `prefixEntry.type`
  for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
    bool isBGP = prefixEntry.type_ref().value() == thrift::PrefixType::BGP;
    hasBGP |= isBGP;
    hasNonBGP |= !isBGP;
    if (nodeAndArea.first == myNodeName) {
      hasSelfPrependLabel &= prefixEntry.prependLabel_ref().has_value();
    }
    if (isBGP and not prefixEntry.mv_ref().has_value()) {
      missingMv = true;
      LOG(ERROR) << "Prefix entry for prefix " << toString(prefix)
                 << " advertised by " << nodeAndArea.first << ", area "
                 << nodeAndArea.second
                 << " is of type BGP and missing the metric vector.";
    }
  }

  // TODO: With new PrefixMetrics we no longer treat routes differently based
  // on their origin source aka `prefixEntry.type`
  // skip adding route for BGP prefixes that have issues
  if (hasBGP) {
    if (hasNonBGP and not enableBestRouteSelection_) {
      LOG(ERROR) << "Skipping route for " << toString(prefix)
                 << " which is advertised with BGP and non-BGP type.";
      fb303::fbData->addStatValue(
          "decision.skipped_unicast_route", 1, fb303::COUNT);
      return std::nullopt;
    }
    if (missingMv) {
      LOG(ERROR) << "Skipping route for " << toString(prefix)
                 << " at least one advertiser is missing its metric vector.";
      fb303::fbData->addStatValue(
          "decision.skipped_unicast_route", 1, fb303::COUNT);
      return std::nullopt;
    }
  }

  // Perform best route selection from received route announcements
  const auto& bestRouteSelectionResult = selectBestRoutes(
      myNodeName, prefix, prefixEntries, hasBGP, areaLinkStates);
  if (not bestRouteSelectionResult.success) {
    return std::nullopt;
  }
  if (bestRouteSelectionResult.allNodeAreas.empty()) {
    LOG(WARNING) << "No route to BGP prefix " << toString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  // Set best route selection in prefix state
  bestRoutesCache_.insert_or_assign(prefix, bestRouteSelectionResult);

  // Skip adding route for prefixes advertised by this node. The originated
  // routes are already programmed on the system e.g. re-distributed from
  // other area, re-distributed from other protocols, interface-subnets etc.
  //
  // TODO: We program self advertise prefix only iff, we're advertising our
  // prefix-entry with the prepend label. Once we support multi-area routing,
  // we can deprecate the check of hasSelfPrependLabel
  if (bestRouteSelectionResult.hasNode(myNodeName) and !hasSelfPrependLabel) {
    return std::nullopt;
  }

  // Get the forwarding type and algorithm
  const auto [forwardingType, forwardingAlgo] =
      getPrefixForwardingTypeAndAlgorithm(
          prefixEntries, bestRouteSelectionResult.allNodeAreas);

  //
  // Route computation flow
  // - Switch on algorithm type
  // - Compute paths, algorithm type influences this step (ECMP or KSPF)
  // - Create next-hops from paths, forwarding type influences this step
  //
  switch (forwardingAlgo) {
  case thrift::PrefixForwardingAlgorithm::SP_ECMP:
    return selectBestPathsSpf(
        myNodeName,
        prefix,
        bestRouteSelectionResult,
        prefixEntries,
        hasBGP,
        forwardingType,
        areaLinkStates,
        prefixState);
  case thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP:
    return selectBestPathsKsp2(
        myNodeName,
        prefix,
        bestRouteSelectionResult,
        prefixEntries,
        hasBGP,
        forwardingType,
        areaLinkStates,
        prefixState);
  default:
    LOG(ERROR) << "Unknown prefix algorithm type "
               << apache::thrift::util::enumNameSafe(forwardingAlgo)
               << " for prefix " << toString(prefix);
    return std::nullopt;
  }
}

std::optional<DecisionRouteDb>
SpfSolver::SpfSolverImpl::buildRouteDb(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  bool nodeExist{false};
  for (const auto& [_, linkState] : areaLinkStates) {
    nodeExist |= linkState.hasNode(myNodeName);
  }
  if (not nodeExist) {
    return std::nullopt;
  }

  const auto startTime = std::chrono::steady_clock::now();
  fb303::fbData->addStatValue("decision.route_build_runs", 1, fb303::COUNT);

  DecisionRouteDb routeDb{};

  // Clear best route selection cache
  bestRoutesCache_.clear();

  // Create IPv4, IPv6 routes (includes IP -> MPLS routes)
  for (const auto& [prefix, _] : prefixState.prefixes()) {
    if (auto maybeRoute = createRouteForPrefix(
            myNodeName, areaLinkStates, prefixState, prefix)) {
      routeDb.addUnicastRoute(std::move(maybeRoute).value());
    }
  }

  // Create static unicast routes
  for (const auto& [prefix, nhs] : staticUnicastRoutes_) {
    if (routeDb.unicastRoutes.count(toIPNetwork(prefix))) {
      // ignore prefixes as prefixState has higher priority
      continue;
    }
    routeDb.addUnicastRoute(RibUnicastEntry(
        toIPNetwork(prefix),
        std::unordered_set<thrift::NextHopThrift>{nhs.begin(), nhs.end()}));
  }

  //
  // Create MPLS routes for all nodeLabel
  //
  std::unordered_map<int32_t, std::pair<std::string, RibMplsEntry>> labelToNode;
  for (const auto& [area, linkState] : areaLinkStates) {
    for (const auto& [_, adjDb] : linkState.getAdjacencyDatabases()) {
      const auto topLabel = *adjDb.nodeLabel_ref();
      // Top label is not set => Non-SR mode
      if (topLabel == 0) {
        continue;
      }
      // If mpls label is not valid then ignore it
      if (not isMplsLabelValid(topLabel)) {
        LOG(ERROR) << "Ignoring invalid node label " << topLabel << " of node "
                   << *adjDb.thisNodeName_ref();
        fb303::fbData->addStatValue(
            "decision.skipped_mpls_route", 1, fb303::COUNT);
        continue;
      }

      // There can be a temporary collision in node label allocation. Usually
      // happens when two segmented networks allocating labels from the same
      // range join together. In case of such conflict we respect the node label
      // of bigger node-ID
      auto iter = labelToNode.find(topLabel);
      if (iter != labelToNode.end()) {
        LOG(INFO) << "Found duplicate label " << topLabel << "from "
                  << iter->second.first << " " << *adjDb.thisNodeName_ref();
        fb303::fbData->addStatValue(
            "decision.duplicate_node_label", 1, fb303::COUNT);
        if (iter->second.first < *adjDb.thisNodeName_ref()) {
          continue;
        }
      }

      // Install POP_AND_LOOKUP for next layer
      if (*adjDb.thisNodeName_ref() == myNodeName) {
        thrift::NextHopThrift nh;
        *nh.address_ref() = toBinaryAddress(folly::IPAddressV6("::"));
        nh.area_ref() = area;
        nh.mplsAction_ref() =
            createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
        labelToNode.erase(topLabel);
        labelToNode.emplace(
            topLabel,
            std::make_pair(
                *adjDb.thisNodeName_ref(), RibMplsEntry(topLabel, {nh})));
        continue;
      }

      // Get best nexthop towards the node
      auto metricNhs = getNextHopsWithMetric(
          myNodeName,
          {{adjDb.thisNodeName_ref().value(), area}},
          false,
          areaLinkStates);
      if (metricNhs.second.empty()) {
        LOG(WARNING) << "No route to nodeLabel " << std::to_string(topLabel)
                     << " of node " << *adjDb.thisNodeName_ref();
        fb303::fbData->addStatValue(
            "decision.no_route_to_label", 1, fb303::COUNT);
        continue;
      }

      // Create nexthops with appropriate MplsAction (PHP and SWAP). Note that
      // all nexthops are valid for routing without loops. Fib is responsible
      // for installing these routes by making sure it programs least cost
      // nexthops first and of same action type (based on HW limitations)
      labelToNode.erase(topLabel);
      labelToNode.emplace(
          topLabel,
          std::make_pair(
              adjDb.thisNodeName_ref().value(),
              RibMplsEntry(
                  topLabel,
                  getNextHopsThrift(
                      myNodeName,
                      {{adjDb.thisNodeName_ref().value(), area}},
                      false,
                      false,
                      metricNhs.first,
                      metricNhs.second,
                      topLabel,
                      areaLinkStates))));
    }
  }

  for (auto& [_, nodeToEntry] : labelToNode) {
    routeDb.addMplsRoute(std::move(nodeToEntry.second));
  }

  //
  // Create MPLS routes for all of our adjacencies
  //
  for (const auto& [_, linkState] : areaLinkStates) {
    for (const auto& link : linkState.linksFromNode(myNodeName)) {
      const auto topLabel = link->getAdjLabelFromNode(myNodeName);
      // Top label is not set => Non-SR mode
      if (topLabel == 0) {
        continue;
      }
      // If mpls label is not valid then ignore it
      if (not isMplsLabelValid(topLabel)) {
        LOG(ERROR) << "Ignoring invalid adjacency label " << topLabel
                   << " of link " << link->directionalToString(myNodeName);
        fb303::fbData->addStatValue(
            "decision.skipped_mpls_route", 1, fb303::COUNT);
        continue;
      }

      routeDb.addMplsRoute(RibMplsEntry(
          topLabel,
          {createNextHop(
              link->getNhV6FromNode(myNodeName),
              link->getIfaceFromNode(myNodeName),
              link->getMetricFromNode(myNodeName),
              createMplsAction(thrift::MplsActionCode::PHP),
              link->getArea(),
              link->getOtherNodeName(myNodeName))}));
    }
  }

  //
  // Add MPLS static routes
  //
  for (const auto& [topLabel, nhs] : staticMplsRoutes_) {
    routeDb.addMplsRoute(RibMplsEntry(
        topLabel,
        std::unordered_set<thrift::NextHopThrift>{nhs.begin(), nhs.end()}));
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "Decision::buildRouteDb took " << deltaTime.count() << "ms.";
  fb303::fbData->addStatValue(
      "decision.route_build_ms", deltaTime.count(), fb303::AVG);
  return routeDb;
} // buildRouteDb

BestRouteSelectionResult
SpfSolver::SpfSolverImpl::selectBestRoutes(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    PrefixEntries const& prefixEntries,
    bool const isBgp,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  CHECK(prefixEntries.size()) << "No prefixes for best route selection";
  BestRouteSelectionResult ret;

  if (enableBestRouteSelection_) {
    // Perform best route selection based on metrics
    ret.allNodeAreas = selectBestPrefixMetrics(prefixEntries);
    ret.bestNodeArea = selectBestNodeArea(ret.allNodeAreas, myNodeName);
    ret.success = true;
  } else if (isBgp) {
    ret = runBestPathSelectionBgp(
        myNodeName, prefix, prefixEntries, areaLinkStates);
  } else {
    // If it is openr route, all nodes are considered as best nodes.
    for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
      ret.allNodeAreas.emplace(nodeAndArea);
    }
    ret.bestNodeArea = *ret.allNodeAreas.begin();
    ret.success = true;
  }

  return maybeFilterDrainedNodes(std::move(ret), areaLinkStates);
}

std::optional<int64_t>
SpfSolver::SpfSolverImpl::getMinNextHopThreshold(
    BestRouteSelectionResult nodes, PrefixEntries const& prefixEntries) {
  std::optional<int64_t> maxMinNexthopForPrefix = std::nullopt;
  for (const auto& nodeArea : nodes.allNodeAreas) {
    const auto& prefixEntry = prefixEntries.at(nodeArea);
    maxMinNexthopForPrefix = prefixEntry.minNexthop_ref().has_value() &&
            (not maxMinNexthopForPrefix.has_value() ||
             prefixEntry.minNexthop_ref().value() >
                 maxMinNexthopForPrefix.value())
        ? prefixEntry.minNexthop_ref().value()
        : maxMinNexthopForPrefix;
  }
  return maxMinNexthopForPrefix;
}

BestRouteSelectionResult
SpfSolver::SpfSolverImpl::maybeFilterDrainedNodes(
    BestRouteSelectionResult&& result,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  BestRouteSelectionResult filtered = folly::copy(result);
  for (auto iter = filtered.allNodeAreas.begin();
       iter != filtered.allNodeAreas.end();) {
    const auto& [node, area] = *iter;
    if (areaLinkStates.at(area).isNodeOverloaded(node)) {
      iter = filtered.allNodeAreas.erase(iter);
    } else {
      ++iter;
    }
  }

  // Update the bestNodeArea to a valid key
  if (not filtered.allNodeAreas.empty() and
      filtered.bestNodeArea != result.bestNodeArea) {
    filtered.bestNodeArea = *filtered.allNodeAreas.begin();
  }

  return filtered.allNodeAreas.empty() ? result : filtered;
}

BestRouteSelectionResult
SpfSolver::SpfSolverImpl::runBestPathSelectionBgp(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    PrefixEntries const& prefixEntries,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  BestRouteSelectionResult ret;
  std::optional<thrift::MetricVector> bestVector;
  for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
    auto const& [nodeName, area] = nodeAndArea;
    switch (bestVector.has_value()
                ? MetricVectorUtils::compareMetricVectors(
                      can_throw(*prefixEntry.mv_ref()), *bestVector)
                : MetricVectorUtils::CompareResult::WINNER) {
    case MetricVectorUtils::CompareResult::WINNER:
      ret.allNodeAreas.clear();
      FOLLY_FALLTHROUGH;
    case MetricVectorUtils::CompareResult::TIE_WINNER:
      bestVector = can_throw(*prefixEntry.mv_ref());
      ret.bestNodeArea = nodeAndArea;
      FOLLY_FALLTHROUGH;
    case MetricVectorUtils::CompareResult::TIE_LOOSER:
      ret.allNodeAreas.emplace(nodeAndArea);
      break;
    case MetricVectorUtils::CompareResult::TIE:
      LOG(ERROR) << "Tie ordering prefix entries. Skipping route for "
                 << toString(prefix);
      return ret;
    case MetricVectorUtils::CompareResult::ERROR:
      LOG(ERROR) << "Error ordering prefix entries. Skipping route for "
                 << toString(prefix);
      return ret;
    default:
      break;
    }
  }
  ret.success = true;
  return maybeFilterDrainedNodes(std::move(ret), areaLinkStates);
}

std::optional<RibUnicastEntry>
SpfSolver::SpfSolverImpl::selectBestPathsSpf(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    BestRouteSelectionResult const& bestRouteSelectionResult,
    PrefixEntries const& prefixEntries,
    bool const isBgp,
    thrift::PrefixForwardingType const& forwardingType,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  const bool isV4Prefix = prefix.prefixAddress_ref()->addr_ref()->size() ==
      folly::IPAddressV4::byteCount();
  const bool perDestination =
      forwardingType == thrift::PrefixForwardingType::SR_MPLS;

  // Special case for programming imported next-hops during route origination.
  // This case, programs the next-hops learned from external processes while
  // importing route, along with the computed next-hops.
  //
  // TODO: This is one off the hack to unblock special routing needs. With
  // complete support of multi-area setup, we can delete the following code
  // block.
  auto filteredBestNodeAreas = bestRouteSelectionResult.allNodeAreas;
  if (bestRouteSelectionResult.hasNode(myNodeName) && perDestination) {
    for (const auto& [nodeAndArea, prefixEntry] : prefixEntries) {
      if (perDestination && nodeAndArea.first == myNodeName &&
          prefixEntry.prependLabel_ref()) {
        filteredBestNodeAreas.erase(nodeAndArea);
        break;
      }
    }
  }

  // Get next-hops
  const auto nextHopsWithMetric = getNextHopsWithMetric(
      myNodeName, filteredBestNodeAreas, perDestination, areaLinkStates);
  if (nextHopsWithMetric.second.empty()) {
    VLOG(3) << "No route to prefix " << toString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  return addBestPaths(
      myNodeName,
      prefix,
      bestRouteSelectionResult,
      prefixEntries,
      prefixState,
      isBgp,
      getNextHopsThrift(
          myNodeName,
          bestRouteSelectionResult.allNodeAreas,
          isV4Prefix,
          perDestination,
          nextHopsWithMetric.first,
          nextHopsWithMetric.second,
          std::nullopt,
          areaLinkStates,
          prefixEntries));
}

std::optional<RibUnicastEntry>
SpfSolver::SpfSolverImpl::selectBestPathsKsp2(
    const std::string& myNodeName,
    const thrift::IpPrefix& prefix,
    BestRouteSelectionResult const& bestRouteSelectionResult,
    PrefixEntries const& prefixEntries,
    bool isBgp,
    thrift::PrefixForwardingType const& forwardingType,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  // Sanity check for forwarding type
  if (forwardingType != thrift::PrefixForwardingType::SR_MPLS) {
    LOG(ERROR) << "Incompatible forwarding type "
               << apache ::thrift::util::enumNameSafe(forwardingType)
               << " for algorithm KSPF2_ED_ECMP of " << toString(prefix);

    fb303::fbData->addStatValue(
        "decision.incompatible_forwarding_type", 1, fb303::COUNT);
    return std::nullopt;
  }

  std::unordered_set<thrift::NextHopThrift> nextHops;
  std::vector<LinkState::Path> paths;

  for (const auto& [area, linkState] : areaLinkStates) {
    // find shortest and sec shortest routes towards each node.
    for (const auto& [node, bestArea] : bestRouteSelectionResult.allNodeAreas) {
      // if ourself is considered as ECMP nodes.
      if (node == myNodeName && bestArea == area) {
        continue;
      }
      for (auto const& path : linkState.getKthPaths(myNodeName, node, 1)) {
        paths.push_back(path);
      }
    }

    // when get to second shortes routes, we want to make sure the shortest
    // route is not part of second shortest route to avoid double spraying
    // issue
    size_t const firstPathsSize = paths.size();
    for (const auto& [node, bestArea] : bestRouteSelectionResult.allNodeAreas) {
      if (area != bestArea) {
        continue;
      }
      for (auto const& secPath : linkState.getKthPaths(myNodeName, node, 2)) {
        bool add = true;
        for (size_t i = 0; i < firstPathsSize; ++i) {
          // this could happen for anycast VIPs.
          // for example, in a full mesh topology contains A, B and C. B and C
          // both annouce a prefix P. When A wants to talk to P, it's shortes
          // paths are A->B and A->C. And it is second shortest path is
          // A->B->C and A->C->B. In this case,  A->B->C containser A->B
          // already, so we want to avoid this.
          if (LinkState::pathAInPathB(paths[i], secPath)) {
            add = false;
            break;
          }
        }
        if (add) {
          paths.push_back(secPath);
        }
      }
    }
  }

  if (paths.size() == 0) {
    return std::nullopt;
  }

  for (const auto& path : paths) {
    for (const auto& [area, linkState] : areaLinkStates) {
      Metric cost = 0;
      std::list<int32_t> labels;
      // if self node is one of it's ecmp, it means this prefix is anycast and
      // we need to add prepend label which is static MPLS route the destination
      // prepared.
      auto nextNodeName = myNodeName;
      for (auto& link : path) {
        cost += link->getMetricFromNode(nextNodeName);
        nextNodeName = link->getOtherNodeName(nextNodeName);
        labels.push_front(*linkState.getAdjacencyDatabases()
                               .at(nextNodeName)
                               .nodeLabel_ref());
      }
      labels.pop_back(); // Remove first node's label to respect PHP
      auto& prefixEntry = prefixEntries.at({nextNodeName, area});
      if (prefixEntry.prependLabel_ref()) {
        // add prepend label to bottom of the stack
        labels.push_front(prefixEntry.prependLabel_ref().value());
      }

      // Create nexthop
      CHECK_GE(path.size(), 1);
      auto const& firstLink = path.front();
      std::optional<thrift::MplsAction> mplsAction;
      if (labels.size()) {
        std::vector<int32_t> labelVec{labels.begin(), labels.end()};
        mplsAction = createMplsAction(
            thrift::MplsActionCode::PUSH, std::nullopt, std::move(labelVec));
      }

      auto const& prefixStr = *prefix.prefixAddress_ref()->addr_ref();
      bool isV4Prefix = prefixStr.size() == folly::IPAddressV4::byteCount();

      nextHops.emplace(createNextHop(
          isV4Prefix ? firstLink->getNhV4FromNode(myNodeName)
                     : firstLink->getNhV6FromNode(myNodeName),
          firstLink->getIfaceFromNode(myNodeName),
          cost,
          mplsAction,
          firstLink->getArea(),
          firstLink->getOtherNodeName(myNodeName)));
    }
  }

  return addBestPaths(
      myNodeName,
      prefix,
      bestRouteSelectionResult,
      prefixEntries,
      prefixState,
      isBgp,
      std::move(nextHops));
}

std::optional<RibUnicastEntry>
SpfSolver::SpfSolverImpl::addBestPaths(
    const std::string& myNodeName,
    const thrift::IpPrefix& prefixThrift,
    const BestRouteSelectionResult& bestRouteSelectionResult,
    const PrefixEntries& prefixEntries,
    const PrefixState& prefixState,
    const bool isBgp,
    std::unordered_set<thrift::NextHopThrift>&& nextHops) {
  const auto prefix = toIPNetwork(prefixThrift);

  // Apply min-nexthop requirements. Ignore the route from programming if
  // min-nexthop requirement is not met.
  auto minNextHop =
      getMinNextHopThreshold(bestRouteSelectionResult, prefixEntries);
  if (minNextHop.has_value() && minNextHop.value() > nextHops.size()) {
    LOG(WARNING) << "Dropping route to " << toString(prefixThrift)
                 << " because of min-nexthop requirement. "
                 << "Minimum required " << minNextHop.value() << ", got "
                 << nextHops.size();
    return std::nullopt;
  }

  // Special case for programming imported next-hops during route origination.
  // This case, programs the next-hops learned from external processes while
  // importing route, along with the computed next-hops.
  // TODO: This is one off the hack to unblock special routing needs. With
  // complete support of multi-area setup, we won't need this any longer.
  if (bestRouteSelectionResult.hasNode(myNodeName)) {
    std::optional<int32_t> prependLabel;
    for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
      if (nodeAndArea.first == myNodeName and prefixEntry.prependLabel_ref()) {
        prependLabel = prefixEntry.prependLabel_ref().value();
        break;
      }
    }

    // Self route must be advertised with prepend label
    CHECK(prependLabel.has_value());

    // Add static next-hops
    auto routeIter = staticMplsRoutes_.find(prependLabel.value());
    if (routeIter != staticMplsRoutes_.end()) {
      for (const auto& nh : routeIter->second) {
        nextHops.emplace(createNextHop(
            nh.address_ref().value(), std::nullopt, 0, std::nullopt));
      }
    } else {
      LOG(ERROR) << "Static nexthops doesn't exist for static mpls label "
                 << prependLabel.value();
    }
  }

  // Create RibUnicastEntry and add it the list
  return RibUnicastEntry(
      prefix,
      std::move(nextHops),
      prefixEntries.at(bestRouteSelectionResult.bestNodeArea),
      bestRouteSelectionResult.bestNodeArea.second,
      isBgp & bgpDryRun_); // doNotInstall
}

std::pair<Metric, std::unordered_set<std::string>>
SpfSolver::SpfSolverImpl::getMinCostNodes(
    const SpfResult& spfResult, const std::set<NodeAndArea>& dstNodeAreas) {
  Metric shortestMetric = std::numeric_limits<Metric>::max();

  // find the set of the closest nodes to our destination
  std::unordered_set<std::string> minCostNodes;
  for (const auto& [dstNode, _] : dstNodeAreas) {
    auto it = spfResult.find(dstNode);
    if (it == spfResult.end()) {
      continue;
    }
    const auto nodeDistance = it->second.metric();
    if (shortestMetric >= nodeDistance) {
      if (shortestMetric > nodeDistance) {
        shortestMetric = nodeDistance;
        minCostNodes.clear();
      }
      minCostNodes.emplace(dstNode);
    }
  }

  return std::make_pair(shortestMetric, std::move(minCostNodes));
}

std::pair<
    Metric /* min metric to destination */,
    std::unordered_map<
        std::pair<std::string /* nextHopNodeName */, std::string /* dstNode */>,
        Metric /* the distance from the nexthop to the dest */>>
SpfSolver::SpfSolverImpl::getNextHopsWithMetric(
    const std::string& myNodeName,
    const std::set<NodeAndArea>& dstNodeAreas,
    bool perDestination,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  // build up next hop nodes both nodes that are along a shortest path to the
  // prefix and, if enabled, those with an LFA path to the prefix
  std::unordered_map<
      std::pair<std::string /* nextHopNodeName */, std::string /* dstNode */>,
      Metric /* the distance from the nexthop to the dest */>
      nextHopNodes;
  Metric shortestMetric = std::numeric_limits<Metric>::max();

  for (auto const& [area, linkState] : areaLinkStates) {
    auto const& shortestPathsFromHere = linkState.getSpfResult(myNodeName);
    auto const& minMetricNodes =
        getMinCostNodes(shortestPathsFromHere, dstNodeAreas);

    // Choose routes with lowest Metric
    // if Metric is the same, ecmp in multiple area
    // TODO: Should we consider ecmp in different area with different Metric?
    if (shortestMetric < minMetricNodes.first) {
      continue;
    }

    if (shortestMetric > minMetricNodes.first) {
      shortestMetric = minMetricNodes.first;
      nextHopNodes.clear();
    }

    auto const& minCostNodes = minMetricNodes.second;
    // If no node is reachable then return
    if (minCostNodes.empty()) {
      continue;
    }

    // Add neighbors with shortest path to the prefix
    for (const auto& dstNode : minCostNodes) {
      const auto dstNodeRef = perDestination ? dstNode : "";
      for (const auto& nhName : shortestPathsFromHere.at(dstNode).nextHops()) {
        nextHopNodes[std::make_pair(nhName, dstNodeRef)] = shortestMetric -
            linkState.getMetricFromAToB(myNodeName, nhName).value();
      }
    }

    // add any other neighbors that have LFA paths to the prefix
    if (computeLfaPaths_) {
      for (auto link : linkState.linksFromNode(myNodeName)) {
        if (!link->isUp()) {
          continue;
        }
        const auto& neighborName = link->getOtherNodeName(myNodeName);
        auto const& shortestPathsFromNeighbor =
            linkState.getSpfResult(neighborName);

        const auto neighborToHere =
            shortestPathsFromNeighbor.at(myNodeName).metric();
        for (const auto& [dstNode, dstArea] : dstNodeAreas) {
          if (area != dstArea) {
            continue;
          }
          auto shortestPathItr = shortestPathsFromNeighbor.find(dstNode);
          if (shortestPathItr == shortestPathsFromNeighbor.end()) {
            continue;
          }
          const auto distanceFromNeighbor = shortestPathItr->second.metric();

          // This is the LFA condition per RFC 5286
          if (distanceFromNeighbor < shortestMetric + neighborToHere) {
            const auto nextHopKey =
                std::make_pair(neighborName, perDestination ? dstNode : "");
            auto nextHopItr = nextHopNodes.find(nextHopKey);
            if (nextHopItr == nextHopNodes.end()) {
              nextHopNodes.emplace(nextHopKey, distanceFromNeighbor);
            } else if (nextHopItr->second > distanceFromNeighbor) {
              nextHopItr->second = distanceFromNeighbor;
            }
          } // end if
        } // end for dstNodeNames
      } // end for linkState.linksFromNode(myNodeName)
    }
  }

  return std::make_pair(shortestMetric, nextHopNodes);
}

std::unordered_set<thrift::NextHopThrift>
SpfSolver::SpfSolverImpl::getNextHopsThrift(
    const std::string& myNodeName,
    const std::set<NodeAndArea>& dstNodeAreas,
    bool isV4,
    bool perDestination,
    const Metric minMetric,
    std::unordered_map<std::pair<std::string, std::string>, Metric>
        nextHopNodes,
    std::optional<int32_t> swapLabel,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixEntries const& prefixEntries) const {
  CHECK(not nextHopNodes.empty());

  std::unordered_set<thrift::NextHopThrift> nextHops;
  for (const auto& [area, linkState] : areaLinkStates) {
    for (const auto& link : linkState.linksFromNode(myNodeName)) {
      for (const auto& [dstNode, dstArea] :
           perDestination ? dstNodeAreas : std::set<NodeAndArea>{{"", ""}}) {
        // Only consider destinations within the area
        if (not dstArea.empty() and area != dstArea) {
          continue;
        }

        const auto neighborNode = link->getOtherNodeName(myNodeName);
        const auto search =
            nextHopNodes.find(std::make_pair(neighborNode, dstNode));

        // Ignore overloaded links or nexthops
        if (search == nextHopNodes.end() or not link->isUp()) {
          continue;
        }

        // Ignore link if other side of link is one of our destination and we
        // are trying to send to dstNode via neighbor (who is also our
        // destination)
        if (not dstNode.empty() and dstNodeAreas.count({neighborNode, area}) and
            neighborNode != dstNode) {
          continue;
        }

        // Ignore nexthops that are not shortest if lfa is disabled. All links
        // towards the nexthop on shortest path are LFA routes.
        Metric distOverLink =
            link->getMetricFromNode(myNodeName) + search->second;
        if (not computeLfaPaths_ and distOverLink != minMetric) {
          continue;
        }

        // Create associated mpls action if swapLabel is provided
        std::optional<thrift::MplsAction> mplsAction;
        if (swapLabel.has_value()) {
          CHECK(not mplsAction.has_value());
          bool isNextHopAlsoDst = dstNodeAreas.count({neighborNode, area});
          mplsAction = createMplsAction(
              isNextHopAlsoDst ? thrift::MplsActionCode::PHP
                               : thrift::MplsActionCode::SWAP,
              isNextHopAlsoDst ? std::nullopt : swapLabel);
        }

        // Create associated mpls action if dest node is not empty and
        // destination is not our neighbor
        if (not dstNode.empty()) {
          std::vector<int32_t> pushLabels;

          // Add prepend label if any
          auto& dstPrefixEntry = prefixEntries.at({dstNode, area});
          if (dstPrefixEntry.prependLabel_ref()) {
            pushLabels.emplace_back(dstPrefixEntry.prependLabel_ref().value());
            if (not isMplsLabelValid(pushLabels.back())) {
              continue;
            }
          }

          // Add destination node label if it is not neighbor node
          if (dstNode != neighborNode) {
            pushLabels.emplace_back(
                *linkState.getAdjacencyDatabases().at(dstNode).nodeLabel_ref());
            if (not isMplsLabelValid(pushLabels.back())) {
              continue;
            }
          }

          // Create PUSH mpls action if there are labels to push
          if (not pushLabels.empty()) {
            CHECK(not mplsAction.has_value());
            mplsAction = createMplsAction(
                thrift::MplsActionCode::PUSH,
                std::nullopt,
                std::move(pushLabels));
          }
        }

        // if we are computing LFA paths, any nexthop to the node will do
        // otherwise, we only want those nexthops along a shortest path
        nextHops.emplace(createNextHop(
            isV4 ? link->getNhV4FromNode(myNodeName)
                 : link->getNhV6FromNode(myNodeName),
            link->getIfaceFromNode(myNodeName),
            distOverLink,
            mplsAction,
            link->getArea(),
            link->getOtherNodeName(myNodeName)));
      } // end for perDestination ...
    } // end for linkState ...
  }
  return nextHops;
}

//
// Public SpfSolver
//

SpfSolver::SpfSolver(
    const std::string& myNodeName,
    bool enableV4,
    bool computeLfaPaths,
    bool enableOrderedFib,
    bool bgpDryRun,
    bool enableBestRouteSelection)
    : impl_(new SpfSolver::SpfSolverImpl(
          myNodeName,
          enableV4,
          computeLfaPaths,
          enableOrderedFib,
          bgpDryRun,
          enableBestRouteSelection)) {}

SpfSolver::~SpfSolver() {}

void
SpfSolver::updateStaticUnicastRoutes(
    const std::vector<thrift::UnicastRoute>& unicastRoutesToUpdate,
    const std::vector<thrift::IpPrefix>& unicastRoutesToDelete) {
  return impl_->updateStaticUnicastRoutes(
      unicastRoutesToUpdate, unicastRoutesToDelete);
}

void
SpfSolver::updateStaticMplsRoutes(
    const std::vector<thrift::MplsRoute>& mplsRoutesToUpdate,
    const std::vector<int32_t>& mplsRoutesToDelete) {
  return impl_->updateStaticMplsRoutes(mplsRoutesToUpdate, mplsRoutesToDelete);
}

std::optional<RibUnicastEntry>
SpfSolver::createRouteForPrefixOrGetStaticRoute(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState,
    thrift::IpPrefix const& prefix) {
  return impl_->createRouteForPrefixOrGetStaticRoute(
      myNodeName, areaLinkStates, prefixState, prefix);
}

std::unordered_map<thrift::IpPrefix, BestRouteSelectionResult> const&
SpfSolver::getBestRoutesCache() const {
  return impl_->getBestRoutesCache();
}

std::optional<DecisionRouteDb>
SpfSolver::buildRouteDb(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  return impl_->buildRouteDb(myNodeName, areaLinkStates, prefixState);
}

//
// Decision class implementation
//

Decision::Decision(
    std::shared_ptr<const Config> config,
    // TODO: migrate argument list flags to OpenrConfig
    bool computeLfaPaths,
    bool bgpDryRun,
    std::chrono::milliseconds debounceMinDur,
    std::chrono::milliseconds debounceMaxDur,
    // consumer queue
    messaging::RQueue<thrift::Publication> kvStoreUpdatesQueue,
    messaging::RQueue<thrift::RouteDatabaseDelta> staticRoutesUpdateQueue,
    // producer queue
    messaging::ReplicateQueue<DecisionRouteUpdate>& routeUpdatesQueue)
    : config_(config),
      routeUpdatesQueue_(routeUpdatesQueue),
      myNodeName_(*config->getConfig().node_name_ref()),
      pendingUpdates_(*config->getConfig().node_name_ref()),
      rebuildRoutesDebounced_(
          getEvb(), debounceMinDur, debounceMaxDur, [this]() noexcept {
            rebuildRoutes("DECISION_DEBOUNCE");
          }) {
  spfSolver_ = std::make_unique<SpfSolver>(
      config->getNodeName(),
      config->isV4Enabled(),
      computeLfaPaths,
      config->isOrderedFibProgrammingEnabled(),
      bgpDryRun,
      config->isBestRouteSelectionEnabled());

  coldStartTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    pendingUpdates_.setNeedsFullRebuild();
    rebuildRoutes("COLD_START_UPDATE");
  });
  if (auto eor = config->getConfig().eor_time_s_ref()) {
    coldStartTimer_->scheduleTimeout(std::chrono::seconds(*eor));
  }

  // Schedule periodic timer for counter submission
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    updateGlobalCounters();
    // Schedule next counters update
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);

  // Schedule periodic timer to decremtOrderedFibHolds
  if (config->isOrderedFibProgrammingEnabled()) {
    orderedFibTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
      LOG(INFO) << "Decrementing Holds";
      if (decrementOrderedFibHolds()) {
        auto timeout = getMaxFib();
        LOG(INFO) << "Scheduling next hold decrement in " << timeout.count()
                  << "ms";
        orderedFibTimer_->scheduleTimeout(getMaxFib());
      }
    });
  }

  // Add reader to process publication from KvStore
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    LOG(INFO) << "Starting KvStore updates processing fiber";
    while (true) {
      auto maybeThriftPub = q.get(); // perform read
      VLOG(2) << "Received KvStore update";
      if (maybeThriftPub.hasError()) {
        LOG(INFO) << "Terminating KvStore updates processing fiber";
        break;
      }
      try {
        processPublication(std::move(maybeThriftPub).value());
      } catch (const std::exception& e) {
#if FOLLY_USE_SYMBOLIZER
        // collect stack strace then fail the process
        for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
          LOG(ERROR) << exInfo;
        }
#endif
        // FATAL to produce core dump
        LOG(FATAL) << "Exception occured in Decision::processPublication - "
                   << folly::exceptionStr(e);
      }
      // compute routes with exponential backoff timer if needed
      if (pendingUpdates_.needsRouteUpdate()) {
        rebuildRoutesDebounced_();
      }
    }
  });

  // Add reader to process static routes publication from prefix-manager
  addFiberTask(
      [q = std::move(staticRoutesUpdateQueue), this]() mutable noexcept {
        LOG(INFO) << "Starting static routes update processing fiber";
        while (true) {
          auto maybeThriftPub = q.get(); // perform read
          VLOG(2) << "Received static routes update";
          if (maybeThriftPub.hasError()) {
            LOG(INFO) << "Terminating static routes update processing fiber";
            break;
          }
          processStaticRoutesUpdate(std::move(maybeThriftPub).value());
        }
      });

  // Create RibPolicy timer to process routes on policy expiry
  ribPolicyTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    LOG(WARNING) << "RibPolicy is expired";
    pendingUpdates_.setNeedsFullRebuild();
    rebuildRoutes("RIB_POLICY_EXPIRED");
  });

  // Initialize some stat keys
  fb303::fbData->addStatExportType(
      "decision.rib_policy_processing.time_ms", fb303::AVG);
}

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
Decision::getDecisionRouteDb(std::string nodeName) {
  folly::Promise<std::unique_ptr<thrift::RouteDatabase>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), nodeName, this]() mutable {
    thrift::RouteDatabase routeDb;

    if (nodeName.empty()) {
      nodeName = myNodeName_;
    }
    auto maybeRouteDb =
        spfSolver_->buildRouteDb(nodeName, areaLinkStates_, prefixState_);
    if (maybeRouteDb.has_value()) {
      routeDb = maybeRouteDb->toThrift();
    }

    *routeDb.thisNodeName_ref() = nodeName;
    p.setValue(std::make_unique<thrift::RouteDatabase>(std::move(routeDb)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
Decision::getDecisionAdjacenciesFiltered(thrift::AdjacenciesFilter filter) {
  folly::Promise<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [p = std::move(p), filter = std::move(filter), this]() mutable {
        auto res = std::make_unique<std::vector<thrift::AdjacencyDatabase>>();
        for (auto const& [area, linkState] : areaLinkStates_) {
          if (filter.get_selectAreas().empty() ||
              filter.get_selectAreas().count(area)) {
            for (auto const& [_, db] : linkState.getAdjacencyDatabases()) {
              res->push_back(db);
            }
          }
        }
        p.setValue(std::move(res));
      });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>
Decision::getReceivedRoutesFiltered(thrift::ReceivedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable noexcept {
        // Get route details
        auto routes = prefixState_.getReceivedRoutesFiltered(filter);

        // Add best path result to this
        auto const& bestRoutesCache = spfSolver_->getBestRoutesCache();
        for (auto& route : routes) {
          auto const& bestRoutesIt = bestRoutesCache.find(*route.prefix_ref());
          if (bestRoutesIt != bestRoutesCache.end()) {
            auto const& bestRoutes = bestRoutesIt->second;
            // Set all selected node-area
            for (auto const& [node, area] : bestRoutes.allNodeAreas) {
              route.bestKeys_ref()->emplace_back();
              auto& key = route.bestKeys_ref()->back();
              key.node_ref() = node;
              key.area_ref() = area;
            }
            // Set best node-area
            route.bestKey_ref()->node_ref() = bestRoutes.bestNodeArea.first;
            route.bestKey_ref()->area_ref() = bestRoutes.bestNodeArea.second;
          }
        }

        // Set the promise
        p.setValue(std::make_unique<std::vector<thrift::ReceivedRouteDetail>>(
            std::move(routes)));
      });
  return std::move(sf);
}

folly::SemiFuture<folly::Unit>
Decision::clearRibPolicy() {
  auto [p, sf] = folly::makePromiseContract<folly::Unit>();
  if (not config_->isRibPolicyEnabled()) {
    thrift::OpenrError error;
    *error.message_ref() = "RibPolicy feature is not enabled";
    p.setException(error);
    return std::move(sf);
  }

  runInEventBaseThread([this, p = std::move(p)]() mutable {
    if (not ribPolicy_) {
      thrift::OpenrError error;
      *error.message_ref() = "No RIB policy configured";
      p.setException(error);
    } else {
      ribPolicy_ = nullptr;
      // Trigger route computation
      pendingUpdates_.setNeedsFullRebuild();
      rebuildRoutes("RIB_POLICY_CLEARED");
      p.setValue();
    }
  });

  return std::move(sf);
}

folly::SemiFuture<folly::Unit>
Decision::setRibPolicy(thrift::RibPolicy const& ribPolicyThrift) {
  auto [p, sf] = folly::makePromiseContract<folly::Unit>();
  if (not config_->isRibPolicyEnabled()) {
    thrift::OpenrError error;
    *error.message_ref() = "RibPolicy feature is not enabled";
    p.setException(error);
    return std::move(sf);
  }

  std::unique_ptr<RibPolicy> ribPolicy;
  try {
    ribPolicy = std::make_unique<RibPolicy>(ribPolicyThrift);
  } catch (thrift::OpenrError const& e) {
    p.setException(e);
    return std::move(sf);
  }

  runInEventBaseThread(
      [this, p = std::move(p), ribPolicy = std::move(ribPolicy)]() mutable {
        const auto durationLeft = ribPolicy->getTtlDuration();
        if (durationLeft.count() <= 0) {
          LOG(ERROR)
              << "Ignoring RibPolicy update with new instance because of "
              << "staleness. Validity " << durationLeft.count() << "ms";
          return;
        }

        // Update local policy instance
        LOG(INFO) << "Updating RibPolicy with new instance. Validity "
                  << durationLeft.count() << "ms";
        ribPolicy_ = std::move(ribPolicy);

        // Schedule timer for processing routes on expiry
        ribPolicyTimer_->scheduleTimeout(durationLeft);

        // Trigger route computation
        pendingUpdates_.setNeedsFullRebuild();
        rebuildRoutes("RIB_POLICY_UPDATE");

        // Mark the policy update request to be done
        p.setValue();
      });
  return std::move(sf);
}

folly::SemiFuture<thrift::RibPolicy>
Decision::getRibPolicy() {
  auto [p, sf] = folly::makePromiseContract<thrift::RibPolicy>();
  if (not config_->isRibPolicyEnabled()) {
    thrift::OpenrError error;
    *error.message_ref() = "RibPolicy feature is not enabled";
    p.setException(error);
    return std::move(sf);
  }

  runInEventBaseThread([this, p = std::move(p)]() mutable {
    if (ribPolicy_) {
      p.setValue(ribPolicy_->toThrift());
    } else {
      thrift::OpenrError e;
      *e.message_ref() = "RibPolicy is not configured";
      p.setException(e);
    }
  });
  return std::move(sf);
}

void
Decision::processPublication(thrift::Publication&& thriftPub) {
  CHECK(not thriftPub.area_ref()->empty());
  auto const& area = *thriftPub.area_ref();

  if (!areaLinkStates_.count(area)) {
    areaLinkStates_.emplace(area, area);
  }
  auto& areaLinkState = areaLinkStates_.at(area);

  // Nothing to process if no adj/prefix db changes
  if (thriftPub.keyVals_ref()->empty() and
      thriftPub.expiredKeys_ref()->empty()) {
    return;
  }

  // LSDB addition/update
  for (const auto& [key, rawVal] : *thriftPub.keyVals_ref()) {
    if (not rawVal.value_ref().has_value()) {
      // skip TTL update
      DCHECK(*rawVal.ttlVersion_ref() > 0);
      continue;
    }

    try {
      // adjacencyDb: update keys starting with "adj:"
      if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
        auto adjacencyDb = readThriftObjStr<thrift::AdjacencyDatabase>(
            rawVal.value_ref().value(), serializer_);
        auto& nodeName = adjacencyDb.get_thisNodeName();

        // TODO - this should directly come from KvStore. Needed for
        // compatibility between default and non-default areas
        adjacencyDb.area_ref() = area;

        LinkStateMetric holdUpTtl = 0, holdDownTtl = 0;
        if (config_->isOrderedFibProgrammingEnabled()) {
          if (auto maybeHoldUpTtl =
                  areaLinkState.getHopsFromAToB(myNodeName_, nodeName)) {
            holdUpTtl = maybeHoldUpTtl.value();
            holdDownTtl = areaLinkState.getMaxHopsToNode(nodeName) - holdUpTtl;
          }
        }
        fb303::fbData->addStatValue("decision.adj_db_update", 1, fb303::COUNT);
        pendingUpdates_.applyLinkStateChange(
            nodeName,
            areaLinkState.updateAdjacencyDatabase(
                adjacencyDb, holdUpTtl, holdDownTtl),
            adjacencyDb.perfEvents_ref());
        if (areaLinkState.hasHolds() && orderedFibTimer_ != nullptr &&
            !orderedFibTimer_->isScheduled()) {
          orderedFibTimer_->scheduleTimeout(getMaxFib());
        }
        continue;
      }

      // prefixDb: update keys starting with "prefix:"
      if (key.find(Constants::kPrefixDbMarker.toString()) == 0) {
        auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
            rawVal.value_ref().value(), serializer_);
        if (1 != prefixDb.get_prefixEntries().size()) {
          LOG(ERROR) << "Expecting exactly one entry per prefix key";
          fb303::fbData->addStatValue("decision.error", 1, fb303::COUNT);
          continue;
        }
        auto const& entry = prefixDb.get_prefixEntries().front();
        auto const& areaStack = entry.get_area_stack();
        // Ignore self redistributed route reflection
        // These routes are programmed by Decision,
        // re-origintaed by me to areas that do not have the best prefix entry
        if (prefixDb.get_thisNodeName() == myNodeName_ &&
            areaStack.size() > 0 && areaLinkStates_.count(areaStack.back())) {
          VLOG(2) << "Ignore self redistributed route reflection for prefix: "
                  << key << " area_stack: " << folly::join(",", areaStack);
          continue;
        }

        // construct new prefix key with local publication area id
        PrefixKey prefixKey(
            prefixDb.get_thisNodeName(), toIPNetwork(entry.get_prefix()), area);

        fb303::fbData->addStatValue(
            "decision.prefix_db_update", 1, fb303::COUNT);
        pendingUpdates_.applyPrefixStateChange(
            prefixDb.get_deletePrefix()
                ? prefixState_.deletePrefix(prefixKey)
                : prefixState_.updatePrefix(prefixKey, entry),
            prefixDb.perfEvents_ref());
        continue;
      }

      // update keys starting with "fibTime:"
      if (key.find(Constants::kFibTimeMarker.toString()) == 0) {
        try {
          std::chrono::milliseconds fibTime{stoll(rawVal.value_ref().value())};
          fibTimes_[getNodeNameFromKey(key)] = fibTime;
        } catch (...) {
          LOG(ERROR) << "Could not convert "
                     << Constants::kFibTimeMarker.toString()
                     << " value to int64";
        }
        continue;
      }
    } catch (const std::exception& e) {
      LOG(ERROR) << "Failed to deserialize info for key " << key
                 << ". Exception: " << folly::exceptionStr(e);
    }
  }

  // LSDB deletion
  for (const auto& key : *thriftPub.expiredKeys_ref()) {
    std::string nodeName = getNodeNameFromKey(key);

    // adjacencyDb: delete keys starting with "adj:"
    if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
      pendingUpdates_.applyLinkStateChange(
          nodeName,
          areaLinkState.deleteAdjacencyDatabase(nodeName),
          thrift::PrefixDatabase().perfEvents_ref()); // Empty perf events
      continue;
    }

    // prefixDb: delete keys starting with "prefix:"
    if (key.find(Constants::kPrefixDbMarker.toString()) == 0) {
      VLOG(2) << "Deleting expired prefix key: " << key
              << " of node: " << nodeName << " in area: " << area;
      auto maybePrefixKey = PrefixKey::fromStr(key);
      if (maybePrefixKey.hasError()) {
        LOG(ERROR) << "Unable to parse prefix key: " << key << ". Skipping.";
        continue;
      }
      // construct new prefix key with local publication area id
      PrefixKey prefixKey(
          maybePrefixKey.value().getNodeName(),
          maybePrefixKey.value().getCIDRNetwork(),
          area);
      pendingUpdates_.applyPrefixStateChange(
          prefixState_.deletePrefix(prefixKey),
          thrift::PrefixDatabase().perfEvents_ref()); // Empty perf events
      continue;
    }
  }
}

void
Decision::processStaticRoutesUpdate(
    thrift::RouteDatabaseDelta&& staticRoutesDelta) {
  // update static unicast routes
  if (staticRoutesDelta.unicastRoutesToUpdate_ref()->size() or
      staticRoutesDelta.unicastRoutesToDelete_ref()->size()) {
    const auto& toUpdate = *staticRoutesDelta.unicastRoutesToUpdate_ref();
    const auto& toDelete = *staticRoutesDelta.unicastRoutesToDelete_ref();

    spfSolver_->updateStaticUnicastRoutes(toUpdate, toDelete);

    // combine add/update/delete prefixes into pending update
    auto change = from(toUpdate) |
        mapped([](const thrift::UnicastRoute& r) { return *r.dest_ref(); }) |
        as<std::unordered_set<thrift::IpPrefix>>();
    change.merge(
        std::unordered_set<thrift::IpPrefix>(toDelete.begin(), toDelete.end()));

    pendingUpdates_.applyPrefixStateChange(
        std::move(change),
        thrift::PrefixDatabase().perfEvents_ref()); // Empty perf events
  }

  // update static MPLS routes
  if (staticRoutesDelta.mplsRoutesToUpdate_ref()->size() or
      staticRoutesDelta.mplsRoutesToDelete_ref()->size()) {
    spfSolver_->updateStaticMplsRoutes(
        *staticRoutesDelta.mplsRoutesToUpdate_ref(),
        *staticRoutesDelta.mplsRoutesToDelete_ref());
    pendingUpdates_.setNeedsFullRebuild(); // Mark for full DB rebuild
  }
  rebuildRoutesDebounced_();
}

void
Decision::rebuildRoutes(std::string const& event) {
  if (coldStartTimer_->isScheduled()) {
    return;
  }

  pendingUpdates_.addEvent(event);
  VLOG(2) << "Decision: processing " << pendingUpdates_.getCount()
          << " accumulated updates. " << event;
  if (pendingUpdates_.perfEvents()) {
    if (auto expectedDuration = getDurationBetweenPerfEvents(
            *pendingUpdates_.perfEvents(),
            "DECISION_RECEIVED",
            "DECISION_DEBOUNCE")) {
      VLOG(2) << "Debounced " << pendingUpdates_.getCount() << " events over "
              << expectedDuration->count() << "ms.";
    }
  }

  DecisionRouteUpdate update;
  if (pendingUpdates_.needsFullRebuild()) {
    // if only static routes gets updated, we still need to update routes
    // because there maybe routes depended on static routes.
    auto maybeRouteDb =
        spfSolver_->buildRouteDb(myNodeName_, areaLinkStates_, prefixState_);
    LOG_IF(WARNING, !maybeRouteDb)
        << "SEVERE: full route rebuild resulted in no routes";
    auto db = std::move(maybeRouteDb).value_or(DecisionRouteDb{});
    if (ribPolicy_) {
      auto start = std::chrono::steady_clock::now();
      ribPolicy_->applyPolicy(db.unicastRoutes);
      updateCounters(
          "decision.rib_policy_processing.time_ms",
          start,
          std::chrono::steady_clock::now());
    }
    // update `DecisionRouteDb` cache and return delta as `update`
    update = routeDb_.calculateUpdate(std::move(db));
  } else {
    // process prefixes update from `prefixState_`
    for (auto const& prefix : pendingUpdates_.updatedPrefixes()) {
      if (auto maybeRibEntry = spfSolver_->createRouteForPrefixOrGetStaticRoute(
              myNodeName_, areaLinkStates_, prefixState_, prefix)) {
        update.addRouteToUpdate(std::move(maybeRibEntry).value());
      } else {
        update.unicastRoutesToDelete.emplace_back(toIPNetwork(prefix));
      }
    }
    if (ribPolicy_) {
      auto start = std::chrono::steady_clock::now();
      auto const changes =
          ribPolicy_->applyPolicy(update.unicastRoutesToUpdate);
      updateCounters(
          "decision.rib_policy_processing.time_ms",
          start,
          std::chrono::steady_clock::now());
      for (auto const& prefix : changes.deletedRoutes) {
        update.unicastRoutesToDelete.push_back(prefix);
      }
    }
  }

  routeDb_.update(update);
  pendingUpdates_.addEvent("ROUTE_UPDATE");
  update.perfEvents = pendingUpdates_.moveOutEvents();
  pendingUpdates_.reset();

  // send `DecisionRouteUpdate` to Fib/PrefixMgr
  routeUpdatesQueue_.push(std::move(update));
}

bool
Decision::decrementOrderedFibHolds() {
  bool topoChanged = false;
  bool stillHasHolds = false;
  for (auto& [_, linkState] : areaLinkStates_) {
    pendingUpdates_.applyLinkStateChange(
        myNodeName_,
        linkState.decrementHolds(),
        thrift::AdjacencyDatabase().perfEvents_ref()); // Empty perf events
    stillHasHolds |= linkState.hasHolds();
  }
  if (pendingUpdates_.needsRouteUpdate()) {
    rebuildRoutes("ORDERED_FIB_HOLDS_EXPIRED");
  }
  return stillHasHolds;
}

std::chrono::milliseconds
Decision::getMaxFib() {
  std::chrono::milliseconds maxFib{1};
  for (auto& kv : fibTimes_) {
    maxFib = std::max(maxFib, kv.second);
  }
  return maxFib;
}

void
Decision::updateCounters(
    std::string key,
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) const {
  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  fb303::fbData->addStatValue(key, elapsedTime.count(), fb303::AVG);
}

void
Decision::updateGlobalCounters() const {
  size_t numAdjacencies = 0, numPartialAdjacencies = 0;
  std::unordered_set<std::string> nodeSet;
  for (auto const& [_, linkState] : areaLinkStates_) {
    numAdjacencies += linkState.numLinks();
    auto const& mySpfResult = linkState.getSpfResult(myNodeName_);
    for (auto const& kv : linkState.getAdjacencyDatabases()) {
      nodeSet.insert(kv.first);
      const auto& adjDb = kv.second;
      size_t numLinks = linkState.linksFromNode(kv.first).size();
      // Consider partial adjacency only iff node is reachable from current
      // node
      if (mySpfResult.count(*adjDb.thisNodeName_ref()) && 0 != numLinks) {
        // only add to the count if this node is not completely disconnected
        size_t diff = adjDb.adjacencies_ref()->size() - numLinks;
        // Number of links (bi-directional) must be <= number of adjacencies
        CHECK_GE(diff, 0);
        numPartialAdjacencies += diff;
      }
    }
  }

  size_t numConflictingPrefixes{0};
  for (const auto& [prefix, prefixEntries] : prefixState_.prefixes()) {
    if (not PrefixState::hasConflictingForwardingInfo(prefixEntries)) {
      continue;
    }
    LOG(WARNING) << "Prefix " << toString(prefix) << " has conflicting "
                 << "forwarding algorithm or type.";
    numConflictingPrefixes += 1;
  }

  // Add custom counters
  fb303::fbData->setCounter(
      "decision.num_conflicting_prefixes", numConflictingPrefixes);
  fb303::fbData->setCounter(
      "decision.num_partial_adjacencies", numPartialAdjacencies);
  fb303::fbData->setCounter(
      "decision.num_complete_adjacencies", numAdjacencies);
  // When node has no adjacencies then linkState reports 0
  fb303::fbData->setCounter(
      "decision.num_nodes", std::max(nodeSet.size(), static_cast<size_t>(1ul)));
  fb303::fbData->setCounter(
      "decision.num_prefixes", prefixState_.prefixes().size());
}

} // namespace openr
