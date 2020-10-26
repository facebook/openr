/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <random>
#include <string>
#include <vector>

#include <boost/functional/hash.hpp>
#include <folly/FileUtil.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <glog/logging.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/BuildInfo.h>
#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/AllocPrefix_types.h>
#include <openr/if/gen-cpp2/Decision_types.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Spark_types.h>

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

#define LOG_FN_EXECUTION_TIME                                                \
  auto FB_ANONYMOUS_VARIABLE(SCOPE_EXIT_STATE) =                           \
  ::folly::detail::ScopeGuardOnExit() +                                    \
  [&, fn=__FUNCTION__, ts=std::chrono::steady_clock::now()] () noexcept { \
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(   \
        std::chrono::steady_clock::now() - ts);                              \
    VLOG(1) << "Execution time for " << fn << " took " << duration.count()   \
            << "ms";                                                         \
  }

namespace openr {

/**
 * Class to store re2 objects, provides API to match string with regex
 */
class KeyPrefix {
 public:
  explicit KeyPrefix(std::vector<std::string> const& keyPrefixList);
  bool keyMatch(std::string const& key) const;

 private:
  std::unique_ptr<re2::RE2::Set> keyPrefix_;
};

/**
 * PrefixKey class to form and parse a PrefixKey. PrefixKey can be instantiated
 * by passing parameters to form a key, or by passing the key string to parse
 * and populate the parameters. In case the parsing fails all the parameters
 * are set to std::nullopt
 */

class PrefixKey {
 public:
  // constructor using IP address, type and and subtype
  PrefixKey(
      std::string const& node,
      folly::CIDRNetwork const& prefix,
      const std::string& area = thrift::KvStore_constants::kDefaultArea());

  // construct PrefixKey object from a give key string
  static folly::Expected<PrefixKey, std::string> fromStr(
      const std::string& key);

  // return node name
  std::string getNodeName() const;

  // return the CIDR network address
  folly::CIDRNetwork getCIDRNetwork() const;

  // return prefix sub type
  std::string getPrefixArea() const;

  // return prefix key string to be used to flood to kvstore
  std::string getPrefixKey() const;

  // return thrift::IpPrefix
  thrift::IpPrefix getIpPrefix() const;

  static const RE2&
  getPrefixRE2() {
    static const RE2 prefixKeyPattern{folly::sformat(
        "{}(?P<node>[a-zA-Z\\d\\.\\-\\_]+):"
        "(?P<area>[a-zA-Z0-9\\.\\_\\-]+):"
        "\\[(?P<IPAddr>[a-fA-F\\d\\.\\:]+)/"
        "(?P<plen>[\\d]{{1,3}})\\]",
        Constants::kPrefixDbMarker.toString())};
    return prefixKeyPattern;
  }

 private:
  // node name
  std::string node_{};

  // IP address
  folly::CIDRNetwork prefix_;

  // prefix area
  std::string prefixArea_;

  // prefix key string
  std::string prefixKeyString_;
};

/**
 * Utility function to execute shell command and return true/false as
 * indication of it's success
 */
int executeShellCommand(const std::string& command);

// get prefix length from ipv6 mask
int maskToPrefixLen(const struct sockaddr_in6* mask);

// get prefix length from ipv4 mask
int maskToPrefixLen(const struct sockaddr_in* mask);

// set value of bit string
uint32_t bitStrValue(const folly::IPAddress& ip, uint32_t start, uint32_t end);

// report all IPv6/IPv4 prefixes configured on the interface
std::vector<folly::CIDRNetwork> getIfacePrefixes(
    std::string ifName, sa_family_t afNet);

bool matchRegexSet(
    const std::string& name, std::shared_ptr<const re2::RE2::Set> regexSet);
bool checkIncludeExcludeRegex(
    const std::string& name,
    std::shared_ptr<const re2::RE2::Set> includeRegexSet,
    std::shared_ptr<const re2::RE2::Set> excludeRegexSet);

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

template <typename SetT, typename T = typename SetT::value_type>
SetT
buildSetDifference(SetT const& lhs, SetT const& rhs) {
  std::vector<T> result;
  std::copy_if(
      lhs.begin(), lhs.end(), std::back_inserter(result), [&rhs](T const& val) {
        return rhs.find(val) == rhs.end();
      });

  SetT diff(
      std::make_move_iterator(result.begin()),
      std::make_move_iterator(result.end()));

  return diff;
}

inline int64_t
getUnixTimeStampMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/**
 * Add a perf event
 */
void addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept;

// util for parsing lists from the command line
std::vector<std::string> splitByComma(const std::string& input);

bool fileExists(const std::string& path);

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
 * Get forwarding algorithm and type from list of prefixes. We're taking map as
 * input for efficiency purpose.
 *
 * It is feasible that multiple nodes will advertise a same prefix and
 * will ask to forward on different modes and algorithms. In case of conflict
 * forwarding type and algorithm with the lowest enum value will be picked.
 */
std::pair<thrift::PrefixForwardingType, thrift::PrefixForwardingAlgorithm>
getPrefixForwardingTypeAndAlgorithm(
    const PrefixEntries& prefixEntries,
    const std::set<NodeAndArea>& bestNodeAreas);

/**
 * Validates that label is 20 bit only and other bits are not set
 * XXX: We can do more validation - e.g. reserved range, global vs local range
 */
inline bool
isMplsLabelValid(int32_t const mplsLabel) {
  return (mplsLabel & 0xfff00000) == 0;
}

/**
 * Validates mplsAction object and fatals
 */
void checkMplsAction(thrift::MplsAction const& mplsAction);

template <class T>
auto
castToStd(apache::thrift::optional_field_ref<T> t) {
  return t.to_optional();
}

template <class T>
std::optional<T>
castToStd(const folly::Optional<T>& t) {
  if (t) {
    return *t;
  }
  return {};
}

template <class T>
std::optional<T>
castToStd(folly::Optional<T>&& t) {
  if (t) {
    return std::move(*t);
  }
  return {};
}

//
// template method to return jittered time based on:
//
// @param: base => base value for random number generation;
// @param: pct => percentage of the deviation from base value;
//
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

thrift::PeerSpec createPeerSpec(
    const std::string& cmdUrl,
    const std::string& thriftPeerAddr = "",
    const int32_t port = 0);

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
    const std::string& area = openr::thrift::KvStore_constants::kDefaultArea(),
    const std::string& state = "IDLE");

thrift::SparkNeighborEvent createSparkNeighborEvent(
    thrift::SparkNeighborEventType event, const thrift::SparkNeighbor& info);

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
    const std::string& area = openr::thrift::KvStore_constants::kDefaultArea());

thrift::PrefixDatabase createPrefixDb(
    const std::string& nodeName,
    const std::vector<thrift::PrefixEntry>& prefixEntries = {},
    const std::string& area = std::string{
        openr::thrift::KvStore_constants::kDefaultArea()});

thrift::PrefixEntry createPrefixEntry(
    thrift::IpPrefix prefix,
    thrift::PrefixType type = thrift::PrefixType::LOOPBACK,
    const std::string& data = "",
    thrift::PrefixForwardingType forwardingType =
        thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm =
        thrift::PrefixForwardingAlgorithm::SP_ECMP,
    std::optional<bool> ephemeral = std::nullopt,
    std::optional<thrift::MetricVector> mv = std::nullopt,
    std::optional<int64_t> minNexthop = std::nullopt);

thrift::Value createThriftValue(
    int64_t version,
    std::string originatorId,
    std::optional<std::string> data,
    int64_t ttl = Constants::kTtlInfinity,
    int64_t ttlVersion = 0,
    std::optional<int64_t> hash = std::nullopt);

thrift::Value createThriftValueWithoutBinaryValue(const thrift::Value& val);

/**
 * Utility function to create `key, value` pair for updating route in KvStore
 */
std::pair<std::string, thrift::Value> createPrefixKeyValue(
    const std::string& nodeName,
    const int64_t version,
    const thrift::PrefixEntry& prefixEntry,
    const std::string& area);

thrift::Publication createThriftPublication(
    const std::unordered_map<std::string, thrift::Value>& kv,
    const std::vector<std::string>& expiredKeys,
    const std::optional<std::vector<std::string>>& nodeIds = std::nullopt,
    const std::optional<std::vector<std::string>>& keysToUpdate = std::nullopt,
    const std::optional<std::string>& floodRootId = std::nullopt,
    const std::string& area = openr::thrift::KvStore_constants::kDefaultArea());

thrift::InterfaceInfo createThriftInterfaceInfo(
    const bool isUp,
    const int ifIndex,
    const std::vector<thrift::IpPrefix>& networks);

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

thrift::MplsRoute createMplsRoute(
    int32_t topLabel, std::vector<thrift::NextHopThrift> nextHops);

std::vector<thrift::UnicastRoute> createUnicastRoutesFromMap(
    const std::unordered_map<thrift::IpPrefix, thrift::UnicastRoute>&
        unicastRoutes);

/**
 * Given list of nextHops for mpls route, validate nexthops and return
 * subset of nextHops with the same MplsActionCode. PHP (immediate) next-hops
 * are preferred over SWAP (in-direct) next-hops
 *
 * Later two APIs are bulk conversion of MPLS route
 */
std::vector<thrift::NextHopThrift> selectMplsNextHops(
    std::vector<thrift::NextHopThrift> const& nextHops);
std::vector<thrift::MplsRoute> createMplsRoutesWithSelectedNextHops(
    const std::vector<thrift::MplsRoute>& routes);
std::vector<thrift::MplsRoute> createMplsRoutesWithSelectedNextHopsMap(
    const std::unordered_map<uint32_t, thrift::MplsRoute>& mplsRoutes);

std::string getNodeNameFromKey(const std::string& key);

std::string createPeerSyncId(const std::string& node, const std::string& area);

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

// Deterministic choose one as best path from multipaths. Used in Decision.
// Choose local if local node is a part of the multipaths.
// Otherwise choose smallest key: allNodeAreas.begin().
NodeAndArea selectBestNodeArea(
    std::set<NodeAndArea> const& allNodeAreas, std::string const& myNodeName);

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
  std::tuple<int32_t, int32_t, int32_t> bestMetricsTuple{0, 0, 0};

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
