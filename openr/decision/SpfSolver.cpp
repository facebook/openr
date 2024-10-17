/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/MplsUtil.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/SpfSolver.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

namespace fb303 = facebook::fb303;

namespace openr {

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
  for (auto const& [prefix, _] : unicastRoutes) {
    if (!newDb.unicastRoutes.count(prefix)) {
      delta.unicastRoutesToDelete.emplace_back(prefix);
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
}

SpfSolver::SpfSolver(
    const std::string& myNodeName,
    bool enableV4,
    bool enableBestRouteSelection,
    bool v4OverV6Nexthop)
    : myNodeName_(myNodeName),
      enableV4_(enableV4),
      enableBestRouteSelection_(enableBestRouteSelection),
      v4OverV6Nexthop_(v4OverV6Nexthop) {
  // Initialize stat keys
  fb303::fbData->addStatExportType("decision.adj_db_update", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.no_route_to_prefix", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.path_build_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.prefix_db_update", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.route_build_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.route_build_runs", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.get_route_for_prefix", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.spf_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.spf_runs", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.errors", fb303::COUNT);
}

SpfSolver::~SpfSolver() = default;

void
SpfSolver::updateStaticUnicastRoutes(
    const std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>&
        unicastRoutesToUpdate,
    const std::vector<folly::CIDRNetwork>& unicastRoutesToDelete) {
  // Process IP routes to add or update
  XLOG_IF(INFO, unicastRoutesToUpdate.size())
      << "Adding/Updating " << unicastRoutesToUpdate.size()
      << " static unicast routes.";
  for (const auto& [prefix, ribUnicastEntry] : unicastRoutesToUpdate) {
    staticUnicastRoutes_.insert_or_assign(prefix, ribUnicastEntry);

    XLOG(DBG1) << "> " << folly::IPAddress::networkToString(prefix)
               << ", NextHopsCount = " << ribUnicastEntry.nexthops.size();
    for (auto const& nh : ribUnicastEntry.nexthops) {
      XLOG(DBG2) << " via " << toString(nh);
    }
  }

  XLOG_IF(INFO, unicastRoutesToDelete.size())
      << "Deleting " << unicastRoutesToDelete.size()
      << " static unicast routes.";
  for (const auto& prefix : unicastRoutesToDelete) {
    // mark unicast entry to be deleted
    staticUnicastRoutes_.erase(prefix);

    XLOG(DBG1) << "> " << folly::IPAddress::networkToString(prefix);
  }
}

std::optional<RibUnicastEntry>
SpfSolver::createRouteForPrefixOrGetStaticRoute(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState,
    folly::CIDRNetwork const& prefix) {
  // route output from `PrefixState` has higher priority over
  // static unicast routes
  if (auto maybeRoute = createRouteForPrefix(
          myNodeName, areaLinkStates, prefixState, prefix)) {
    return maybeRoute;
  }

  // check `staticUnicastRoutes_`
  auto it = staticUnicastRoutes_.find(prefix);
  if (it != staticUnicastRoutes_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<RibUnicastEntry>
SpfSolver::createRouteForPrefix(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState,
    folly::CIDRNetwork const& prefix) {
  fb303::fbData->addStatValue("decision.get_route_for_prefix", 1, fb303::COUNT);

  // Sanity check for V4 prefixes
  const bool isV4Prefix = prefix.first.isV4();
  if (isV4Prefix and (not enableV4_) and (not v4OverV6Nexthop_)) {
    XLOG(WARNING) << "Received v4 prefix "
                  << folly::IPAddress::networkToString(prefix)
                  << " while v4 is not enabled, and "
                  << "we are not allowing v4 prefix over v6 nexthop.";
    return std::nullopt;
  }

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
  bool localPrefixConsidered{false};
  for (auto& [area, linkState] : areaLinkStates) {
    auto const& mySpfResult = linkState.getSpfResult(myNodeName);

    // Delete entries of unreachable nodes from prefixEntries
    for (auto it = prefixEntries.cbegin(); it != prefixEntries.cend();) {
      const auto& [prefixNode, prefixArea] = it->first;
      // TODO: remove this when tie-breaking process completely done in Decision
      // instead of PrefixManager
      // This indicates that when we calculated the
      // best path, we have considered locally originated prefix as well
      if (myNodeName == prefixNode) {
        localPrefixConsidered = true;
      }
      // Only check reachability within the area that prefixNode belongs to.
      if (area != prefixArea || mySpfResult.count(prefixNode)) {
        ++it; // retain
      } else {
        it = prefixEntries.erase(it); // erase the unreachable prefix entry
      }
    }
  }

  // Skip if no valid prefixes
  if (prefixEntries.empty()) {
    XLOG(INFO) << "Skipping route to "
               << folly::IPAddress::networkToString(prefix)
               << " with no reachable node.";
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  /*
   * [Best Route Selection]
   *
   * Prefix can be advertised from multiple places:
   *  - locally originated;
   *  - re-distributeed from BGP speaker;
   *  - re-advertised across multiple areas;
   *  - etc.;
   *
   * route selection procedure will find the best candidate(NodeAndArea) to run
   * Dijkstra(SPF)
   */
  auto routeSelectionResult =
      selectBestRoutes(myNodeName, prefix, prefixEntries, areaLinkStates);
  if (routeSelectionResult.allNodeAreas.empty()) {
    XLOG(WARNING) << "No route to prefix "
                  << folly::IPAddress::networkToString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  // Set best route selection in prefix state
  bestRoutesCache_.insert_or_assign(prefix, routeSelectionResult);

  /*
   * ATTN:
   * Skip adding route if one prefix is advertised by local node.
   */
  if (routeSelectionResult.hasNode(myNodeName)) {
    XLOG(DBG3) << "Skip adding route for prefixes advertised by " << myNodeName
               << " " << folly::IPAddress::networkToString(prefix);

    return std::nullopt;
  }

  // TODO: What if there are multiple best areas populating for the single
  // prefix?
  std::unordered_set<std::string> areaWithBestRoutes;
  for (const auto& [areaId, _] : areaLinkStates) {
    if (hasBestRoutesInArea(
            areaId, prefixEntries, routeSelectionResult.allNodeAreas)) {
      areaWithBestRoutes.insert(areaId);
    }
  }

  /*
   * [Route Computation]
   *
   * For each area:
   * - Switch on algorithm type:
   *   - Create next-hops from paths
   *   - Only use the next-hop set if it has the shortest metric;
   *   - Combine shortest metric next-hops from all areas;
   */
  std::unordered_set<thrift::NextHopThrift> totalNextHops;
  Metric shortestMetric = std::numeric_limits<Metric>::max();
  for (const auto& area : areaWithBestRoutes) {
    const auto& linkState = areaLinkStates.find(area);
    if (linkState == areaLinkStates.end()) {
      // If the route computation rules are default, then area path computation
      // rules will only contains valid areas.
      continue;
    }

    auto spfAreaResults = selectBestPathsSpf(
        myNodeName, prefix, routeSelectionResult, area, linkState->second);

    // Only use next-hops in areas with the shortest IGP metric
    if (shortestMetric >= spfAreaResults.bestMetric) {
      if (shortestMetric > spfAreaResults.bestMetric) {
        shortestMetric = spfAreaResults.bestMetric;
        totalNextHops.clear();
      }
      totalNextHops.insert(
          spfAreaResults.nextHops.begin(), spfAreaResults.nextHops.end());
    }
  }

  return addBestPaths(
      myNodeName,
      prefix,
      routeSelectionResult,
      prefixEntries,
      std::move(totalNextHops),
      shortestMetric,
      localPrefixConsidered);
}

std::optional<DecisionRouteDb>
SpfSolver::buildRouteDb(
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
  for (auto [prefix, ribUnicastEntry] : staticUnicastRoutes_) {
    if (routeDb.unicastRoutes.count(prefix)) {
      // ignore prefixes as prefixState has higher priority
      continue;
    }
    routeDb.addUnicastRoute(RibUnicastEntry(ribUnicastEntry));
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  XLOG(INFO) << "Decision::buildRouteDb took " << deltaTime.count() << "ms.";
  fb303::fbData->addStatValue(
      "decision.route_build_ms", deltaTime.count(), fb303::AVG);
  return routeDb;
} // buildRouteDb

RouteSelectionResult
SpfSolver::selectBestRoutes(
    std::string const& myNodeName,
    folly::CIDRNetwork const& prefix,
    PrefixEntries& prefixEntries,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  CHECK(prefixEntries.size()) << "No prefixes for best route selection";
  RouteSelectionResult ret;

  // Filter out nodes that are hard drained (overloaded set), noop if all
  // destination are overloaded
  auto filteredPrefixes = filterHardDrainedNodes(prefixEntries, areaLinkStates);
  auto softDrainedNodes = getSoftDrainedNodes(prefixEntries, areaLinkStates);

  if (enableBestRouteSelection_) {
    // Perform best route selection based on metrics
    ret.allNodeAreas = selectRoutes(
        filteredPrefixes,
        thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE,
        softDrainedNodes);
    ret.bestNodeArea = selectBestNodeArea(ret.allNodeAreas, myNodeName);
  } else {
    // If it is openr route, all nodes are considered as best nodes.
    // Except for drained
    for (auto const& [nodeAndArea, prefixEntry] : filteredPrefixes) {
      ret.allNodeAreas.emplace(nodeAndArea);
    }
    ret.bestNodeArea = *ret.allNodeAreas.begin();
  }

  if (isNodeDrained(ret.bestNodeArea, areaLinkStates)) {
    // Decision will change RibEntry's drain_metric to 1 if isBestNodeDrained is
    // true, when it creates routeDB. So nodes in other areas would know
    // that this forwarding path has a drained node when RibEntry is
    // redistributed
    ret.isBestNodeDrained = true;
  }

  return ret;
}

std::optional<int64_t>
SpfSolver::getMinNextHopThreshold(
    RouteSelectionResult nodes, PrefixEntries const& prefixEntries) {
  std::optional<int64_t> maxMinNexthopForPrefix = std::nullopt;
  for (const auto& nodeArea : nodes.allNodeAreas) {
    const auto& prefixEntry = prefixEntries.at(nodeArea);
    maxMinNexthopForPrefix = prefixEntry->minNexthop().has_value() &&
            (not maxMinNexthopForPrefix.has_value() ||
             prefixEntry->minNexthop().value() > maxMinNexthopForPrefix.value())
        ? prefixEntry->minNexthop().value()
        : maxMinNexthopForPrefix;
  }
  return maxMinNexthopForPrefix;
}

std::unordered_set<NodeAndArea>
SpfSolver::getSoftDrainedNodes(
    PrefixEntries& prefixes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  std::unordered_set<NodeAndArea> softDrainedNodes;
  for (auto& [nodeArea, metricsWrapper] : prefixes) {
    const auto& [node, area] = nodeArea;
    int softDrainValue = areaLinkStates.at(area).getNodeMetricIncrement(node);
    if (softDrainValue > 0) {
      softDrainedNodes.emplace(nodeArea);
    }
  }
  return softDrainedNodes;
}

PrefixEntries
SpfSolver::filterHardDrainedNodes(
    PrefixEntries& prefixes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  PrefixEntries filtered = folly::copy(prefixes);
  for (auto iter = filtered.cbegin(); iter != filtered.cend();) {
    const auto& [node, area] = iter->first;
    if (areaLinkStates.at(area).isNodeOverloaded(node)) {
      iter = filtered.erase(iter);
    } else {
      ++iter;
    }
  }
  // Erase hard-drained nodes as candidates, unless everything is hard-drained
  return filtered.empty() ? prefixes : filtered;
}

bool
SpfSolver::isNodeDrained(
    const NodeAndArea& nodeArea,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  const auto& [node, area] = nodeArea;
  const auto& linkState = areaLinkStates.at(area);
  return linkState.isNodeOverloaded(node) or
      linkState.getNodeMetricIncrement(node) != 0;
}

SpfSolver::SpfAreaResults
SpfSolver::selectBestPathsSpf(
    std::string const& myNodeName,
    folly::CIDRNetwork const& prefix,
    RouteSelectionResult const& routeSelectionResult,
    const std::string& area,
    const LinkState& linkState) {
  /*
   * [Next hop Calculation]
   *
   * This step will calcuate the NH set with metric:
   *
   *  current node(myNodeName) ->
   *  dst node(prefix originator selected inside `RouteSelectionResult`)
   *
   * NOTE: the returned result contains best metric along with NH set.
   */
  SpfAreaResults result;
  const BestNextHopMetrics nextHopsWithMetric = getNextHopsWithMetric(
      myNodeName, routeSelectionResult.allNodeAreas, linkState);

  // Populate the SPF result
  result.bestMetric = nextHopsWithMetric.first;
  if (nextHopsWithMetric.second.empty()) {
    XLOG(DBG3) << "No route to prefix "
               << folly::IPAddress::networkToString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return result;
  }

  result.nextHops = getNextHopsThrift(
      myNodeName,
      routeSelectionResult.allNodeAreas,
      prefix.first.isV4(), /* isV4Prefix */
      nextHopsWithMetric,
      area,
      linkState);

  return result;
}

std::optional<RibUnicastEntry>
SpfSolver::addBestPaths(
    const std::string& myNodeName,
    const folly::CIDRNetwork& prefix,
    const RouteSelectionResult& routeSelectionResult,
    const PrefixEntries& prefixEntries,
    std::unordered_set<thrift::NextHopThrift>&& nextHops,
    const Metric shortestMetric,
    const bool localPrefixConsidered) {
  // Check if next-hop list is empty
  if (nextHops.empty()) {
    return std::nullopt;
  }

  // Apply min-nexthop requirements. Ignore the route from programming if
  // min-nexthop requirement is not met.
  auto minNextHop = getMinNextHopThreshold(routeSelectionResult, prefixEntries);
  if (minNextHop.has_value() and minNextHop.value() > nextHops.size()) {
    XLOG(WARNING) << "Ignore programming of route to "
                  << folly::IPAddress::networkToString(prefix)
                  << " because of min-nexthop requirement. "
                  << "Minimum required " << minNextHop.value() << ", got "
                  << nextHops.size();
    return std::nullopt;
  }

  auto entry =
      *(prefixEntries.at(routeSelectionResult.bestNodeArea)); // copy intended
  // We don't modify original prefixEntries (referenced from prefixState)
  // because they reflect the prefix entries we received from others.
  if (routeSelectionResult.isBestNodeDrained) {
    *entry.metrics()->drain_metric() = 1;
  }

  // Create RibUnicastEntry and add it the list
  return RibUnicastEntry(
      prefix,
      std::move(nextHops),
      std::move(entry),
      routeSelectionResult.bestNodeArea.second,
      false, /* doNotInstall */
      shortestMetric,
      std::nullopt, /* ucmp weight */
      localPrefixConsidered);
}

/*
 * This util function will return the pair of:
 *  - min metric from SRC to DST node;
 *  - a map of NH node and the shortest distance from NH -> DST node;
 *
 * ATTN: the metric in pair.first is DIFFERENT from metric inside pair.second!
 */
BestNextHopMetrics
SpfSolver::getNextHopsWithMetric(
    const std::string& myNodeName,
    const std::set<NodeAndArea>& dstNodeAreas,
    const LinkState& linkState) {
  // build up next hop nodes that are along a shortest path to the prefix
  std::unordered_map<
      std::string /* nextHopNodeName */,
      Metric /* the distance from the nexthop to the dest */>
      nextHopNodes;
  Metric shortestMetric = std::numeric_limits<Metric>::max();

  auto const& spfResult = linkState.getSpfResult(myNodeName);

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

  // Add neighbors with shortest path to the prefix
  for (const auto& dstNode : minCostNodes) {
    for (const auto& nhName : spfResult.at(dstNode).nextHops()) {
      nextHopNodes[nhName] = shortestMetric -
          linkState.getMetricFromAToB(myNodeName, nhName).value();
    }
  }

  return std::make_pair(shortestMetric, nextHopNodes);
}

std::unordered_set<thrift::NextHopThrift>
SpfSolver::getNextHopsThrift(
    const std::string& myNodeName,
    const std::set<NodeAndArea>& dstNodeAreas,
    bool isV4,
    const BestNextHopMetrics& bestNextHopMetrics,
    const std::string& area,
    const LinkState& linkState) const {
  // Use reference to avoid copy
  const auto& nextHopNodes = bestNextHopMetrics.second;
  const auto& minMetric = bestNextHopMetrics.first;
  CHECK(not nextHopNodes.empty());

  std::unordered_set<thrift::NextHopThrift> nextHops;
  for (const auto& link : linkState.linksFromNode(myNodeName)) {
    const auto neighborNode = link->getOtherNodeName(myNodeName);
    const auto search = nextHopNodes.find(neighborNode);

    /*
     * [Interface Hard-Drain]
     *
     * When interface is hard-drained, aka, with overload bit set,
     *
     * link->isUp() returns false
     *
     * with either side of the adjacency marked as `overloaded`.
     *
     * Ignore overloaded links or nexthops.
     */
    if (search == nextHopNodes.end() or not link->isUp()) {
      continue;
    }

    /*
     * [Interface Soft-Drain]
     *
     * When interface is soft-drained, aka, with metric overridden,
     *
     * link->getMaxMetric() returns the larger metric among both ends.
     *
     * In case interface drain is operated only on one side,
     *
     * nodeA(if_a_b)[DRAINED] - (if_b_a)nodeB
     *
     * nodeB will still be able to detect interface soft-drain with higher
     * metric and re-calculate SPF path.
     *
     * Ignore nexthops that are not shortest.
     */
    Metric distOverLink = link->getMaxMetric() + search->second;
    if (distOverLink != minMetric) {
      continue;
    }

    nextHops.emplace(createNextHop(
        isV4 and not v4OverV6Nexthop_ ? link->getNhV4FromNode(myNodeName)
                                      : link->getNhV6FromNode(myNodeName),
        link->getIfaceFromNode(myNodeName),
        distOverLink,
        std::nullopt, /* mplsAction */
        link->getArea(),
        link->getOtherNodeName(myNodeName),
        0 /* ucmp weight */));
  }
  return nextHops;
}

} // namespace openr
