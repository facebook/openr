/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/RibEntry.h>
#include <openr/decision/SpfSolver.h>
#include "openr/if/gen-cpp2/OpenrConfig_types.h"

namespace fb303 = facebook::fb303;

using apache::thrift::can_throw;
using Metric = openr::LinkStateMetric;

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

  // mplsRoutesToUpdate
  for (auto& [label, entry] : newDb.mplsRoutes) {
    const auto& search = mplsRoutes.find(label);
    if (search == mplsRoutes.end() || search->second != entry) {
      delta.addMplsRouteToUpdate(std::move(entry));
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
  for (auto const& [_, entry] : update.mplsRoutesToUpdate) {
    mplsRoutes.insert_or_assign(entry.label, entry);
  }
}

SpfSolver::SpfSolver(
    std::shared_ptr<const Config> config,
    const std::string& myNodeName,
    bool enableV4,
    bool enableNodeSegmentLabel,
    bool enableAdjacencyLabels,
    bool enableBgpRouteProgramming,
    bool enableBestRouteSelection,
    bool v4OverV6Nexthop,
    const std::optional<std::vector<thrift::SrPolicy>>& srPoliciesConfig,
    const std::optional<neteng::config::routing_policy::PolicyConfig>&
        areaPolicyConfig)
    : myNodeName_(myNodeName),
      enableV4_(enableV4),
      enableNodeSegmentLabel_(enableNodeSegmentLabel),
      enableAdjacencyLabels_(enableAdjacencyLabels),
      enableBgpRouteProgramming_(enableBgpRouteProgramming),
      enableBestRouteSelection_(enableBestRouteSelection),
      v4OverV6Nexthop_(v4OverV6Nexthop) {
  // Initialize stat keys
  fb303::fbData->addStatExportType("decision.adj_db_update", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.incompatible_forwarding_type", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.no_route_to_label", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.no_route_to_prefix", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.path_build_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.prefix_db_update", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.route_build_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.route_build_runs", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.get_route_for_prefix", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.skipped_mpls_route", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.duplicate_node_label", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "decision.skipped_unicast_route", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.spf_ms", fb303::AVG);
  fb303::fbData->addStatExportType("decision.spf_runs", fb303::COUNT);
  fb303::fbData->addStatExportType("decision.errors", fb303::COUNT);

  // Create SrPolicy internal classes
  if (srPoliciesConfig && areaPolicyConfig) {
    for (const auto& srPolicy : *srPoliciesConfig) {
      srPolicies_.emplace_back(srPolicy, *areaPolicyConfig->definitions_ref());
    }
  }

  prependLabelAllocator_ =
      std::make_unique<PrependLabelAllocator<thrift::NextHopThrift>>(config);
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

void
SpfSolver::updateStaticMplsRoutes(
    const std::unordered_map<int32_t, RibMplsEntry>& mplsRoutesToUpdate,
    const std::vector<int32_t>& mplsRoutesToDelete) {
  // Process MPLS routes to add or update
  XLOG_IF(INFO, mplsRoutesToUpdate.size())
      << "Adding/Updating " << mplsRoutesToUpdate.size()
      << " static mpls routes.";
  for (const auto& [label, mplsRoute] : mplsRoutesToUpdate) {
    staticMplsRoutes_.insert_or_assign(label, mplsRoute);

    XLOG(DBG1) << "> " << label
               << ", NextHopsCount = " << mplsRoute.nexthops.size();
    for (auto const& nh : mplsRoute.nexthops) {
      XLOG(DBG2) << " via " << toString(nh);
    }
  }

  XLOG_IF(INFO, mplsRoutesToDelete.size())
      << "Deleting " << mplsRoutesToDelete.size() << " static mpls routes.";
  for (const auto& topLabel : mplsRoutesToDelete) {
    staticMplsRoutes_.erase(topLabel);

    XLOG(DBG1) << "> " << std::to_string(topLabel);
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
    fb303::fbData->addStatValue(
        "decision.skipped_unicast_route", 1, fb303::COUNT);
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
  for (auto& [area, linkState] : areaLinkStates) {
    auto const& mySpfResult = linkState.getSpfResult(myNodeName);

    // Delete entries of unreachable nodes from prefixEntries
    for (auto it = prefixEntries.cbegin(); it != prefixEntries.cend();) {
      const auto& [prefixNode, prefixArea] = it->first;
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
    XLOG(DBG3) << "Skipping route to "
               << folly::IPAddress::networkToString(prefix)
               << " with no reachable node.";
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  // TODO: These variables are deprecated and will go away soon
  bool hasBGP = false, hasNonBGP = false, missingMv = false;

  // Boolean flag indicating whether current node advertises the prefix entry
  // with prepend label.
  bool hasSelfPrependLabel{true};

  // TODO: With new PrefixMetrics we no longer treat routes differently based
  // on their origin source aka `prefixEntry.type`
  for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
    bool isBGP = prefixEntry->type_ref().value() == thrift::PrefixType::BGP;
    hasBGP |= isBGP;
    hasNonBGP |= !isBGP;
    if (nodeAndArea.first == myNodeName) {
      hasSelfPrependLabel &= prefixEntry->prependLabel_ref().has_value();
    }
    if (isBGP and not prefixEntry->mv_ref().has_value()) {
      missingMv = true;
      XLOG_IF(ERR, not enableBestRouteSelection_)
          << "Prefix entry for " << folly::IPAddress::networkToString(prefix)
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
      XLOG(ERR) << "Skipping route for "
                << folly::IPAddress::networkToString(prefix)
                << " which is advertised with BGP and non-BGP type.";
      fb303::fbData->addStatValue(
          "decision.skipped_unicast_route", 1, fb303::COUNT);
      return std::nullopt;
    }
    if (missingMv and not enableBestRouteSelection_) {
      XLOG(ERR) << "Skipping route for "
                << folly::IPAddress::networkToString(prefix)
                << " at least one advertiser is missing its metric vector.";
      fb303::fbData->addStatValue(
          "decision.skipped_unicast_route", 1, fb303::COUNT);
      return std::nullopt;
    }
  }

  auto routeSelectionResult = selectBestRoutes(
      myNodeName, prefix, prefixEntries, hasBGP, areaLinkStates);
  if (not routeSelectionResult.success) {
    return std::nullopt;
  }
  if (routeSelectionResult.allNodeAreas.empty()) {
    XLOG(WARNING) << "No route to prefix "
                  << folly::IPAddress::networkToString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::nullopt;
  }

  // Set best route selection in prefix state
  bestRoutesCache_.insert_or_assign(prefix, routeSelectionResult);

  // Skip adding route for one prefix advertised by current node in all
  // following scenarios:
  // - Scenario1: The node has routes of fine-granularity prefixes
  //   (e.g., 10.0.0.0/25 and 10.0.0.128/25) programmed locally. It originates
  //   the aggregated prefix (e.g., 10.0.0.0/24) to attract traffic.
  // - Scenario2: The node locates in multi areas and the prefix is
  //   distributed cross areas (e.g., from previous area1 to current area2). IP
  //   routes for the prefix were programmed when OpenR handled the prefix in
  //   previous area1.
  // Other scenarios: re-distributed from other protocols (unclear),
  //   interface-subnets, etc;
  //
  // TODO: We program self advertise prefix only iff, we're advertising our
  // prefix-entry with the prepend label. Once we support multi-area routing,
  // we can deprecate the check of hasSelfPrependLabel
  if (routeSelectionResult.hasNode(myNodeName) and !hasSelfPrependLabel) {
    XLOG(DBG3) << "Skip adding route for prefixes advertised by " << myNodeName
               << " " << folly::IPAddress::networkToString(prefix);
    return std::nullopt;
  }

  // Match the best route's attributes (tags & area stack) to an SR Policy.
  // If the route doesn't match to any, default rules are returned
  auto routeComputationRules = getRouteComputationRules(
      prefixEntries, routeSelectionResult, areaLinkStates);

  // Avoid duplicated efforts with selectBestRoutes() in case of
  // SHORTEST_DISTANCE route selection algorithm.
  auto routeSelectionAlgo = routeComputationRules.get_routeSelectionAlgo();
  if (routeSelectionAlgo !=
      thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE) {
    extendRoutes(
        routeSelectionAlgo,
        prefixEntries,
        areaLinkStates,
        routeSelectionResult);
  }

  //
  // Route computation flow
  // - For each area:
  //    - Switch on algorithm type
  //    - Compute paths, algorithm type influences this step (SP or KSP2)
  //    - Create next-hops from paths, forwarding type influences this step
  //    - Only use the next-hop set if it has the shortest metric
  //        - TODO: support this functionality for KSP2 forwarding algorithm
  //    - Combine shortest metric next-hops from all area
  std::unordered_set<thrift::NextHopThrift> totalNextHops;
  std::unordered_set<thrift::NextHopThrift> ksp2NextHops;
  Metric shortestMetric = std::numeric_limits<Metric>::max();
  for (const auto& [area, areaRules] :
       *routeComputationRules.areaPathComputationRules_ref()) {
    const auto& linkState = areaLinkStates.find(area);
    if (linkState == areaLinkStates.end()) {
      // Possible if the route computation rules are based of a configured SR
      // Policy which contains area path computation rules for an invalid area.
      //
      // If the route computation rules are default then area path computation
      // rules will only contains valid areas.
      continue;
    }

    switch (*areaRules.forwardingAlgo_ref()) {
    case thrift::PrefixForwardingAlgorithm::SP_ECMP: {
      auto metricsAreaNextHops = selectBestPathsSpf(
          myNodeName,
          prefix,
          routeSelectionResult,
          prefixEntries,
          hasBGP,
          *areaRules.forwardingType_ref(),
          area,
          linkState->second,
          prefixState);
      // Only use next-hops in areas with the shortest IGP metric
      //
      // TODO: bypass this code to allow UCMP paths between areas if
      // new RouteComputationRules flag allowUcmpPathsBetweenAreas is true
      if (shortestMetric >= metricsAreaNextHops.first) {
        if (shortestMetric > metricsAreaNextHops.first) {
          shortestMetric = metricsAreaNextHops.first;
          totalNextHops.clear();
        }
        totalNextHops.insert(
            metricsAreaNextHops.second.begin(),
            metricsAreaNextHops.second.end());
      }
    } break;
    case thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP: {
      // T96779848: selectBestPathsKsp2() should only use selected
      // routes with the best IGP metrics (similar to selectBestPathsSpf)
      // Also next-hops returned by selectBestPathsKsp2() should only be
      // used if they have the best IGP metrics compared to other areas.
      // Comment above for T96776309 also applies here as well.
      auto areaNextHops = selectBestPathsKsp2(
          myNodeName,
          prefix,
          routeSelectionResult,
          prefixEntries,
          hasBGP,
          *areaRules.forwardingType_ref(),
          area,
          linkState->second);
      ksp2NextHops.insert(areaNextHops.begin(), areaNextHops.end());
    } break;
    default:
      XLOG(ERR)
          << "Unknown prefix algorithm type "
          << apache::thrift::util::enumNameSafe(*areaRules.forwardingAlgo_ref())
          << " for prefix " << folly::IPAddress::networkToString(prefix);
      break;
    }

    // Merge nexthops from SP and KSP2 path computations.
    totalNextHops.insert(ksp2NextHops.begin(), ksp2NextHops.end());
  }

  return addBestPaths(
      myNodeName,
      prefix,
      routeSelectionResult,
      prefixEntries,
      hasBGP,
      std::move(totalNextHops),
      shortestMetric);

  // SrPolicy TODO: (T94500292) before returning need to apply prepend label
  // rules. Prepend label rules, may create a new MPLS route (RibMplsEntry)
  // that needs to be return along with the IP route (RibUnicastEntry)
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

  //
  // Create MPLS routes for all nodeLabel
  //
  if (enableNodeSegmentLabel_) {
    std::unordered_map<int32_t, std::pair<std::string, RibMplsEntry>>
        labelToNode;
    for (const auto& [area, linkState] : areaLinkStates) {
      for (const auto& [_, adjDb] : linkState.getAdjacencyDatabases()) {
        const auto topLabel = *adjDb.nodeLabel_ref();
        const auto& nodeName = *adjDb.thisNodeName_ref();
        // Top label is not set => Non-SR mode
        if (topLabel == 0) {
          XLOG(INFO) << "Ignoring node label " << topLabel << " of node "
                     << nodeName << " in area " << area;
          fb303::fbData->addStatValue(
              "decision.skipped_mpls_route", 1, fb303::COUNT);
          continue;
        }
        // If mpls label is not valid then ignore it
        if (not isMplsLabelValid(topLabel)) {
          XLOG(ERR) << "Ignoring invalid node label " << topLabel << " of node "
                    << nodeName << " in area " << area;
          fb303::fbData->addStatValue(
              "decision.skipped_mpls_route", 1, fb303::COUNT);
          continue;
        }

        // There can be a temporary collision in node label allocation.
        // Usually happens when two segmented networks allocating labels from
        // the same range join together. In case of such conflict we respect
        // the node label of bigger node-ID
        auto iter = labelToNode.find(topLabel);
        if (iter != labelToNode.end()) {
          XLOG(INFO) << "Found duplicate label " << topLabel << "from "
                     << iter->second.first << " " << nodeName << " in area "
                     << area;
          fb303::fbData->addStatValue(
              "decision.duplicate_node_label", 1, fb303::COUNT);
          if (iter->second.first < nodeName) {
            continue;
          }
        }

        // Install POP_AND_LOOKUP for next layer
        if (*adjDb.thisNodeName_ref() == myNodeName) {
          thrift::NextHopThrift nh;
          nh.address_ref() = toBinaryAddress(folly::IPAddressV6("::"));
          nh.area_ref() = area;
          nh.mplsAction_ref() =
              createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
          labelToNode.erase(topLabel);
          labelToNode.emplace(
              topLabel,
              std::make_pair(myNodeName, RibMplsEntry(topLabel, {nh})));
          continue;
        }

        // Get best nexthop towards the node
        auto metricNhs = getNextHopsWithMetric(
            myNodeName,
            {{adjDb.thisNodeName_ref().value(), area}},
            false,
            linkState);
        if (metricNhs.second.empty()) {
          XLOG(WARNING) << "No route to nodeLabel " << std::to_string(topLabel)
                        << " of node " << nodeName;
          fb303::fbData->addStatValue(
              "decision.no_route_to_label", 1, fb303::COUNT);
          continue;
        }

        // Create nexthops with appropriate MplsAction (PHP and SWAP). Note
        // that all nexthops are valid for routing without loops. Fib is
        // responsible for installing these routes by making sure it programs
        // least cost nexthops first and of same action type (based on HW
        // limitations)
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
                        false /* isV4 */,
                        v4OverV6Nexthop_,
                        false /* perDestination */,
                        metricNhs.first,
                        metricNhs.second,
                        topLabel,
                        area,
                        linkState))));
      }
    }

    for (auto& [_, nodeToEntry] : labelToNode) {
      routeDb.addMplsRoute(std::move(nodeToEntry.second));
    }
  }

  //
  // Create MPLS routes for all of our adjacencies
  //
  if (enableAdjacencyLabels_) {
    for (const auto& [_, linkState] : areaLinkStates) {
      for (const auto& link : linkState.linksFromNode(myNodeName)) {
        const auto topLabel = link->getAdjLabelFromNode(myNodeName);
        // Top label is not set => Non-SR mode
        if (topLabel == 0) {
          continue;
        }
        // If mpls label is not valid then ignore it
        if (not isMplsLabelValid(topLabel)) {
          XLOG(ERR) << "Ignoring invalid adjacency label " << topLabel
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
  }

  //
  // Add MPLS static routes
  //
  for (const auto& [_, mplsEntry] : staticMplsRoutes_) {
    routeDb.addMplsRoute(RibMplsEntry(mplsEntry));
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
    PrefixEntries const& prefixEntries,
    bool const isBgp,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  CHECK(prefixEntries.size()) << "No prefixes for best route selection";
  RouteSelectionResult ret;

  if (enableBestRouteSelection_) {
    // Perform best route selection based on metrics
    ret.allNodeAreas = selectRoutes(
        prefixEntries, thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE);
    ret.bestNodeArea =
        selectBestNodeArea(ret.allNodeAreas, myNodeName, areaLinkStates);
    ret.success = true;
  } else if (isBgp) {
    ret = runBestPathSelectionBgp(prefix, prefixEntries, areaLinkStates);
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

void
SpfSolver::extendRoutes(
    const thrift::RouteSelectionAlgorithm algorithm,
    const PrefixEntries& prefixEntries,
    const std::unordered_map<std::string, LinkState>& areaLinkStates,
    RouteSelectionResult& selectedRoutes) {
  // Select routes according to specified algorithm, and store the results in
  // selectedRoutes.allNodeAreas.
  for (const auto& nodeArea : selectRoutes(prefixEntries, algorithm)) {
    selectedRoutes.allNodeAreas.insert(nodeArea);
  }
  selectedRoutes =
      maybeFilterDrainedNodes(std::move(selectedRoutes), areaLinkStates);
}

std::optional<int64_t>
SpfSolver::getMinNextHopThreshold(
    RouteSelectionResult nodes, PrefixEntries const& prefixEntries) {
  std::optional<int64_t> maxMinNexthopForPrefix = std::nullopt;
  for (const auto& nodeArea : nodes.allNodeAreas) {
    const auto& prefixEntry = prefixEntries.at(nodeArea);
    maxMinNexthopForPrefix = prefixEntry->minNexthop_ref().has_value() &&
            (not maxMinNexthopForPrefix.has_value() ||
             prefixEntry->minNexthop_ref().value() >
                 maxMinNexthopForPrefix.value())
        ? prefixEntry->minNexthop_ref().value()
        : maxMinNexthopForPrefix;
  }
  return maxMinNexthopForPrefix;
}

RouteSelectionResult
SpfSolver::maybeFilterDrainedNodes(
    RouteSelectionResult&& result,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  RouteSelectionResult filtered = folly::copy(result);
  for (auto iter = filtered.allNodeAreas.cbegin();
       iter != filtered.allNodeAreas.cend();) {
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
    filtered.bestNodeArea =
        selectBestNodeArea(filtered.allNodeAreas, myNodeName_, areaLinkStates);
  }

  return filtered.allNodeAreas.empty() ? result : filtered;
}

RouteSelectionResult
SpfSolver::runBestPathSelectionBgp(
    folly::CIDRNetwork const& prefix,
    PrefixEntries const& prefixEntries,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  RouteSelectionResult ret;
  std::optional<thrift::MetricVector> bestVector;
  for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
    switch (bestVector.has_value()
                ? MetricVectorUtils::compareMetricVectors(
                      can_throw(*prefixEntry->mv_ref()), *bestVector)
                : MetricVectorUtils::CompareResult::WINNER) {
    case MetricVectorUtils::CompareResult::WINNER:
      ret.allNodeAreas.clear();
      FOLLY_FALLTHROUGH;
    case MetricVectorUtils::CompareResult::TIE_WINNER:
      bestVector = can_throw(*prefixEntry->mv_ref());
      ret.bestNodeArea = nodeAndArea;
      FOLLY_FALLTHROUGH;
    case MetricVectorUtils::CompareResult::TIE_LOOSER:
      ret.allNodeAreas.emplace(nodeAndArea);
      break;
    case MetricVectorUtils::CompareResult::TIE:
      XLOG(ERR) << "Tie ordering prefix entries. Skipping route for "
                << folly::IPAddress::networkToString(prefix);
      return ret;
    case MetricVectorUtils::CompareResult::ERROR:
      XLOG(ERR) << "Error ordering prefix entries. Skipping route for "
                << folly::IPAddress::networkToString(prefix);
      return ret;
    default:
      break;
    }
  }
  ret.success = true;
  return maybeFilterDrainedNodes(std::move(ret), areaLinkStates);
}

std::pair<Metric, std::unordered_set<thrift::NextHopThrift>>
SpfSolver::selectBestPathsSpf(
    std::string const& myNodeName,
    folly::CIDRNetwork const& prefix,
    RouteSelectionResult const& routeSelectionResult,
    PrefixEntries const& prefixEntries,
    bool const isBgp,
    thrift::PrefixForwardingType const& forwardingType,
    const std::string& area,
    const LinkState& linkState,
    PrefixState const& prefixState) {
  const bool isV4Prefix = prefix.first.isV4();
  const bool perDestination =
      forwardingType == thrift::PrefixForwardingType::SR_MPLS;

  // Special case for programming imported next-hops during route origination.
  // This case, programs the next-hops learned from external processes while
  // importing route, along with the computed next-hops.
  //
  // TODO: This is one off the hack to unblock special routing needs. With
  // complete support of multi-area setup, we can delete the following code
  // block.
  auto filteredBestNodeAreas = routeSelectionResult.allNodeAreas;
  if (routeSelectionResult.hasNode(myNodeName) and perDestination) {
    for (const auto& [nodeAndArea, prefixEntry] : prefixEntries) {
      if (nodeAndArea.first == myNodeName and prefixEntry->prependLabel_ref()) {
        filteredBestNodeAreas.erase(nodeAndArea);
        break;
      }
    }
  }

  // Get next-hops
  const auto nextHopsWithMetric = getNextHopsWithMetric(
      myNodeName, filteredBestNodeAreas, perDestination, linkState);
  if (nextHopsWithMetric.second.empty()) {
    XLOG(DBG3) << "No route to prefix "
               << folly::IPAddress::networkToString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return std::make_pair(
        nextHopsWithMetric.first, std::unordered_set<thrift::NextHopThrift>());
  }

  return std::make_pair(
      nextHopsWithMetric.first,
      getNextHopsThrift(
          myNodeName,
          routeSelectionResult.allNodeAreas,
          isV4Prefix,
          v4OverV6Nexthop_,
          perDestination,
          nextHopsWithMetric.first,
          nextHopsWithMetric.second,
          std::nullopt /* swapLabel */,
          area,
          linkState,
          prefixEntries));
}

std::unordered_set<thrift::NextHopThrift>
SpfSolver::selectBestPathsKsp2(
    const std::string& myNodeName,
    const folly::CIDRNetwork& prefix,
    RouteSelectionResult const& routeSelectionResult,
    PrefixEntries const& prefixEntries,
    bool isBgp,
    thrift::PrefixForwardingType const& forwardingType,
    const std::string& area,
    const LinkState& linkState) {
  std::unordered_set<thrift::NextHopThrift> nextHops;
  std::vector<LinkState::Path> paths;

  // Sanity check for forwarding type
  if (forwardingType != thrift::PrefixForwardingType::SR_MPLS) {
    XLOG(ERR) << "Incompatible forwarding type "
              << apache ::thrift::util::enumNameSafe(forwardingType)
              << " for algorithm KSPF2_ED_ECMP of "
              << folly::IPAddress::networkToString(prefix);

    fb303::fbData->addStatValue(
        "decision.incompatible_forwarding_type", 1, fb303::COUNT);
    return nextHops;
  }

  // find shortest and sec shortest routes towards each node.
  for (const auto& [node, bestArea] : routeSelectionResult.allNodeAreas) {
    // if ourself is considered as ECMP nodes.
    if (node == myNodeName and bestArea == area) {
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
  for (const auto& [node, bestArea] : routeSelectionResult.allNodeAreas) {
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

  if (paths.size() == 0) {
    return nextHops;
  }

  for (const auto& path : paths) {
    Metric cost = 0;
    std::list<int32_t> labels;
    // if self node is one of it's ecmp, it means this prefix is anycast and
    // we need to add prepend label which is static MPLS route the destination
    // prepared.
    std::vector<std::string> invalidNodes;
    auto nextNodeName = myNodeName;
    for (auto& link : path) {
      cost += link->getMetricFromNode(nextNodeName);
      nextNodeName = link->getOtherNodeName(nextNodeName);
      auto& adjDb = linkState.getAdjacencyDatabases().at(nextNodeName);
      labels.push_front(adjDb.get_nodeLabel());
      if (not isMplsLabelValid(adjDb.get_nodeLabel())) {
        invalidNodes.emplace_back(adjDb.get_thisNodeName());
      }
    }
    // Ignore paths including nodes with invalid node labels.
    if (invalidNodes.size() > 0) {
      XLOG(WARNING) << fmt::format(
          "Ignore path for {} through [{}] because of invalid node label.",
          folly::IPAddress::networkToString(prefix),
          folly::join(", ", invalidNodes));
      continue;
    }
    labels.pop_back(); // Remove first node's label to respect PHP

    // Add prepend label of last node in the path.
    auto lastNodeInPath = nextNodeName;
    auto& prefixEntry = prefixEntries.at({lastNodeInPath, area});
    if (prefixEntry->prependLabel_ref()) {
      // add prepend label to bottom of the stack
      labels.push_front(prefixEntry->prependLabel_ref().value());
    }

    // Create nexthop
    CHECK_GE(path.size(), 1);
    auto const& firstLink = path.front();
    std::optional<thrift::MplsAction> mplsAction;
    if (labels.size()) {
      std::vector<int32_t> labelVec{labels.cbegin(), labels.cend()};
      mplsAction = createMplsAction(
          thrift::MplsActionCode::PUSH, std::nullopt, std::move(labelVec));
    }

    bool isV4Prefix = prefix.first.isV4();
    nextHops.emplace(createNextHop(
        isV4Prefix and not v4OverV6Nexthop_
            ? firstLink->getNhV4FromNode(myNodeName)
            : firstLink->getNhV6FromNode(myNodeName),
        firstLink->getIfaceFromNode(myNodeName),
        cost,
        mplsAction,
        firstLink->getArea(),
        firstLink->getOtherNodeName(myNodeName)));
  }

  return nextHops;
}

std::optional<RibUnicastEntry>
SpfSolver::addBestPaths(
    const std::string& myNodeName,
    const folly::CIDRNetwork& prefix,
    const RouteSelectionResult& routeSelectionResult,
    const PrefixEntries& prefixEntries,
    const bool isBgp,
    std::unordered_set<thrift::NextHopThrift>&& nextHops,
    const Metric shortestMetric) {
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

  // Special case for programming imported next-hops during route origination.
  // This case, programs the next-hops learned from external processes while
  // importing route, along with the computed next-hops.
  // TODO: This is one off the hack to unblock special routing needs. With
  // complete support of multi-area setup, we won't need this any longer.
  if (routeSelectionResult.hasNode(myNodeName)) {
    std::optional<int32_t> prependLabel;
    for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
      if (nodeAndArea.first == myNodeName and prefixEntry->prependLabel_ref()) {
        prependLabel = prefixEntry->prependLabel_ref().value();
        break;
      }
    }

    // Self route must be advertised with prepend label
    CHECK(prependLabel.has_value());

    // Add static next-hops
    auto routeIter = staticMplsRoutes_.find(prependLabel.value());
    if (routeIter != staticMplsRoutes_.end()) {
      for (const auto& nh : routeIter->second.nexthops) {
        nextHops.emplace(createNextHop(
            nh.address_ref().value(), std::nullopt, 0, std::nullopt));
      }
    } else {
      XLOG(ERR) << "Static nexthops do not exist for static mpls label "
                << prependLabel.value();
    }
  }

  // Create RibUnicastEntry and add it the list
  return RibUnicastEntry(
      prefix,
      std::move(nextHops),
      *(prefixEntries.at(routeSelectionResult.bestNodeArea)),
      routeSelectionResult.bestNodeArea.second,
      isBgp & (not enableBgpRouteProgramming_), // doNotInstall
      shortestMetric);
}

std::pair<
    Metric /* min metric to destination */,
    std::unordered_map<
        std::pair<std::string /* nextHopNodeName */, std::string /* dstNode */>,
        Metric /* the distance from the nexthop to the dest */>>
SpfSolver::getNextHopsWithMetric(
    const std::string& myNodeName,
    const std::set<NodeAndArea>& dstNodeAreas,
    bool perDestination,
    const LinkState& linkState) {
  // build up next hop nodes that are along a shortest path to the prefix
  std::unordered_map<
      std::pair<std::string /* nextHopNodeName */, std::string /* dstNode */>,
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
    const auto dstNodeRef = perDestination ? dstNode : "";
    for (const auto& nhName : spfResult.at(dstNode).nextHops()) {
      nextHopNodes[std::make_pair(nhName, dstNodeRef)] = shortestMetric -
          linkState.getMetricFromAToB(myNodeName, nhName).value();
    }
  }

  return std::make_pair(shortestMetric, nextHopNodes);
}

// TODO Let's use strong-types for the bools to detect any abusement at the
// building time.
std::unordered_set<thrift::NextHopThrift>
SpfSolver::getNextHopsThrift(
    const std::string& myNodeName,
    const std::set<NodeAndArea>& dstNodeAreas,
    bool isV4,
    bool v4OverV6Nexthop,
    bool perDestination,
    const Metric minMetric,
    std::unordered_map<std::pair<std::string, std::string>, Metric>
        nextHopNodes,
    std::optional<int32_t> swapLabel,
    const std::string& area,
    const LinkState& linkState,
    PrefixEntries const& prefixEntries) const {
  // Note: perDestination flag determines the next-hop search range by whether
  // filtering those not-in dstNodeAreas or not.
  // TODO: Reorg this function to make the logic cleaner, and cleanup unused
  // code.

  CHECK(not nextHopNodes.empty());

  std::unordered_set<thrift::NextHopThrift> nextHops;
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

      // Ignore nexthops that are not shortest
      Metric distOverLink =
          link->getMetricFromNode(myNodeName) + search->second;
      if (distOverLink != minMetric) {
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

      // Create associated mpls action if dest node is not empty
      if (not dstNode.empty()) {
        std::vector<int32_t> pushLabels;

        // Add destination prepend label if any.
        auto& dstPrefixEntry = prefixEntries.at({dstNode, area});
        if (dstPrefixEntry->prependLabel_ref()) {
          pushLabels.emplace_back(dstPrefixEntry->prependLabel_ref().value());
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

      nextHops.emplace(createNextHop(
          isV4 and not v4OverV6Nexthop ? link->getNhV4FromNode(myNodeName)
                                       : link->getNhV6FromNode(myNodeName),
          link->getIfaceFromNode(myNodeName),
          distOverLink,
          mplsAction,
          link->getArea(),
          link->getOtherNodeName(myNodeName)));
    } // end for perDestination ...
  } // end for linkState ...
  return nextHops;
}

size_t
SpfSolver::getNumSrPolicies() const {
  return srPolicies_.size();
}

thrift::RouteComputationRules
SpfSolver::getRouteComputationRules(
    const PrefixEntries& prefixEntries,
    const RouteSelectionResult& routeSelectionResult,
    const std::unordered_map<std::string, LinkState>& areaLinkStates) const {
  // Walk the srPolicies_ list and return the rules of the first one that
  // matches the route attributes
  for (const auto& srPolicy : srPolicies_) {
    auto rules = srPolicy.matchAndGetRules(
        prefixEntries.at(routeSelectionResult.bestNodeArea));
    if (rules) {
      return *rules;
    }
  }

  // No SR Policy has matched. Construct default route computation rules.
  //
  // 1. Best route selection = SHORTEST_DISTANCE
  // 2. forwarding algorithm and forwarding type based on PrefixEntry
  //    attributes
  // 3. Prepend label = None
  thrift::RouteComputationRules defaultRules;
  defaultRules.routeSelectionAlgo_ref() =
      thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE;
  for (const auto& [areaId, _] : areaLinkStates) {
    const auto areaPathComputationRules = getPrefixForwardingTypeAndAlgorithm(
        areaId, prefixEntries, routeSelectionResult.allNodeAreas);
    if (not areaPathComputationRules) {
      // There are no best routes in this area
      continue;
    }

    thrift::AreaPathComputationRules areaRules;
    areaRules.forwardingType_ref() = areaPathComputationRules->first;
    areaRules.forwardingAlgo_ref() = areaPathComputationRules->second;
    defaultRules.areaPathComputationRules_ref()->emplace(
        areaId, std::move(areaRules));
  }

  return defaultRules;
}

} // namespace openr
