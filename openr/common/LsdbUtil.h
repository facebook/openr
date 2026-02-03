/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <folly/logging/xlog.h>
#include <openr/common/BuildInfo.h>
#include <openr/common/LsdbTypes.h>
#include <openr/common/Util.h>
#include <openr/decision/RibEntry.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {

/**
 * Populate build info in thrift format
 */
thrift::BuildInfo getBuildInfoThrift() noexcept;

/**
 * [Perf Event] util functions for thrift::PerfEvent
 */
thrift::PerfEvent createPerfEvent(
    std::string nodeName, std::string eventDescr, int64_t unixTs);

void addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept;

std::vector<std::string> sprintPerfEvents(
    const thrift::PerfEvents& perfEvents) noexcept;

std::chrono::milliseconds getTotalPerfEventsDuration(
    const thrift::PerfEvents& perfEvents) noexcept;

folly::Expected<std::chrono::milliseconds, std::string>
getDurationBetweenPerfEvents(
    const thrift::PerfEvents& perfEvents,
    const std::string& firstName,
    const std::string& secondName) noexcept;

/**
 * Utility functions to convert thrift Enum value to string
 */
std::string toString(thrift::PrefixForwardingType const& value);
std::string toString(thrift::PrefixForwardingAlgorithm const& value);
std::string toString(thrift::PrefixType const& value);

/**
 * thrift::PrefixMetrics to string
 */
std::string toString(thrift::PrefixMetrics const& metrics);

/**
 * thrift::PrefixEntry to string. If detailed is set to true then output will
 * include `tags` and `area-stack`
 */
std::string toString(thrift::PrefixEntry const& entry, bool detailed = false);

/**
 * Convert NeighborEventType enum to string
 */
std::string toString(NeighborEventType const& type);

/**
 * Set value of bit string
 */
uint32_t bitStrValue(const folly::IPAddress& ip, uint32_t start, uint32_t end);

/**
 * @param prefixIndex subprefix index, starting from 0
 * @return n-th subprefix of allocated length in seed prefix
 * note: only handle IPv6 and assume seed prefix comes unmasked
 */
folly::CIDRNetwork getNthPrefix(
    const folly::CIDRNetwork& seedPrefix,
    uint32_t allocPrefixLen,
    uint32_t prefixIndex);

/**
 * Helper function to create loopback address (/128) out of network block.
 * Ideally any address in the block is valid address, in this case we just set
 * last bit of network block to `1`
 */
folly::IPAddress createLoopbackAddr(const folly::CIDRNetwork& prefix) noexcept;
folly::CIDRNetwork createLoopbackPrefix(
    const folly::CIDRNetwork& prefix) noexcept;

/**
 * TO BE DEPRECATED SOON: Backward compatible with empty remoteIfName
 * Translate remote interface name from local interface name
 * This is only applicable when remoteIfName is empty from peer adjacency
 * update It returns remoteIfName if it is there else constructs one from
 * localIfName
 */
std::string getRemoteIfName(const thrift::Adjacency& adj);

/**
 * Find delta between two route databases, it requires input to be sorted.
 */
thrift::RouteDatabaseDelta findDeltaRoutes(
    const thrift::RouteDatabase& newRouteDb,
    const thrift::RouteDatabase& oldRouteDb);

/**
 * Check if there are any best routes in that area by given 1. an area, 2.
 * prefixEntries, and 3. set of NodeAndArea
 *
 * Returns true if there are any best routes in the given area, otherwise false
 */
bool hasBestRoutesInArea(
    const std::string& area,
    const PrefixEntries& prefixEntries,
    const std::set<NodeAndArea>& bestNodeAreas);

/**
 * Utility functions for creating thrift objects
 */

thrift::PeerSpec createPeerSpec(
    const std::string& thriftPeerAddr = "",
    const int32_t port = 0,
    const thrift::KvStorePeerState state = thrift::KvStorePeerState::IDLE);

thrift::Adjacency createAdjacency(
    const std::string& nodeName,
    const std::string& ifName,
    const std::string& remoteIfName,
    const std::string& nextHopV6,
    const std::string& nextHopV4,
    int32_t metric,
    int32_t adjLabel,
    int64_t weight = Constants::kDefaultAdjWeight,
    bool adjOnlyUsedByOtherNode = false);

thrift::Adjacency createThriftAdjacency(
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
    bool adjOnlyUsedByOtherNode = false);

thrift::AdjacencyDatabase createAdjDb(
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    int32_t nodeLabel,
    bool overLoadBit = false,
    const std::string& area = kTestingAreaName,
    int32_t nodeMetricIncrementVal = 0);

thrift::PrefixDatabase createPrefixDb(
    const std::string& nodeName,
    const std::vector<thrift::PrefixEntry>& prefixEntries = {},
    bool withdraw = false);

thrift::PrefixEntry createPrefixEntry(
    thrift::IpPrefix prefix,
    thrift::PrefixType type = thrift::PrefixType::LOOPBACK,
    const std::string& data = "",
    thrift::PrefixForwardingType forwardingType =
        thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm =
        thrift::PrefixForwardingAlgorithm::SP_ECMP,
    std::optional<int64_t> minNexthop = std::nullopt,
    std::optional<int64_t> weight = std::nullopt);

thrift::PrefixMetrics createMetrics(int32_t pp, int32_t sp, int32_t d);

thrift::PrefixEntry createPrefixEntryWithMetrics(
    thrift::IpPrefix const& prefix,
    thrift::PrefixType const& type,
    thrift::PrefixMetrics const& metrics);

thrift::OpenrVersions createOpenrVersions(
    const thrift::OpenrVersion& version,
    const thrift::OpenrVersion& lowestSupportedVersion);

/**
 * Utility functions to create `key, value` pairs for updating route
 * advertisements in KvStore or tests
 */
std::pair<PrefixKey, std::shared_ptr<thrift::PrefixEntry>>
createPrefixKeyAndEntry(
    const std::string& nodeName,
    thrift::IpPrefix const& prefix,
    const std::string& area = kTestingAreaName);

std::pair<PrefixKey, thrift::PrefixDatabase> createPrefixKeyAndDb(
    const std::string& nodeName,
    const thrift::PrefixEntry& prefixEntry,
    const std::string& area = kTestingAreaName,
    bool withdraw = false);

std::pair<std::string, thrift::Value> createPrefixKeyValue(
    const std::string& nodeName,
    const int64_t version,
    const thrift::PrefixEntry& prefixEntry,
    const std::string& area = kTestingAreaName,
    bool withdraw = false);

std::pair<std::string, thrift::Value> createPrefixKeyValue(
    const std::string& nodeName,
    const int64_t version,
    thrift::IpPrefix const& prefix,
    const std::string& area = kTestingAreaName,
    bool withdraw = false);

thrift::OriginatedPrefixEntry createOriginatedPrefixEntry(
    const thrift::OriginatedPrefix& originatedPrefix,
    const std::vector<std::string>& supportingPrefixes,
    bool installed = false);

thrift::NextHopThrift createNextHop(
    thrift::BinaryAddress addr,
    std::optional<std::string> ifName = std::nullopt,
    int32_t metric = 0,
    std::optional<thrift::MplsAction> maybeMplsAction = std::nullopt,
    const std::optional<std::string>& area = std::nullopt,
    const std::optional<std::string>& neighborNodeName = std::nullopt,
    int64_t weight = 0);

thrift::MplsAction createMplsAction(
    thrift::MplsActionCode const mplsActionCode,
    std::optional<int32_t> maybeSwapLabel = std::nullopt,
    std::optional<std::vector<int32_t>> maybePushLabels = std::nullopt);

thrift::UnicastRoute createUnicastRoute(
    thrift::IpPrefix dest, std::vector<thrift::NextHopThrift> nextHops);

thrift::UnicastRouteDetail createUnicastRouteDetail(
    thrift::IpPrefix dest,
    std::vector<thrift::NextHopThrift> nextHops,
    std::optional<thrift::PrefixEntry> maybeBestRoute = std::nullopt);

thrift::MplsRoute createMplsRoute(
    int32_t topLabel, std::vector<thrift::NextHopThrift> nextHops);

std::vector<thrift::UnicastRoute> createUnicastRoutesFromMap(
    const std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>&
        unicastRoutes);
std::vector<thrift::MplsRoute> createMplsRoutesFromMap(
    const std::unordered_map<int32_t, RibMplsEntry>& mplsRoutes);

std::string getNodeNameFromKey(const std::string& key);

/**
 * Implements Open/R best route selection based on `thrift::PrefixMetrics`. The
 * metrics are compared and keys representing the best metric are returned. It
 * is upto the caller of this function to determine the representative best
 * key, if more than one keys are returned. Choosing a lowest key as the
 * representative, will be deterministic and easier for implementation.
 *
 * NOTE: MetricsWrapper is expected to provide following API
 *   apache::thrift::field_ref<const thrift::PrefixMetrics&> metrics();
 */
template <typename Key, typename MetricsWrapper>
std::set<Key>
selectBestPrefixMetrics(
    std::unordered_map<Key, MetricsWrapper> const& prefixes) {
  // Leveraging tuple for ease of comparision
  std::tuple<int32_t, int32_t, int32_t> bestMetricsTuple{
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::min()};
  std::set<Key> bestKeys;
  for (auto& [key, metricsWrapper] : prefixes) {
    auto& metrics = metricsWrapper.metrics().value();
    std::tuple<int32_t, int32_t, int32_t> metricsTuple{
        metrics.path_preference().value(), /* prefer-higher */
        metrics.source_preference().value(), /* prefer-higher */
        metrics.distance().value() * -1 /* prefer-lower */};

    // Skip if this is less than best metrics we've seen so far
    if (metricsTuple < bestMetricsTuple) {
      continue;
    }

    // Clear set and update best metric if this is a new best metric
    if (metricsTuple > bestMetricsTuple) {
      bestMetricsTuple = metricsTuple;
      bestKeys.clear();
    }

    // Current metrics is either best or same as best metrics we've seen so far
    bestKeys.emplace(key);
  }

  return bestKeys;
}

/**
 * Selected routes from received prefix entry announcements of one prefix,
 * following the instruction of route selection algorithm.
 */
std::set<NodeAndArea> selectRoutes(
    const PrefixEntries& prefixEntries,
    thrift::RouteSelectionAlgorithm algorithm,
    const folly::F14FastSet<NodeAndArea>& drainedNodes = {});

// Deterministically choose one as best path from multipaths. Used in Decision.
// Choose local if local node is a part of the multipaths.
// Otherwise choose smallest key: allNodeAreas.begin().
NodeAndArea selectBestNodeArea(
    std::set<NodeAndArea> const& allNodeAreas, std::string const& myNodeName);

} // namespace openr
