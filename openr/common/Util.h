/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <folly/FileUtil.h>
#include <folly/IPAddress.h>
#include <folly/memory/MallctlHelper.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <wangle/ssl/SSLContextConfig.h>
#include "openr/common/Types.h"

#include <openr/common/BuildInfo.h>
#include <openr/common/Constants.h>
#include <openr/common/MplsUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/decision/RibEntry.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/OpenrCtrl.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>

/**
 * Helper macro function to log execution time of function.
 * Usage:
 *  void someFunction() {
 *    LOG_FN_EXECUTION_TIME;
 *    ...
 *    <function-body>
 *    ...
 *  }
 *
 * Output:
 *  V0402 16:35:54 ... UtilTest.cpp:970] Execution time for TestBody took 0ms.
 */

#define LOG_FN_EXECUTION_TIME                                                  \
  auto FB_ANONYMOUS_VARIABLE(SCOPE_EXIT_STATE) =                               \
      ::folly::detail::ScopeGuardOnExit() +                                    \
      [&,                                                                      \
       fn = __FUNCTION__,                                                      \
       ts = std::chrono::steady_clock::now()]() noexcept {                     \
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( \
            std::chrono::steady_clock::now() - ts);                            \
        VLOG(1) << "Execution time for " << fn << " took " << duration.count() \
                << "ms";                                                       \
      }

const openr::AreaId kTestingAreaName{"test_area_name"};

namespace openr {
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

/*
 * Convert NeighborEventType enum to string
 */
std::string toString(NeighborEventType const& type);

/**
 * Setup thrift server for TLS
 */
void setupThriftServerTls(
    apache::thrift::ThriftServer& thriftServer,
    apache::thrift::SSLPolicy sslPolicy,
    std::string const& ticketSeedPath,
    std::shared_ptr<wangle::SSLContextConfig> sslContext);

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
 * Return unix timestamp - Number of milliseconds elapsed since the epoch
 */
inline int64_t
getUnixTimeStampMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/**
 * Add a perf event to perf event list
 */
void addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept;

/**
 * Print perf event and return total convergence time
 */
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
 * Generate hash for each keyval pair
 * as a abstract of version number, originator and values
 * TODO: Remove the API in favor of other one
 */
int64_t generateHash(
    const int64_t version,
    const std::string& originatorId,
    const std::optional<std::string>& value);

int64_t generateHash(
    const int64_t version,
    const std::string& originatorId,
    const apache::thrift::optional_field_ref<const std::string&> value);

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

thrift::BuildInfo getBuildInfoThrift() noexcept;

/**
 * Get forwarding algorithm and type from list of prefixes for the given area.
 * We're taking map as input for efficiency purpose.
 *
 * It is feasible that multiple nodes in the same area will advertise a same
 * prefix and will ask to forward on different modes and algorithms. In case
 * of conflict forwarding type and algorithm with the lowest enum value will be
 * picked.
 *
 * Returns std::nullopt if there are no best routes in the given area.
 */
std::optional<
    std::pair<thrift::PrefixForwardingType, thrift::PrefixForwardingAlgorithm>>
getPrefixForwardingTypeAndAlgorithm(
    const std::string& area,
    const PrefixEntries& prefixEntries,
    const std::set<NodeAndArea>& bestNodeAreas);

/**
 * Validates mplsAction object and fatals
 */
void checkMplsAction(thrift::MplsAction const& mplsAction);

/**
 * template method to return jittered time based on:
 *
 * @param: base => base value for random number generation;
 * @param: pct => percentage of the deviation from base value;
 */
template <class T>
T
addJitter(T base, double pct = 20.0) {
  CHECK(pct > 0 and pct <= 100) << "percentage input must between 0 and 100";
  thread_local static std::default_random_engine generator;
  std::uniform_int_distribution<int> distribution(
      pct / -100.0 * base.count(), pct / 100.0 * base.count());
  auto roll = std::bind(distribution, generator);
  return T(base.count() + roll());
}

/**
 * Utility functions for conversion between thrift objects and string/IOBuf
 */

template <typename ThriftType, typename Serializer>
std::string
writeThriftObjStr(ThriftType const& obj, Serializer& serializer) {
  std::string result;
  serializer.serialize(obj, &result);
  return result;
}

template <typename ThriftType, typename Serializer>
ThriftType
readThriftObj(folly::IOBuf& buf, Serializer& serializer) {
  ThriftType obj;
  serializer.deserialize(&buf, obj);
  return obj;
}

template <typename ThriftType, typename Serializer>
ThriftType
readThriftObjStr(const std::string& buf, Serializer& serializer) {
  ThriftType obj;
  serializer.deserialize(buf, obj);
  return obj;
}

/**
 * Utility functions for creating thrift objects
 */

thrift::PeerSpec createPeerSpec(
    const std::string& cmdUrl,
    const std::string& thriftPeerAddr = "",
    const int32_t port = 0,
    const thrift::KvStorePeerState state = thrift::KvStorePeerState::IDLE,
    const bool supportFloodOptimization = false);

thrift::SparkNeighbor createSparkNeighbor(
    const std::string& nodeName,
    const thrift::BinaryAddress& v4Addr,
    const thrift::BinaryAddress& v6Addr,
    const int64_t kvStoreCmdPort,
    const int64_t openrCtrlThriftPort,
    const int32_t label,
    const int64_t rttUs,
    const std::string& remoteIfName,
    const std::string& localIfName,
    const std::string& area = kTestingAreaName,
    const std::string& state = "IDLE");

thrift::Adjacency createAdjacency(
    const std::string& nodeName,
    const std::string& ifName,
    const std::string& remoteIfName,
    const std::string& nextHopV6,
    const std::string& nextHopV4,
    int32_t metric,
    int32_t adjLabel,
    int64_t weight = Constants::kDefaultAdjWeight);

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
    const std::string& remoteIfName);

thrift::AdjacencyDatabase createAdjDb(
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    int32_t nodeLabel,
    bool overLoadBit = false,
    const std::string& area = kTestingAreaName);

thrift::PrefixDatabase createPrefixDb(
    const std::string& nodeName,
    const std::vector<thrift::PrefixEntry>& prefixEntries = {},
    const std::string& area = kTestingAreaName,
    bool withdraw = false);

thrift::PrefixEntry createPrefixEntry(
    thrift::IpPrefix prefix,
    thrift::PrefixType type = thrift::PrefixType::LOOPBACK,
    const std::string& data = "",
    thrift::PrefixForwardingType forwardingType =
        thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm =
        thrift::PrefixForwardingAlgorithm::SP_ECMP,
    std::optional<thrift::MetricVector> mv = std::nullopt,
    std::optional<int64_t> minNexthop = std::nullopt);

thrift::PrefixEntry createPrefixEntryWithPrependLabel(
    thrift::IpPrefix prefix,
    std::optional<int32_t> prependLabel = std::nullopt);

thrift::PrefixMetrics createMetrics(int32_t pp, int32_t sp, int32_t d);

thrift::PrefixEntry createPrefixEntryWithMetrics(
    thrift::IpPrefix const& prefix,
    thrift::PrefixType const& type,
    thrift::PrefixMetrics const& metrics);

thrift::AllocPrefix createAllocPrefix(
    thrift::IpPrefix const& seedPrefix,
    int64_t allocPrefixLen,
    int64_t allocPrefixIndex);

thrift::PerfEvent createPerfEvent(
    std::string nodeName, std::string eventDescr, int64_t unixTs);

thrift::KvstoreFloodRate createKvstoreFloodRate(
    int32_t flood_msg_per_sec, int32_t flood_msg_burst_size);

thrift::OpenrVersions createOpenrVersions(
    const thrift::OpenrVersion& version,
    const thrift::OpenrVersion& lowestSupportedVersion);

thrift::Value createThriftValue(
    int64_t version,
    std::string originatorId,
    std::optional<std::string> data,
    int64_t ttl = Constants::kTtlInfinity,
    int64_t ttlVersion = 0,
    std::optional<int64_t> hash = std::nullopt);

thrift::Value createThriftValueWithoutBinaryValue(const thrift::Value& val);

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
    bool withdraw = false,
    bool isPrefixKeyV2 = false);

std::pair<std::string, thrift::Value> createPrefixKeyValue(
    const std::string& nodeName,
    const int64_t version,
    thrift::IpPrefix const& prefix,
    const std::string& area = kTestingAreaName,
    bool withdraw = false,
    bool isPrefixKeyV2 = false);

thrift::Publication createThriftPublication(
    const std::unordered_map<std::string, thrift::Value>& kv,
    const std::vector<std::string>& expiredKeys,
    const std::optional<std::vector<std::string>>& nodeIds = std::nullopt,
    const std::optional<std::vector<std::string>>& keysToUpdate = std::nullopt,
    const std::optional<std::string>& floodRootId = std::nullopt,
    const std::string& area = kTestingAreaName);

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
    const std::optional<std::string>& neighborNodeName = std::nullopt);

thrift::MplsAction createMplsAction(
    thrift::MplsActionCode const mplsActionCode,
    std::optional<int32_t> maybeSwapLabel = std::nullopt,
    std::optional<std::vector<int32_t>> maybePushLabels = std::nullopt);

thrift::PrefixEntry createBgpWithdrawEntry(const thrift::IpPrefix& prefix);

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
 *   apache::thrift::field_ref<const thrift::PrefixMetrics&> metrics_ref();
 */
template <typename Key, typename MetricsWrapper>
std::set<Key> selectBestPrefixMetrics(
    std::unordered_map<Key, MetricsWrapper> const& prefixes);

/**
 * Selected routes from received prefix entry announcements of one prefix,
 * following the instruction of route selection algorithm.
 */
std::set<NodeAndArea> selectRoutes(
    const PrefixEntries& prefixEntries,
    thrift::RouteSelectionAlgorithm algorithm);

// Deterministically choose one as best path from multipaths. Used in Decision.
// Choose local if local node is a part of the multipaths.
// Otherwise choose smallest key: allNodeAreas.begin().
NodeAndArea selectBestNodeArea(
    std::set<NodeAndArea> const& allNodeAreas, std::string const& myNodeName);

/**
 * TODO: Deprecated and the support for best route selection based on metric
 * vector will soon be dropped in favor of new `PrefixMetrics`
 */
namespace MetricVectorUtils {

enum class CompareResult { WINNER, TIE_WINNER, TIE, TIE_LOOSER, LOOSER, ERROR };

std::optional<const openr::thrift::MetricEntity> getMetricEntityByType(
    const openr::thrift::MetricVector& mv, int64_t type);

thrift::MetricEntity createMetricEntity(
    int64_t type,
    int64_t priority,
    thrift::CompareType op,
    bool isBestPathTieBreaker,
    const std::vector<int64_t>& metric);

CompareResult operator!(CompareResult mv);

bool isDecisive(CompareResult const& result);

bool isSorted(thrift::MetricVector const& mv);

// sort a metric vector in decreasing order of priority
void sortMetricVector(thrift::MetricVector const& mv);

CompareResult compareMetrics(
    std::vector<int64_t> const& l,
    std::vector<int64_t> const& r,
    bool tieBreaker);

CompareResult resultForLoner(thrift::MetricEntity const& entity);

void maybeUpdate(CompareResult& target, CompareResult update);

CompareResult compareMetricVectors(
    thrift::MetricVector const& l, thrift::MetricVector const& r);
} // namespace MetricVectorUtils

namespace memory {

uint64_t getThreadBytesImpl(bool isAllocated);

} // namespace memory

} // namespace openr

//
// Implementation of templatized functions
//

namespace openr {

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
    auto& metrics = metricsWrapper.metrics_ref().value();
    std::tuple<int32_t, int32_t, int32_t> metricsTuple{
        metrics.path_preference_ref().value(), /* prefer-higher */
        metrics.source_preference_ref().value(), /* prefer-higher */
        metrics.distance_ref().value() * -1 /* prefer-lower */};

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
} // namespace openr
