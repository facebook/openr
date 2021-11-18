/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/Util.h>

#include <fb303/ServiceData.h>
#include <fmt/core.h>
#include <folly/logging/xlog.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#if __has_include("filesystem")
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

#include <openr/common/Constants.h>

namespace openr {

std::string
toString(thrift::PrefixType const& value) {
  return apache::thrift::TEnumTraits<thrift::PrefixType>::findName(value);
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
toString(thrift::PrefixMetrics const& metrics) {
  return fmt::format(
      "Metrics: [SP={}, PP={}, D={}]",
      *metrics.source_preference_ref(),
      *metrics.path_preference_ref(),
      *metrics.distance_ref());
}

std::string
toString(thrift::PrefixEntry const& entry, bool detailed) {
  std::stringstream ss;
  ss << fmt::format(
      FMT_STRING("[{}], Forwarding: [{}, {}], {}, Type: {}"),
      toString(*entry.prefix_ref()),
      toString(*entry.forwardingType_ref()),
      toString(*entry.forwardingAlgorithm_ref()),
      toString(*entry.metrics_ref()),
      toString(*entry.type_ref()));
  if (entry.minNexthop_ref()) {
    ss << ", NM: " << *entry.minNexthop_ref();
  }
  if (entry.prependLabel_ref()) {
    ss << ", PL: " << *entry.prependLabel_ref();
  }
  if (detailed) {
    ss << ", Tags: [" << folly::join(", ", *entry.tags_ref()) << "]";
    ss << ", AreaStack: [" << folly::join(", ", *entry.area_stack_ref()) << "]";
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

void
logInitializationEvent(
    const std::string& publisher,
    const thrift::InitializationEvent event,
    const std::optional<std::string>& message) {
  // OpenR start time. Initialized first time function is called.
  const static auto kOpenrStartTime = std::chrono::steady_clock::now();
  // Duration in milliseconds since OpenR start.
  auto durationSinceStart =
      std::chrono::ceil<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - kOpenrStartTime)
          .count();
  auto durationStr = durationSinceStart >= 1000
      ? fmt::format("{}s", durationSinceStart * 1.0 / 1000)
      : fmt::format("{}ms", durationSinceStart);

  auto logMsg = fmt::format(
      "[Initialization] event: {}, publisher: {}, durationSinceStart: {}",
      apache::thrift::util::enumNameSafe(event),
      publisher,
      durationStr);
  if (message.has_value()) {
    logMsg = logMsg + fmt::format(", message: {}", message.value());
  }
  XLOG(INFO) << logMsg;

  // Log OpenR initialization event to fb303::fbData.
  facebook::fb303::fbData->setCounter(
      fmt::format(
          Constants::kInitEventCounterFormat.toString(),
          apache::thrift::util::enumNameSafe(event)),
      durationSinceStart);
}

void
setupThriftServerTls(
    apache::thrift::ThriftServer& thriftServer,
    apache::thrift::SSLPolicy sslPolicy,
    std::string const& ticketSeedPath,
    std::shared_ptr<wangle::SSLContextConfig> sslContext) {
  thriftServer.setSSLPolicy(sslPolicy);
  // Allow non-secure clients on localhost (e.g. breeze / fbmeshd)
  thriftServer.setAllowPlaintextOnLoopback(true);
  thriftServer.setSSLConfig(sslContext);
  if (fs::exists(ticketSeedPath)) {
    thriftServer.watchTicketPathForChanges(ticketSeedPath);
  }
  return;
}

folly::IPAddress
createLoopbackAddr(const folly::CIDRNetwork& prefix) noexcept {
  auto addr = prefix.first.mask(prefix.second);

  // Set last bit to `1` if prefix length is not full
  if (prefix.second != prefix.first.bitCount()) {
    auto bytes = std::string(
        reinterpret_cast<const char*>(addr.bytes()), addr.byteCount());
    bytes[bytes.size() - 1] |= 0x01; // Set last bit to 1
    addr = folly::IPAddress::fromBinary(folly::ByteRange(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  }

  return addr;
}

folly::CIDRNetwork
createLoopbackPrefix(const folly::CIDRNetwork& prefix) noexcept {
  auto addr = createLoopbackAddr(prefix);
  return folly::CIDRNetwork{addr, prefix.first.bitCount()};
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
  if (allocBits < 32 and prefixIndex >= (1u << allocBits)) {
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
  auto allocPrefixIp = folly::IPAddress::fromBinary(folly::ByteRange(
      reinterpret_cast<const uint8_t*>(ipBytes.data()), ipBytes.size()));
  return {allocPrefixIp.mask(allocPrefixLen), allocPrefixLen};
}

void
addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept {
  thrift::PerfEvent event =
      createPerfEvent(nodeName, eventDescr, getUnixTimeStampMs());
  perfEvents.events_ref()->emplace_back(std::move(event));
}

std::vector<std::string>
sprintPerfEvents(const thrift::PerfEvents& perfEvents) noexcept {
  const auto& events = *perfEvents.events_ref();
  if (events.empty()) {
    return {};
  }

  std::vector<std::string> eventStrs;
  auto recentTs = *events.front().unixTs_ref();
  for (auto const& event : events) {
    auto durationMs = *event.unixTs_ref() - recentTs;
    recentTs = *event.unixTs_ref();
    eventStrs.emplace_back(fmt::format(
        "node: {}, event: {}, duration: {}ms, unix-timestamp: {}",
        *event.nodeName_ref(),
        *event.eventDescr_ref(),
        durationMs,
        *event.unixTs_ref()));
  }
  return eventStrs;
}

std::chrono::milliseconds
getTotalPerfEventsDuration(const thrift::PerfEvents& perfEvents) noexcept {
  if (perfEvents.events_ref()->empty()) {
    return std::chrono::milliseconds(0);
  }

  auto recentTs = *perfEvents.events_ref()->front().unixTs_ref();
  auto latestTs = *perfEvents.events_ref()->back().unixTs_ref();
  return std::chrono::milliseconds(latestTs - recentTs);
}

folly::Expected<std::chrono::milliseconds, std::string>
getDurationBetweenPerfEvents(
    const thrift::PerfEvents& perfEvents,
    const std::string& firstName,
    const std::string& secondName) noexcept {
  auto search = std::find_if(
      perfEvents.events_ref()->cbegin(),
      perfEvents.events_ref()->cend(),
      [&firstName](const thrift::PerfEvent& event) {
        return *event.eventDescr_ref() == firstName;
      });
  if (search == perfEvents.events_ref()->cend()) {
    return folly::makeUnexpected(
        fmt::format("Could not find first event: {}", firstName));
  }
  int64_t first = *search->unixTs_ref();
  search = std::find_if(
      search + 1,
      perfEvents.events_ref()->cend(),
      [&secondName](const thrift::PerfEvent& event) {
        return *event.eventDescr_ref() == secondName;
      });
  if (search == perfEvents.events_ref()->cend()) {
    return folly::makeUnexpected(
        fmt::format("Could not find second event: {}", secondName));
  }
  int64_t second = *search->unixTs_ref();
  if (second < first) {
    return folly::makeUnexpected(
        std::string{"Negative duration between first and second event"});
  }
  return std::chrono::milliseconds(second - first);
}

template <class T>
int64_t
generateHashImpl(
    const int64_t version, const std::string& originatorId, const T& value) {
  size_t seed = 0;
  boost::hash_combine(seed, version);
  boost::hash_combine(seed, originatorId);
  if (value.has_value()) {
    boost::hash_combine(seed, value.value());
  }
  return static_cast<int64_t>(seed);
}

int64_t
generateHash(
    const int64_t version,
    const std::string& originatorId,
    const std::optional<std::string>& value) {
  return generateHashImpl(version, originatorId, value);
}

int64_t
generateHash(
    const int64_t version,
    const std::string& originatorId,
    const apache::thrift::optional_field_ref<const std::string&> value) {
  return generateHashImpl(version, originatorId, value);
}

std::string
getRemoteIfName(const thrift::Adjacency& adj) {
  if (not adj.otherIfName_ref()->empty()) {
    return *adj.otherIfName_ref();
  }
  return fmt::format("neigh-{}", *adj.ifName_ref());
}

thrift::RouteDatabaseDelta
findDeltaRoutes(
    const thrift::RouteDatabase& newRouteDb,
    const thrift::RouteDatabase& oldRouteDb) {
  DCHECK(*newRouteDb.thisNodeName_ref() == *oldRouteDb.thisNodeName_ref());
  // verify the input is sorted.
  CHECK(
      std::is_sorted(
          newRouteDb.unicastRoutes_ref()->begin(),
          newRouteDb.unicastRoutes_ref()->end()) &&
      std::is_sorted(
          oldRouteDb.unicastRoutes_ref()->begin(),
          oldRouteDb.unicastRoutes_ref()->end()) &&
      std::is_sorted(
          newRouteDb.mplsRoutes_ref()->begin(),
          newRouteDb.mplsRoutes_ref()->end()) &&
      std::is_sorted(
          oldRouteDb.mplsRoutes_ref()->begin(),
          oldRouteDb.mplsRoutes_ref()->end()));

  // Find unicast routes to be added/updated or removed
  std::vector<thrift::UnicastRoute> unicastRoutesToUpdate;
  std::set_difference(
      newRouteDb.unicastRoutes_ref()->begin(),
      newRouteDb.unicastRoutes_ref()->end(),
      oldRouteDb.unicastRoutes_ref()->begin(),
      oldRouteDb.unicastRoutes_ref()->end(),
      std::inserter(unicastRoutesToUpdate, unicastRoutesToUpdate.begin()));
  std::vector<thrift::UnicastRoute> unicastRoutesToDelete;
  std::set_difference(
      oldRouteDb.unicastRoutes_ref()->begin(),
      oldRouteDb.unicastRoutes_ref()->end(),
      newRouteDb.unicastRoutes_ref()->begin(),
      newRouteDb.unicastRoutes_ref()->end(),
      std::inserter(unicastRoutesToDelete, unicastRoutesToDelete.begin()));

  // Find mpls routes to be added/updated or removed
  std::vector<thrift::MplsRoute> mplsRoutesToUpdate;
  std::set_difference(
      newRouteDb.mplsRoutes_ref()->begin(),
      newRouteDb.mplsRoutes_ref()->end(),
      oldRouteDb.mplsRoutes_ref()->begin(),
      oldRouteDb.mplsRoutes_ref()->end(),
      std::inserter(mplsRoutesToUpdate, mplsRoutesToUpdate.begin()));
  std::vector<thrift::MplsRoute> mplsRoutesToDelete;
  std::set_difference(
      oldRouteDb.mplsRoutes_ref()->begin(),
      oldRouteDb.mplsRoutes_ref()->end(),
      newRouteDb.mplsRoutes_ref()->begin(),
      newRouteDb.mplsRoutes_ref()->end(),
      std::inserter(mplsRoutesToDelete, mplsRoutesToDelete.begin()));

  // Find entry of prefix to be removed
  std::set<thrift::IpPrefix> prefixesToRemove;
  for (const auto& route : unicastRoutesToDelete) {
    prefixesToRemove.emplace(*route.dest_ref());
  }
  for (const auto& route : unicastRoutesToUpdate) {
    prefixesToRemove.erase(*route.dest_ref());
  }

  // Find labels to be removed
  std::set<int32_t> labelsToRemove;
  for (const auto& route : mplsRoutesToDelete) {
    labelsToRemove.emplace(*route.topLabel_ref());
  }
  for (const auto& route : mplsRoutesToUpdate) {
    labelsToRemove.erase(*route.topLabel_ref());
  }

  // Build routes to be programmed.
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.unicastRoutesToUpdate_ref() = std::move(unicastRoutesToUpdate);
  routeDbDelta.unicastRoutesToDelete_ref() = {
      prefixesToRemove.begin(), prefixesToRemove.end()};
  routeDbDelta.mplsRoutesToUpdate_ref() = std::move(mplsRoutesToUpdate);
  routeDbDelta.mplsRoutesToDelete_ref() = {
      labelsToRemove.begin(), labelsToRemove.end()};

  return routeDbDelta;
}

thrift::BuildInfo
getBuildInfoThrift() noexcept {
  thrift::BuildInfo buildInfo;
  buildInfo.buildUser_ref() = BuildInfo::getBuildUser();
  buildInfo.buildTime_ref() = BuildInfo::getBuildTime();
  buildInfo.buildTimeUnix_ref() =
      static_cast<int64_t>(BuildInfo::getBuildTimeUnix());
  buildInfo.buildHost_ref() = BuildInfo::getBuildHost();
  buildInfo.buildPath_ref() = BuildInfo::getBuildPath();
  buildInfo.buildRevision_ref() = BuildInfo::getBuildRevision();
  buildInfo.buildRevisionCommitTimeUnix_ref() =
      static_cast<int64_t>(BuildInfo::getBuildRevisionCommitTimeUnix());
  buildInfo.buildUpstreamRevision_ref() = BuildInfo::getBuildUpstreamRevision();
  buildInfo.buildUpstreamRevisionCommitTimeUnix_ref() =
      BuildInfo::getBuildUpstreamRevisionCommitTimeUnix();
  buildInfo.buildPackageName_ref() = BuildInfo::getBuildPackageName();
  buildInfo.buildPackageVersion_ref() = BuildInfo::getBuildPackageVersion();
  buildInfo.buildPackageRelease_ref() = BuildInfo::getBuildPackageRelease();
  buildInfo.buildPlatform_ref() = BuildInfo::getBuildPlatform();
  buildInfo.buildRule_ref() = BuildInfo::getBuildRule();
  buildInfo.buildType_ref() = BuildInfo::getBuildType();
  buildInfo.buildTool_ref() = BuildInfo::getBuildTool();
  buildInfo.buildMode_ref() = BuildInfo::getBuildMode();
  return buildInfo;
}

std::optional<
    std::pair<thrift::PrefixForwardingType, thrift::PrefixForwardingAlgorithm>>
getPrefixForwardingTypeAndAlgorithm(
    const std::string& area,
    const PrefixEntries& prefixEntries,
    const std::set<NodeAndArea>& bestNodeAreas) {
  std::optional<std::pair<
      thrift::PrefixForwardingType,
      thrift::PrefixForwardingAlgorithm>>
      r = std::nullopt;

  for (auto const& [nodeAndArea, prefixEntry] : prefixEntries) {
    if (not bestNodeAreas.count(nodeAndArea)) {
      continue; // Skip the prefix-entry of non best node-area
    }
    if (area != nodeAndArea.second) {
      continue; // Skip best routes in different areas
    }

    if (!r) {
      r = {
          *prefixEntry->forwardingType_ref(),
          *prefixEntry->forwardingAlgorithm_ref()};
    } else {
      r->first = std::min(r->first, *prefixEntry->forwardingType_ref());
      r->second = std::min(r->second, *prefixEntry->forwardingAlgorithm_ref());
    }

    // Optimization case for most common algorithm and forwarding type
    if (r->first == thrift::PrefixForwardingType::IP &&
        r->second == thrift::PrefixForwardingAlgorithm::SP_ECMP) {
      return r;
    }
  }

  return r;
}

void
checkMplsAction(thrift::MplsAction const& mplsAction) {
  switch (*mplsAction.action_ref()) {
  case thrift::MplsActionCode::PUSH:
    // Swap label shouldn't be set
    CHECK(not mplsAction.swapLabel_ref().has_value());
    // Push labels should be set
    CHECK(mplsAction.pushLabels_ref().has_value());
    // there should be atleast one push label
    CHECK(not mplsAction.pushLabels_ref()->empty());
    for (auto const& label : mplsAction.pushLabels_ref().value()) {
      CHECK(isMplsLabelValid(label));
    }
    break;
  case thrift::MplsActionCode::SWAP:
    // Swap label should be set
    CHECK(mplsAction.swapLabel_ref().has_value());
    CHECK(isMplsLabelValid(mplsAction.swapLabel_ref().value()));
    // Push labels shouldn't be set
    CHECK(not mplsAction.pushLabels_ref().has_value());
    break;
  case thrift::MplsActionCode::PHP:
  case thrift::MplsActionCode::POP_AND_LOOKUP:
    // Swap label should not be set
    CHECK(not mplsAction.swapLabel_ref().has_value());
    CHECK(not mplsAction.pushLabels_ref().has_value());
    break;
  default:
    CHECK(false) << "Unknown action code";
  }
}

thrift::PeerSpec
createPeerSpec(
    const std::string& cmdUrl,
    const std::string& peerAddr,
    const int32_t port,
    const thrift::KvStorePeerState state,
    const bool supportFloodOptimization) {
  thrift::PeerSpec peerSpec;
  peerSpec.cmdUrl_ref() = cmdUrl;
  peerSpec.peerAddr_ref() = peerAddr;
  peerSpec.ctrlPort_ref() = port;
  peerSpec.state_ref() = state;
  peerSpec.supportFloodOptimization_ref() = supportFloodOptimization;
  return peerSpec;
}

thrift::SparkNeighbor
createSparkNeighbor(
    const std::string& nodeName,
    const thrift::BinaryAddress& v4Addr,
    const thrift::BinaryAddress& v6Addr,
    const int64_t kvStoreCmdPort,
    const int64_t openrCtrlThriftPort,
    const int32_t label,
    const int64_t rttUs,
    const std::string& remoteIfName,
    const std::string& localIfName,
    const std::string& area,
    const std::string& state) {
  thrift::SparkNeighbor neighbor;
  neighbor.nodeName_ref() = nodeName;
  neighbor.transportAddressV4_ref() = v4Addr;
  neighbor.transportAddressV6_ref() = v6Addr;
  neighbor.kvStoreCmdPort_ref() = kvStoreCmdPort;
  neighbor.openrCtrlThriftPort_ref() = openrCtrlThriftPort;
  neighbor.remoteIfName_ref() = remoteIfName;
  neighbor.localIfName_ref() = localIfName;
  neighbor.area_ref() = area;
  neighbor.state_ref() = state;
  neighbor.rttUs_ref() = rttUs;
  neighbor.label_ref() = label;
  return neighbor;
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
  adj.otherNodeName_ref() = nodeName;
  adj.ifName_ref() = ifName;
  adj.nextHopV6_ref() = toBinaryAddress(folly::IPAddress(nextHopV6));
  adj.nextHopV4_ref() = toBinaryAddress(folly::IPAddress(nextHopV4));
  adj.metric_ref() = metric;
  adj.adjLabel_ref() = adjLabel;
  adj.isOverloaded_ref() = isOverloaded;
  adj.rtt_ref() = rtt;
  adj.timestamp_ref() = timestamp;
  adj.weight_ref() = weight;
  adj.otherIfName_ref() = remoteIfName;
  adj.adjOnlyUsedByOtherNode_ref() = adjOnlyUsedByOtherNode;
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
    const std::string& area) {
  thrift::AdjacencyDatabase adjDb;
  adjDb.thisNodeName_ref() = nodeName;
  adjDb.isOverloaded_ref() = overLoadBit;
  adjDb.adjacencies_ref() = adjs;
  adjDb.nodeLabel_ref() = nodeLabel;
  adjDb.area_ref() = area;
  return adjDb;
}

thrift::PrefixDatabase
createPrefixDb(
    const std::string& nodeName,
    const std::vector<thrift::PrefixEntry>& prefixEntries,
    const std::string& area,
    bool withdraw) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName_ref() = nodeName;
  prefixDb.prefixEntries_ref() = prefixEntries;
  prefixDb.area_ref() = area;
  prefixDb.deletePrefix_ref() = withdraw;
  return prefixDb;
}

// TODO: refactor this as prefixEntry struct has been
// completely refactored.
// TODO: Create and return thrift::PrefixEntry as a
// shared_ptr after adding support in BGPRIB/FIB/UT
thrift::PrefixEntry
createPrefixEntry(
    thrift::IpPrefix prefix,
    thrift::PrefixType type,
    const std::string& data,
    thrift::PrefixForwardingType forwardingType,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    std::optional<thrift::MetricVector> mv,
    std::optional<int64_t> minNexthop) {
  thrift::PrefixEntry prefixEntry;
  prefixEntry.prefix_ref() = prefix;
  prefixEntry.type_ref() = type;
  if (not data.empty()) {
    prefixEntry.data_ref() = data;
  }
  prefixEntry.forwardingType_ref() = forwardingType;
  prefixEntry.forwardingAlgorithm_ref() = forwardingAlgorithm;
  prefixEntry.mv_ref().from_optional(mv);
  prefixEntry.minNexthop_ref().from_optional(minNexthop);
  return prefixEntry;
}

// TODO: Audit which util functions in this file are only used in unit test.
// Move functions only used in unit tests to be closer to unit tests, or create
// a util_test.h/cpp to hold those ones.
thrift::PrefixEntry
createPrefixEntryWithPrependLabel(
    thrift::IpPrefix prefix, std::optional<int32_t> prependLabel) {
  auto prefixEntry = createPrefixEntry(prefix, thrift::PrefixType::BGP);
  prefixEntry.prependLabel_ref().from_optional(prependLabel);
  return prefixEntry;
}

// Currently used by DecisionTest and PrefixManagerTest
thrift::PrefixMetrics
createMetrics(int32_t pp, int32_t sp, int32_t d) {
  thrift::PrefixMetrics metrics;
  metrics.path_preference_ref() = pp;
  metrics.source_preference_ref() = sp;
  metrics.distance_ref() = d;
  return metrics;
}

thrift::PrefixEntry
createPrefixEntryWithMetrics(
    thrift::IpPrefix const& prefix,
    thrift::PrefixType const& type,
    thrift::PrefixMetrics const& metrics) {
  auto prefixEntry = createPrefixEntry(prefix, type);
  prefixEntry.metrics_ref() = metrics;
  return prefixEntry;
}

// construct thrift::AllocPrefix
thrift::AllocPrefix
createAllocPrefix(
    const thrift::IpPrefix& seedPrefix,
    int64_t allocPrefixLen,
    int64_t allocPrefixIndex) {
  thrift::AllocPrefix allocPrefix;
  allocPrefix.seedPrefix_ref() = seedPrefix;
  allocPrefix.allocPrefixLen_ref() = allocPrefixLen;
  allocPrefix.allocPrefixIndex_ref() = allocPrefixIndex;
  return allocPrefix;
}

// construct thrift::PerfEvent
thrift::PerfEvent
createPerfEvent(std::string nodeName, std::string eventDescr, int64_t unixTs) {
  thrift::PerfEvent perfEvent;
  perfEvent.nodeName_ref() = nodeName;
  perfEvent.eventDescr_ref() = eventDescr;
  perfEvent.unixTs_ref() = unixTs;
  return perfEvent;
}

// construct thrift::KvstoreFloodRate
thrift::KvstoreFloodRate
createKvstoreFloodRate(
    int32_t flood_msg_per_sec, int32_t flood_msg_burst_size) {
  thrift::KvstoreFloodRate floodRate;
  floodRate.flood_msg_per_sec_ref() = flood_msg_per_sec;
  floodRate.flood_msg_burst_size_ref() = flood_msg_burst_size;
  return floodRate;
}

// construct thrift::OpenrVersions
thrift::OpenrVersions
createOpenrVersions(
    const thrift::OpenrVersion& version,
    const thrift::OpenrVersion& lowestSupportedVersion) {
  thrift::OpenrVersions openrVersions;
  openrVersions.version_ref() = version;
  openrVersions.lowestSupportedVersion_ref() = lowestSupportedVersion;
  return openrVersions;
}

// construct thrift::Value
thrift::Value
createThriftValue(
    int64_t version,
    std::string originatorId,
    std::optional<std::string> data,
    int64_t ttl,
    int64_t ttlVersion,
    std::optional<int64_t> hash) {
  thrift::Value value;
  value.version_ref() = version;
  value.originatorId_ref() = originatorId;
  value.value_ref().from_optional(data);
  value.ttl_ref() = ttl;
  value.ttlVersion_ref() = ttlVersion;
  if (hash.has_value()) {
    value.hash_ref().from_optional(hash);
  } else {
    value.hash_ref() = generateHash(version, originatorId, data);
  }

  return value;
}

// Create thrift::Value without setting thrift::Value.value
// Used for monitoring applications.
thrift::Value
createThriftValueWithoutBinaryValue(const thrift::Value& val) {
  thrift::Value updatedVal;
  updatedVal.version_ref() = *val.version_ref();
  updatedVal.originatorId_ref() = *val.originatorId_ref();
  updatedVal.ttl_ref() = *val.ttl_ref();
  updatedVal.ttlVersion_ref() = *val.ttlVersion_ref();
  if (val.hash_ref().has_value()) {
    updatedVal.hash_ref() = *val.hash_ref();
  }
  return updatedVal;
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
      PrefixKey(nodeName, toIPNetwork(*prefixEntry.prefix_ref()), area),
      createPrefixDb(nodeName, {prefixEntry}, area, withdraw)};
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

thrift::Publication
createThriftPublication(
    const std::unordered_map<std::string, thrift::Value>& kv,
    const std::vector<std::string>& expiredKeys,
    const std::optional<std::vector<std::string>>& nodeIds,
    const std::optional<std::vector<std::string>>& keysToUpdate,
    const std::optional<std::string>& floodRootId,
    const std::string& area) {
  thrift::Publication pub;
  *pub.keyVals_ref() = kv;
  *pub.expiredKeys_ref() = expiredKeys;
  pub.nodeIds_ref().from_optional(nodeIds);
  pub.tobeUpdatedKeys_ref().from_optional(keysToUpdate);
  pub.floodRootId_ref().from_optional(floodRootId);
  *pub.area_ref() = area;
  return pub;
}

thrift::OriginatedPrefixEntry
createOriginatedPrefixEntry(
    const thrift::OriginatedPrefix& originatedPrefix,
    const std::vector<std::string>& supportingPrefixes,
    bool installed) {
  thrift::OriginatedPrefixEntry entry;
  entry.prefix_ref() = originatedPrefix;
  entry.supporting_prefixes_ref() = supportingPrefixes;
  entry.installed_ref() = installed;
  return entry;
}

thrift::NextHopThrift
createNextHop(
    thrift::BinaryAddress addr,
    std::optional<std::string> ifName,
    int32_t metric,
    std::optional<thrift::MplsAction> maybeMplsAction,
    const std::optional<std::string>& area,
    const std::optional<std::string>& neighborNodeName) {
  thrift::NextHopThrift nextHop;
  nextHop.address_ref() = addr;
  nextHop.address_ref()->ifName_ref().from_optional(std::move(ifName));
  nextHop.metric_ref() = metric;
  nextHop.mplsAction_ref().from_optional(maybeMplsAction);
  nextHop.area_ref().from_optional(area);
  nextHop.neighborNodeName_ref().from_optional(neighborNodeName);
  return nextHop;
}

thrift::MplsAction
createMplsAction(
    thrift::MplsActionCode const mplsActionCode,
    std::optional<int32_t> maybeSwapLabel,
    std::optional<std::vector<int32_t>> maybePushLabels) {
  thrift::MplsAction mplsAction;
  mplsAction.action_ref() = mplsActionCode;
  mplsAction.swapLabel_ref().from_optional(maybeSwapLabel);
  mplsAction.pushLabels_ref().from_optional(maybePushLabels);
  checkMplsAction(mplsAction); // sanity checks
  return mplsAction;
}

thrift::PrefixEntry
createBgpWithdrawEntry(const thrift::IpPrefix& prefix) {
  thrift::PrefixEntry pfx;
  pfx.type_ref() = thrift::PrefixType::BGP;
  *pfx.prefix_ref() = prefix;
  return pfx;
}

thrift::UnicastRoute
createUnicastRoute(
    thrift::IpPrefix dest, std::vector<thrift::NextHopThrift> nextHops) {
  thrift::UnicastRoute unicastRoute;
  unicastRoute.dest_ref() = std::move(dest);
  std::sort(nextHops.begin(), nextHops.end());
  unicastRoute.nextHops_ref() = std::move(nextHops);
  return unicastRoute;
}

thrift::UnicastRouteDetail
createUnicastRouteDetail(
    thrift::IpPrefix dest,
    std::vector<thrift::NextHopThrift> nextHops,
    std::optional<thrift::PrefixEntry> maybeBestRoute) {
  thrift::UnicastRouteDetail unicastRouteDetail;
  unicastRouteDetail.unicastRoute_ref() = createUnicastRoute(dest, nextHops);
  unicastRouteDetail.bestRoute_ref().from_optional(maybeBestRoute);
  return unicastRouteDetail;
}

thrift::MplsRoute
createMplsRoute(int32_t topLabel, std::vector<thrift::NextHopThrift> nextHops) {
  // Sanity checks
  CHECK(isMplsLabelValid(topLabel));
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.mplsAction_ref().has_value());
  }

  thrift::MplsRoute mplsRoute;
  mplsRoute.topLabel_ref() = topLabel;
  std::sort(nextHops.begin(), nextHops.end());
  mplsRoute.nextHops_ref() = std::move(nextHops);
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
    if (prefixEntries.count(nodeArea) == 0) {
      continue;
    }
    int32_t dist = prefixEntries.at(nodeArea)->get_metrics().get_distance();
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
selectShortestDistance2(
    const PrefixEntries& prefixEntries, std::set<NodeAndArea>& nodeAreaSet) {
  // Get results with shortest distance.
  std::set<NodeAndArea> shortestSet =
      selectShortestDistance(prefixEntries, nodeAreaSet);
  // Remove NodeAndArea with shortest distance.
  for (const auto& nodeArea : shortestSet) {
    nodeAreaSet.erase(nodeArea);
  }
  // Get results with second shortest distance.
  std::set<NodeAndArea> secShortestSet =
      selectShortestDistance(prefixEntries, nodeAreaSet);

  std::set<NodeAndArea> ret;
  std::merge(
      shortestSet.begin(),
      shortestSet.end(),
      secShortestSet.begin(),
      secShortestSet.end(),
      std::inserter(ret, ret.begin()));
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
    thrift::RouteSelectionAlgorithm algorithm) {
  // First, select prefixEntries with best <path_preference, source_preference>
  // tuples. This is a must-have regardless of route selection algorithms.
  // Leverage tuple for ease of comparision.
  std::tuple<int32_t, int32_t> bestMetricsTuple{
      std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min()};
  std::set<NodeAndArea> nodeAreaSet;
  for (auto& [key, metricsWrapper] : prefixEntries) {
    auto& metrics = metricsWrapper->get_metrics();
    std::tuple<int32_t, int32_t> metricsTuple{
        metrics.get_path_preference(), /* prefer-higher */
        metrics.get_source_preference() /* prefer-higher */};

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

  // Second, select routes based on selection algorithm.
  switch (algorithm) {
  case thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE:
    return selectShortestDistance(prefixEntries, nodeAreaSet);
  case thrift::RouteSelectionAlgorithm::K_SHORTEST_DISTANCE_2:
    return selectShortestDistance2(prefixEntries, nodeAreaSet);
  case thrift::RouteSelectionAlgorithm::PER_AREA_SHORTEST_DISTANCE:
    return selectShortestDistancePerArea(prefixEntries, nodeAreaSet);
  default:
    XLOG(INFO) << "Unsupported route selection algorithm "
               << apache::thrift::util::enumNameSafe(algorithm);
    break;
  }

  return std::set<NodeAndArea>();
}

namespace MetricVectorUtils {

std::optional<const openr::thrift::MetricEntity>
getMetricEntityByType(const openr::thrift::MetricVector& mv, int64_t type) {
  for (auto& me : *mv.metrics_ref()) {
    if (*me.type_ref() == type) {
      return me;
    }
  }
  return std::nullopt;
}

// Utility method to create metric entity.
thrift::MetricEntity
createMetricEntity(
    int64_t type,
    int64_t priority,
    thrift::CompareType op,
    bool isBestPathTieBreaker,
    const std::vector<int64_t>& metric) {
  thrift::MetricEntity me;

  me.type_ref() = type;
  me.priority_ref() = priority;
  me.op_ref() = op;
  me.isBestPathTieBreaker_ref() = isBestPathTieBreaker;
  me.metric_ref() = metric;

  return me;
}

CompareResult
operator!(CompareResult mv) {
  switch (mv) {
  case CompareResult::WINNER: {
    return CompareResult::LOOSER;
  }
  case CompareResult::TIE_WINNER: {
    return CompareResult::TIE_LOOSER;
  }
  case CompareResult::TIE: {
    return CompareResult::TIE;
  }
  case CompareResult::TIE_LOOSER: {
    return CompareResult::TIE_WINNER;
  }
  case CompareResult::LOOSER: {
    return CompareResult::WINNER;
  }
  case CompareResult::ERROR: {
    return CompareResult::ERROR;
  }
  }
  return CompareResult::ERROR;
}

bool
isDecisive(CompareResult const& result) {
  return CompareResult::WINNER == result || CompareResult::LOOSER == result ||
      CompareResult::ERROR == result;
}

bool
isSorted(thrift::MetricVector const& mv) {
  int64_t priorPriority = std::numeric_limits<int64_t>::max();
  for (auto const& ent : *mv.metrics_ref()) {
    if (*ent.priority_ref() > priorPriority) {
      return false;
    }
    priorPriority = *ent.priority_ref();
  }
  return true;
}

// sort a metric vector in decreasing order of priority
void
sortMetricVector(thrift::MetricVector const& mv) {
  if (isSorted(mv)) {
    return;
  }
  std::vector<thrift::MetricEntity>& metrics =
      const_cast<std::vector<thrift::MetricEntity>&>(*mv.metrics_ref());
  std::sort(
      metrics.begin(),
      metrics.end(),
      [](thrift::MetricEntity& l, thrift::MetricEntity& r) {
        return *l.priority_ref() > *r.priority_ref();
      });
}

CompareResult
compareMetrics(
    std::vector<int64_t> const& l,
    std::vector<int64_t> const& r,
    bool tieBreaker) {
  if (l.size() != r.size()) {
    return CompareResult::ERROR;
  }
  for (auto lIter = l.begin(), rIter = r.begin(); lIter != l.end();
       ++lIter, ++rIter) {
    if (*lIter > *rIter) {
      return tieBreaker ? CompareResult::TIE_WINNER : CompareResult::WINNER;
    } else if (*lIter < *rIter) {
      return tieBreaker ? CompareResult::TIE_LOOSER : CompareResult::LOOSER;
    }
  }
  return CompareResult::TIE;
}

CompareResult
resultForLoner(thrift::MetricEntity const& entity) {
  if (thrift::CompareType::WIN_IF_PRESENT == *entity.op_ref()) {
    return *entity.isBestPathTieBreaker_ref() ? CompareResult::TIE_WINNER
                                              : CompareResult::WINNER;
  } else if (thrift::CompareType::WIN_IF_NOT_PRESENT == *entity.op_ref()) {
    return *entity.isBestPathTieBreaker_ref() ? CompareResult::TIE_LOOSER
                                              : CompareResult::LOOSER;
  }
  // IGNORE_IF_NOT_PRESENT
  return CompareResult::TIE;
}

void
maybeUpdate(CompareResult& target, CompareResult update) {
  if (isDecisive(update) || CompareResult::TIE == target) {
    target = update;
  }
}

CompareResult
compareMetricVectors(
    thrift::MetricVector const& l, thrift::MetricVector const& r) {
  CompareResult result = CompareResult::TIE;

  if (*l.version_ref() != *r.version_ref()) {
    return CompareResult::ERROR;
  }

  sortMetricVector(l);
  sortMetricVector(r);

  auto lIter = l.metrics_ref()->begin();
  auto rIter = r.metrics_ref()->begin();
  while (!isDecisive(result) &&
         (lIter != l.metrics_ref()->end() && rIter != r.metrics_ref()->end())) {
    if (*lIter->type_ref() == *rIter->type_ref()) {
      if (*lIter->isBestPathTieBreaker_ref() !=
          *rIter->isBestPathTieBreaker_ref()) {
        maybeUpdate(result, CompareResult::ERROR);
      } else {
        maybeUpdate(
            result,
            compareMetrics(
                *lIter->metric_ref(),
                *rIter->metric_ref(),
                *lIter->isBestPathTieBreaker_ref()));
      }
      ++lIter;
      ++rIter;
    } else if (*lIter->priority_ref() > *rIter->priority_ref()) {
      maybeUpdate(result, resultForLoner(*lIter));
      ++lIter;
    } else if (*lIter->priority_ref() < *rIter->priority_ref()) {
      maybeUpdate(result, !resultForLoner(*rIter));
      ++rIter;
    } else {
      // priorities are the same but types are different
      maybeUpdate(result, CompareResult::ERROR);
    }
  }
  while (!isDecisive(result) && lIter != l.metrics_ref()->end()) {
    maybeUpdate(result, resultForLoner(*lIter));
    ++lIter;
  }
  while (!isDecisive(result) && rIter != r.metrics_ref()->end()) {
    maybeUpdate(result, !resultForLoner(*rIter));
    ++rIter;
  }
  return result;
}
} // namespace MetricVectorUtils

namespace memory {

uint64_t
getThreadBytesImpl(bool isAllocated) {
  uint64_t bytes{0};
  const char* cmd = isAllocated ? "thread.allocated" : "thread.deallocated";
  try {
    folly::mallctlRead(cmd, &bytes);
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Failed to read thread allocated/de-allocated bytes: "
              << ex.what();
  }
  return bytes;
}

} // namespace memory
} // namespace openr
