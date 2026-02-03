/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/String.h>
#include <openr/common/Constants.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/MplsUtil.h>

namespace openr {

thrift::BuildInfo
getBuildInfoThrift() noexcept {
  thrift::BuildInfo buildInfo;
  buildInfo.buildUser() = BuildInfo::getBuildUser();
  buildInfo.buildTime() = BuildInfo::getBuildTime();
  buildInfo.buildTimeUnix() =
      static_cast<int64_t>(BuildInfo::getBuildTimeUnix());
  buildInfo.buildHost() = BuildInfo::getBuildHost();
  buildInfo.buildPath() = BuildInfo::getBuildPath();
  buildInfo.buildRevision() = BuildInfo::getBuildRevision();
  buildInfo.buildRevisionCommitTimeUnix() =
      static_cast<int64_t>(BuildInfo::getBuildRevisionCommitTimeUnix());
  buildInfo.buildUpstreamRevision() = BuildInfo::getBuildUpstreamRevision();
  buildInfo.buildUpstreamRevisionCommitTimeUnix() =
      BuildInfo::getBuildUpstreamRevisionCommitTimeUnix();
  buildInfo.buildPackageName() = BuildInfo::getBuildPackageName();
  buildInfo.buildPackageVersion() = BuildInfo::getBuildPackageVersion();
  buildInfo.buildPackageRelease() = BuildInfo::getBuildPackageRelease();
  buildInfo.buildPlatform() = BuildInfo::getBuildPlatform();
  buildInfo.buildRule() = BuildInfo::getBuildRule();
  buildInfo.buildType() = BuildInfo::getBuildType();
  buildInfo.buildTool() = BuildInfo::getBuildTool();
  buildInfo.buildMode() = BuildInfo::getBuildMode();
  return buildInfo;
}

// construct thrift::PerfEvent
thrift::PerfEvent
createPerfEvent(std::string nodeName, std::string eventDescr, int64_t unixTs) {
  thrift::PerfEvent perfEvent;
  perfEvent.nodeName() = nodeName;
  perfEvent.eventDescr() = eventDescr;
  perfEvent.unixTs() = unixTs;
  return perfEvent;
}

void
addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept {
  thrift::PerfEvent event =
      createPerfEvent(nodeName, eventDescr, getUnixTimeStampMs());
  perfEvents.events()->emplace_back(std::move(event));
}

std::vector<std::string>
sprintPerfEvents(const thrift::PerfEvents& perfEvents) noexcept {
  const auto& events = *perfEvents.events();
  if (events.empty()) {
    return {};
  }

  std::vector<std::string> eventStrs;
  auto recentTs = *events.front().unixTs();
  for (auto const& event : events) {
    auto durationMs = *event.unixTs() - recentTs;
    recentTs = *event.unixTs();
    eventStrs.emplace_back(
        fmt::format(
            "node: {}, event: {}, duration: {}ms, unix-timestamp: {}",
            *event.nodeName(),
            *event.eventDescr(),
            durationMs,
            *event.unixTs()));
  }
  return eventStrs;
}

std::chrono::milliseconds
getTotalPerfEventsDuration(const thrift::PerfEvents& perfEvents) noexcept {
  if (perfEvents.events()->empty()) {
    return std::chrono::milliseconds(0);
  }

  auto recentTs = *perfEvents.events()->front().unixTs();
  auto latestTs = *perfEvents.events()->back().unixTs();
  return std::chrono::milliseconds(latestTs - recentTs);
}

folly::Expected<std::chrono::milliseconds, std::string>
getDurationBetweenPerfEvents(
    const thrift::PerfEvents& perfEvents,
    const std::string& firstName,
    const std::string& secondName) noexcept {
  auto search = std::find_if(
      perfEvents.events()->cbegin(),
      perfEvents.events()->cend(),
      [&firstName](const thrift::PerfEvent& event) {
        return *event.eventDescr() == firstName;
      });
  if (search == perfEvents.events()->cend()) {
    return folly::makeUnexpected(
        fmt::format("Could not find first event: {}", firstName));
  }
  int64_t first = *search->unixTs();
  search = std::find_if(
      search + 1,
      perfEvents.events()->cend(),
      [&secondName](const thrift::PerfEvent& event) {
        return *event.eventDescr() == secondName;
      });
  if (search == perfEvents.events()->cend()) {
    return folly::makeUnexpected(
        fmt::format("Could not find second event: {}", secondName));
  }
  int64_t second = *search->unixTs();
  if (second < first) {
    return folly::makeUnexpected(
        std::string{"Negative duration between first and second event"});
  }
  return std::chrono::milliseconds(second - first);
}

std::string
toString(thrift::PrefixForwardingType const& value) {
  return apache::thrift::TEnumTraits<thrift::PrefixForwardingType>::findName(
      value);
}

std::string
toString(thrift::PrefixForwardingAlgorithm const& value) {
  return apache::thrift::TEnumTraits<
      thrift::PrefixForwardingAlgorithm>::findName(value);
}

std::string
toString(thrift::PrefixType const& value) {
  return apache::thrift::TEnumTraits<thrift::PrefixType>::findName(value);
}

std::string
toString(thrift::PrefixMetrics const& metrics) {
  return fmt::format(
      "Metrics: [Drain={}, SP={}, PP={}, D={}]",
      *metrics.drain_metric(),
      *metrics.source_preference(),
      *metrics.path_preference(),
      *metrics.distance());
}

std::string
toString(thrift::PrefixEntry const& entry, bool detailed) {
  std::stringstream ss;
  ss << fmt::format(
      FMT_STRING("[{}], Forwarding: [{}, {}], {}, Type: {}"),
      toString(*entry.prefix()),
      toString(*entry.forwardingType()),
      toString(*entry.forwardingAlgorithm()),
      toString(*entry.metrics()),
      toString(*entry.type()));
  if (entry.minNexthop()) {
    ss << ", NM: " << *entry.minNexthop();
  }
  if (entry.weight()) {
    ss << ", W: " << *entry.weight();
  }
  if (detailed) {
    ss << ", Tags: [" << folly::join(", ", *entry.tags()) << "]";
    ss << ", AreaStack: [" << folly::join(", ", *entry.area_stack()) << "]";
  }
  return ss.str();
}

std::string
toString(NeighborEventType const& type) {
  switch (type) {
  case NeighborEventType::NEIGHBOR_UP:
    return "NEIGHBOR_UP";
  case NeighborEventType::NEIGHBOR_DOWN:
    return "NEIGHBOR_DOWN";
  case NeighborEventType::NEIGHBOR_RESTARTED:
    return "NEIGHBOR_RESTARTED";
  case NeighborEventType::NEIGHBOR_RTT_CHANGE:
    return "NEIGHBOR_RTT_CHANGE";
  case NeighborEventType::NEIGHBOR_RESTARTING:
    return "NEIGHBOR_RESTARTING";
  case NeighborEventType::NEIGHBOR_ADJ_SYNCED:
    return "NEIGHBOR_ADJ_SYNCED";
  default:
    return "UNKNOWN";
  }
}

// bit position starts from 0
uint32_t
bitStrValue(const folly::IPAddress& ip, uint32_t start, uint32_t end) {
  uint32_t index{0};
  CHECK_GE(start, 0);
  CHECK_LE(start, end);
  // 0 based index
  for (uint32_t i = start; i <= end; i++) {
    index <<= 1;
    index |= ip.getNthMSBit(i);
  }
  return index;
}

folly::CIDRNetwork
getNthPrefix(
    const folly::CIDRNetwork& seedPrefix,
    uint32_t allocPrefixLen,
    uint32_t prefixIndex) {
  // get underlying byte array representing IP
  const uint32_t bitCount = seedPrefix.first.bitCount();
  auto ipBytes = std::string(
      reinterpret_cast<const char*>(seedPrefix.first.bytes()),
      seedPrefix.first.byteCount());

  // host number bit length
  // in seed prefix
  const uint32_t seedHostBitLen = bitCount - seedPrefix.second;
  // in allocated prefix
  const uint32_t allocHostBitLen = bitCount - allocPrefixLen;

  // sanity check
  const int32_t allocBits =
      std::min(32, static_cast<int32_t>(seedHostBitLen - allocHostBitLen));
  if (allocBits < 0) {
    throw std::invalid_argument("Alloc prefix is bigger than seed prefix.");
  }
  if (allocBits < 32 && prefixIndex >= (1u << allocBits)) {
    throw std::invalid_argument("Prefix index is out of range.");
  }

  // using bits (seedHostBitLen-allocHostBitLen-1)..0 of @prefixIndex to
  // set bits (seedHostBitLen - 1)..allocHostBitLen of ipBytes
  for (uint8_t i = 0; i < allocBits; ++i) {
    // global bit index across bytes
    auto idx = i + allocHostBitLen;
    // byte index: network byte order, i.e., big-endian
    auto byteIdx = bitCount / 8 - idx / 8 - 1;
    // bit index inside the byte
    auto bitIdx = idx % 8;
    if (prefixIndex & (0x1 << i)) {
      // set
      ipBytes.at(byteIdx) |= (0x1 << bitIdx);
    } else {
      // clear
      ipBytes.at(byteIdx) &= ~(0x1 << bitIdx);
    }
  }

  // convert back to CIDR
  auto allocPrefixIp = folly::IPAddress::fromBinary(
      folly::ByteRange(
          reinterpret_cast<const uint8_t*>(ipBytes.data()), ipBytes.size()));
  return {allocPrefixIp.mask(allocPrefixLen), allocPrefixLen};
}

folly::IPAddress
createLoopbackAddr(const folly::CIDRNetwork& prefix) noexcept {
  auto addr = prefix.first.mask(prefix.second);

  // Set last bit to `1` if prefix length is not full
  if (prefix.second != prefix.first.bitCount()) {
    auto bytes = std::string(
        reinterpret_cast<const char*>(addr.bytes()), addr.byteCount());
    bytes[bytes.size() - 1] |= 0x01; // Set last bit to 1
    addr = folly::IPAddress::fromBinary(
        folly::ByteRange(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  }

  return addr;
}

folly::CIDRNetwork
createLoopbackPrefix(const folly::CIDRNetwork& prefix) noexcept {
  auto addr = createLoopbackAddr(prefix);
  return folly::CIDRNetwork{addr, prefix.first.bitCount()};
}

std::string
getRemoteIfName(const thrift::Adjacency& adj) {
  if (!adj.otherIfName()->empty()) {
    return *adj.otherIfName();
  }
  return fmt::format("neigh-{}", *adj.ifName());
}

thrift::RouteDatabaseDelta
findDeltaRoutes(
    const thrift::RouteDatabase& newRouteDb,
    const thrift::RouteDatabase& oldRouteDb) {
  DCHECK(*newRouteDb.thisNodeName() == *oldRouteDb.thisNodeName());
  // verify the input is sorted.
  CHECK(
      std::is_sorted(
          newRouteDb.unicastRoutes()->begin(),
          newRouteDb.unicastRoutes()->end()) &&
      std::is_sorted(
          oldRouteDb.unicastRoutes()->begin(),
          oldRouteDb.unicastRoutes()->end()) &&
      std::is_sorted(
          newRouteDb.mplsRoutes()->begin(), newRouteDb.mplsRoutes()->end()) &&
      std::is_sorted(
          oldRouteDb.mplsRoutes()->begin(), oldRouteDb.mplsRoutes()->end()));

  // Find unicast routes to be added/updated or removed
  std::vector<thrift::UnicastRoute> unicastRoutesToUpdate;
  std::set_difference(
      newRouteDb.unicastRoutes()->begin(),
      newRouteDb.unicastRoutes()->end(),
      oldRouteDb.unicastRoutes()->begin(),
      oldRouteDb.unicastRoutes()->end(),
      std::inserter(unicastRoutesToUpdate, unicastRoutesToUpdate.begin()));
  std::vector<thrift::UnicastRoute> unicastRoutesToDelete;
  std::set_difference(
      oldRouteDb.unicastRoutes()->begin(),
      oldRouteDb.unicastRoutes()->end(),
      newRouteDb.unicastRoutes()->begin(),
      newRouteDb.unicastRoutes()->end(),
      std::inserter(unicastRoutesToDelete, unicastRoutesToDelete.begin()));

  // Find mpls routes to be added/updated or removed
  std::vector<thrift::MplsRoute> mplsRoutesToUpdate;
  std::set_difference(
      newRouteDb.mplsRoutes()->begin(),
      newRouteDb.mplsRoutes()->end(),
      oldRouteDb.mplsRoutes()->begin(),
      oldRouteDb.mplsRoutes()->end(),
      std::inserter(mplsRoutesToUpdate, mplsRoutesToUpdate.begin()));
  std::vector<thrift::MplsRoute> mplsRoutesToDelete;
  std::set_difference(
      oldRouteDb.mplsRoutes()->begin(),
      oldRouteDb.mplsRoutes()->end(),
      newRouteDb.mplsRoutes()->begin(),
      newRouteDb.mplsRoutes()->end(),
      std::inserter(mplsRoutesToDelete, mplsRoutesToDelete.begin()));

  // Find entry of prefix to be removed
  std::set<thrift::IpPrefix> prefixesToRemove;
  for (const auto& route : unicastRoutesToDelete) {
    prefixesToRemove.emplace(*route.dest());
  }
  for (const auto& route : unicastRoutesToUpdate) {
    prefixesToRemove.erase(*route.dest());
  }

  // Find labels to be removed
  std::set<int32_t> labelsToRemove;
  for (const auto& route : mplsRoutesToDelete) {
    labelsToRemove.emplace(*route.topLabel());
  }
  for (const auto& route : mplsRoutesToUpdate) {
    labelsToRemove.erase(*route.topLabel());
  }

  // Build routes to be programmed.
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.unicastRoutesToUpdate() = std::move(unicastRoutesToUpdate);
  routeDbDelta.unicastRoutesToDelete() = {
      prefixesToRemove.begin(), prefixesToRemove.end()};
  routeDbDelta.mplsRoutesToUpdate() = std::move(mplsRoutesToUpdate);
  routeDbDelta.mplsRoutesToDelete() = {
      labelsToRemove.begin(), labelsToRemove.end()};

  return routeDbDelta;
}

bool
hasBestRoutesInArea(
    const std::string& area,
    const PrefixEntries& prefixEntries,
    const std::set<NodeAndArea>& bestNodeAreas) {
  for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
    if (!bestNodeAreas.count(nodeAndArea)) {
      continue; // Skip the prefix-entry of non best node-area
    }
    if (area != nodeAndArea.second) {
      continue; // Skip best routes in different areas
    }

    return true;
  }
  return false;
}

thrift::PeerSpec
createPeerSpec(
    const std::string& peerAddr,
    const int32_t port,
    const thrift::KvStorePeerState state) {
  thrift::PeerSpec peerSpec;
  peerSpec.peerAddr() = peerAddr;
  peerSpec.ctrlPort() = port;
  peerSpec.state() = state;
  return peerSpec;
}

thrift::Adjacency
createThriftAdjacency(
    const std::string& nodeName,
    const std::string& ifName,
    const std::string& nextHopV6,
    const std::string& nextHopV4,
    int32_t metric,
    int32_t adjLabel,
    bool isOverloaded,
    int32_t rtt,
    int64_t timestamp,
    int64_t weight,
    const std::string& remoteIfName,
    bool adjOnlyUsedByOtherNode) {
  thrift::Adjacency adj;
  adj.otherNodeName() = nodeName;
  adj.ifName() = ifName;
  adj.nextHopV6() = toBinaryAddress(folly::IPAddress(nextHopV6));
  adj.nextHopV4() = toBinaryAddress(folly::IPAddress(nextHopV4));
  adj.metric() = metric;
  adj.adjLabel() = adjLabel;
  adj.isOverloaded() = isOverloaded;
  adj.rtt() = rtt;
  adj.timestamp() = timestamp;
  adj.weight() = weight;
  adj.otherIfName() = remoteIfName;
  adj.adjOnlyUsedByOtherNode() = adjOnlyUsedByOtherNode;
  return adj;
}

thrift::Adjacency
createAdjacency(
    const std::string& nodeName,
    const std::string& ifName,
    const std::string& remoteIfName,
    const std::string& nextHopV6,
    const std::string& nextHopV4,
    int32_t metric,
    int32_t adjLabel,
    int64_t weight,
    bool adjOnlyUsedByOtherNode) {
  return createThriftAdjacency(
      nodeName,
      ifName,
      nextHopV6,
      nextHopV4,
      metric,
      adjLabel,
      false,
      metric * 100,
      getUnixTimeStampMs() / 1000,
      weight,
      remoteIfName,
      adjOnlyUsedByOtherNode);
}

thrift::AdjacencyDatabase
createAdjDb(
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    int32_t nodeLabel,
    bool overLoadBit,
    const std::string& area,
    int nodeMetricIncrementVal) {
  thrift::AdjacencyDatabase adjDb;
  adjDb.thisNodeName() = nodeName;
  adjDb.isOverloaded() = overLoadBit;
  adjDb.adjacencies() = adjs;
  adjDb.nodeLabel() = nodeLabel;
  adjDb.area() = area;
  adjDb.nodeMetricIncrementVal() = nodeMetricIncrementVal;
  return adjDb;
}

thrift::PrefixDatabase
createPrefixDb(
    const std::string& nodeName,
    const std::vector<thrift::PrefixEntry>& prefixEntries,
    bool withdraw) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName() = nodeName;
  prefixDb.prefixEntries() = prefixEntries;
  prefixDb.deletePrefix() = withdraw;
  return prefixDb;
}

// TODO: Create and return thrift::PrefixEntry as a
// shared_ptr after adding support in BGPRIB/FIB/UT
thrift::PrefixEntry
createPrefixEntry(
    thrift::IpPrefix prefix,
    thrift::PrefixType type,
    const std::string& data,
    thrift::PrefixForwardingType forwardingType,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    std::optional<int64_t> minNexthop,
    std::optional<int64_t> weight) {
  thrift::PrefixEntry prefixEntry;
  prefixEntry.prefix() = prefix;
  prefixEntry.type() = type;
  prefixEntry.forwardingType() = forwardingType;
  prefixEntry.forwardingAlgorithm() = forwardingAlgorithm;
  prefixEntry.minNexthop().from_optional(minNexthop);
  prefixEntry.weight().from_optional(weight);
  return prefixEntry;
}

// Currently used by DecisionTest and PrefixManagerTest
thrift::PrefixMetrics
createMetrics(int32_t pp, int32_t sp, int32_t d) {
  thrift::PrefixMetrics metrics;
  metrics.path_preference() = pp;
  metrics.source_preference() = sp;
  metrics.distance() = d;
  return metrics;
}

thrift::PrefixEntry
createPrefixEntryWithMetrics(
    thrift::IpPrefix const& prefix,
    thrift::PrefixType const& type,
    thrift::PrefixMetrics const& metrics) {
  auto prefixEntry = createPrefixEntry(prefix, type);
  prefixEntry.metrics() = metrics;
  return prefixEntry;
}

// construct thrift::OpenrVersions
thrift::OpenrVersions
createOpenrVersions(
    const thrift::OpenrVersion& version,
    const thrift::OpenrVersion& lowestSupportedVersion) {
  thrift::OpenrVersions openrVersions;
  openrVersions.version() = version;
  openrVersions.lowestSupportedVersion() = lowestSupportedVersion;
  return openrVersions;
}

std::pair<PrefixKey, std::shared_ptr<thrift::PrefixEntry>>
createPrefixKeyAndEntry(
    const std::string& nodeName,
    thrift::IpPrefix const& prefix,
    const std::string& area) {
  return {
      PrefixKey(nodeName, toIPNetwork(prefix), area),
      std::make_shared<thrift::PrefixEntry>(createPrefixEntry(prefix))};
}

std::pair<PrefixKey, thrift::PrefixDatabase>
createPrefixKeyAndDb(
    const std::string& nodeName,
    const thrift::PrefixEntry& prefixEntry,
    const std::string& area,
    bool withdraw) {
  return {
      PrefixKey(nodeName, toIPNetwork(*prefixEntry.prefix()), area),
      createPrefixDb(nodeName, {prefixEntry}, withdraw)};
}

std::pair<std::string, thrift::Value>
createPrefixKeyValue(
    const std::string& nodeName,
    const int64_t version,
    const thrift::PrefixEntry& prefixEntry,
    const std::string& area,
    bool withdraw) {
  apache::thrift::CompactSerializer serializer;
  auto [key, db] = createPrefixKeyAndDb(nodeName, prefixEntry, area, withdraw);
  return {
      key.getPrefixKeyV2(),
      createThriftValue(
          version, nodeName, writeThriftObjStr(std::move(db), serializer))};
}

std::pair<std::string, thrift::Value>
createPrefixKeyValue(
    const std::string& nodeName,
    const int64_t version,
    thrift::IpPrefix const& prefix,
    const std::string& area,
    bool withdraw) {
  return createPrefixKeyValue(
      nodeName, version, createPrefixEntry(prefix), area, withdraw);
}

thrift::OriginatedPrefixEntry
createOriginatedPrefixEntry(
    const thrift::OriginatedPrefix& originatedPrefix,
    const std::vector<std::string>& supportingPrefixes,
    bool installed) {
  thrift::OriginatedPrefixEntry entry;
  entry.prefix() = originatedPrefix;
  entry.supporting_prefixes() = supportingPrefixes;
  entry.installed() = installed;
  return entry;
}

thrift::NextHopThrift
createNextHop(
    thrift::BinaryAddress addr,
    std::optional<std::string> ifName,
    int32_t metric,
    std::optional<thrift::MplsAction> maybeMplsAction,
    const std::optional<std::string>& area,
    const std::optional<std::string>& neighborNodeName,
    int64_t weight) {
  thrift::NextHopThrift nextHop;
  nextHop.address() = addr;
  nextHop.address()->ifName().from_optional(std::move(ifName));
  nextHop.metric() = metric;
  nextHop.mplsAction().from_optional(maybeMplsAction);
  nextHop.area().from_optional(area);
  nextHop.neighborNodeName().from_optional(neighborNodeName);
  nextHop.weight() = weight;
  return nextHop;
}

thrift::MplsAction
createMplsAction(
    thrift::MplsActionCode const mplsActionCode,
    std::optional<int32_t> maybeSwapLabel,
    std::optional<std::vector<int32_t>> maybePushLabels) {
  thrift::MplsAction mplsAction;
  mplsAction.action() = mplsActionCode;
  mplsAction.swapLabel().from_optional(maybeSwapLabel);
  mplsAction.pushLabels().from_optional(maybePushLabels);
  checkMplsAction(mplsAction); // sanity checks
  return mplsAction;
}

thrift::UnicastRoute
createUnicastRoute(
    thrift::IpPrefix dest, std::vector<thrift::NextHopThrift> nextHops) {
  thrift::UnicastRoute unicastRoute;
  unicastRoute.dest() = std::move(dest);
  std::sort(nextHops.begin(), nextHops.end());
  unicastRoute.nextHops() = std::move(nextHops);
  return unicastRoute;
}

thrift::UnicastRouteDetail
createUnicastRouteDetail(
    thrift::IpPrefix dest,
    std::vector<thrift::NextHopThrift> nextHops,
    std::optional<thrift::PrefixEntry> maybeBestRoute) {
  thrift::UnicastRouteDetail unicastRouteDetail;
  unicastRouteDetail.unicastRoute() = createUnicastRoute(dest, nextHops);
  unicastRouteDetail.bestRoute().from_optional(maybeBestRoute);
  return unicastRouteDetail;
}

thrift::MplsRoute
createMplsRoute(int32_t topLabel, std::vector<thrift::NextHopThrift> nextHops) {
  // Sanity checks
  CHECK(isMplsLabelValid(topLabel));
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.mplsAction().has_value());
  }

  thrift::MplsRoute mplsRoute;
  mplsRoute.topLabel() = topLabel;
  std::sort(nextHops.begin(), nextHops.end());
  mplsRoute.nextHops() = std::move(nextHops);
  return mplsRoute;
}

std::vector<thrift::UnicastRoute>
createUnicastRoutesFromMap(
    const std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>&
        unicastRoutes) {
  std::vector<thrift::UnicastRoute> newRoutes;
  for (auto const& [_, route] : unicastRoutes) {
    newRoutes.emplace_back(route.toThrift());
  }
  return newRoutes;
}

std::vector<thrift::MplsRoute>
createMplsRoutesFromMap(
    const std::unordered_map<int32_t, RibMplsEntry>& mplsRoutes) {
  std::vector<thrift::MplsRoute> newRoutes;
  for (auto const& [_, route] : mplsRoutes) {
    newRoutes.emplace_back(route.toThrift());
  }
  return newRoutes;
}

std::string
getNodeNameFromKey(const std::string& key) {
  std::vector<std::string> split;
  folly::split(Constants::kPrefixNameSeparator.toString(), key, split);
  if (split.size() < 2) {
    return "";
  }
  return split[1];
}

NodeAndArea
selectBestNodeArea(
    std::set<NodeAndArea> const& allNodeAreas, std::string const& myNodeName) {
  NodeAndArea bestNodeArea = *allNodeAreas.begin();
  for (const auto& nodeAndArea : allNodeAreas) {
    if (nodeAndArea.first == myNodeName) {
      bestNodeArea = nodeAndArea;
      break;
    }
  }
  return bestNodeArea;
}

namespace {

std::set<NodeAndArea>
selectShortestDistance(
    const PrefixEntries& prefixEntries,
    const std::set<NodeAndArea>& nodeAreaSet) {
  std::set<NodeAndArea> ret;
  int32_t shortestDist = std::numeric_limits<int32_t>::max();
  for (const auto& nodeArea : nodeAreaSet) {
    auto it = prefixEntries.find(nodeArea);
    if (it == prefixEntries.end()) {
      continue;
    }
    int32_t dist = *it->second->metrics()->distance();
    if (dist > shortestDist) {
      continue;
    }
    if (dist < shortestDist) {
      shortestDist = dist;
      ret.clear();
    }
    ret.emplace(nodeArea);
  }
  return ret;
}

std::set<NodeAndArea>
selectShortestDistancePerArea(
    const PrefixEntries& prefixEntries,
    const std::set<NodeAndArea>& nodeAreaSet) {
  // Split nodeAreaSet based on area.
  std::unordered_map<std::string /*area*/, std::set<NodeAndArea>> areaMap;
  for (const auto& nodeArea : nodeAreaSet) {
    areaMap[nodeArea.second].emplace(nodeArea);
  }
  std::set<NodeAndArea> ret;
  // Get shortest distance result in each area.
  for (const auto& [_, setInArea] : areaMap) {
    auto shortestRet = selectShortestDistance(prefixEntries, setInArea);
    for (const auto& nodeArea : shortestRet) {
      ret.insert(nodeArea);
    }
  }
  return ret;
}

} // namespace

std::set<NodeAndArea>
selectRoutes(
    const PrefixEntries& prefixEntries,
    thrift::RouteSelectionAlgorithm algorithm,
    const folly::F14FastSet<NodeAndArea>& drainedNodes) {
  /*
   * [Best Route Selection] Part 1/2:
   *
   *  - 1st tie-breaker : drained_state - prefer not drained;
   *     a node is drained if it has any drain state;
   *       - metrics.drain_metric is positive (drained path redistributed
   *          remotely)
   *       - drainedNodes.count(key) is positive (node is drained (in the same
   *          area))
   *     Set to -1 if drained, otherwise 0.
   *  - 2nd tie-breaker: path_preference - prefer higher;
   *  - 3rd tie-breaker: source_preference - prefer higher;
   */
  std::tuple<int32_t, int32_t, int32_t> bestMetricsTuple{
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::min()};
  std::set<NodeAndArea> nodeAreaSet;
  for (auto& [key, metricsWrapper] : prefixEntries) {
    auto& metrics = *metricsWrapper->metrics();
    std::tuple<int32_t, int32_t, int32_t> metricsTuple{
        -(*metrics.drain_metric() ||
          (int32_t)drainedNodes.count(key)), /* prefer-lower, set to -1 if
                                                drained, otherwise 0*/
        *metrics.path_preference(), /* prefer-higher */
        *metrics.source_preference() /* prefer-higher */};

    if (metricsTuple < bestMetricsTuple) {
      continue;
    }
    if (metricsTuple > bestMetricsTuple) {
      // Clear set and update best metric if this is a new best metric
      bestMetricsTuple = metricsTuple;
      nodeAreaSet.clear();
    }
    nodeAreaSet.emplace(key);
  }

  /*
   * [Best Route Selection] Part 2/2:
   *
   * With different selection algorithm, the preference over distance is
   * different.
   *
   * Please see `openr/if/OpenrConfig.thrift` for detailed reference.
   */
  switch (algorithm) {
  case thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE:
    return selectShortestDistance(prefixEntries, nodeAreaSet);
  case thrift::RouteSelectionAlgorithm::PER_AREA_SHORTEST_DISTANCE:
    return selectShortestDistancePerArea(prefixEntries, nodeAreaSet);
  default:
    XLOG(INFO) << "Unsupported route selection algorithm "
               << apache::thrift::util::enumNameSafe(algorithm);
    break;
  }

  return std::set<NodeAndArea>();
}

} // namespace openr
