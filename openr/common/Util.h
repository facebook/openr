/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include <boost/functional/hash.hpp>
#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/FileUtil.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/String.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/BuildInfo.h>
#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/AllocPrefix_types.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>

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
 * Utility function to execute shell command and return true/false as indication
 * of it's success
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
    const std::string& name, const std::unique_ptr<re2::RE2::Set>& regexSet);

bool checkIncludeExcludeRegex(
    const std::string& name,
    const std::unique_ptr<re2::RE2::Set>& includeRegexSet,
    const std::unique_ptr<re2::RE2::Set>& excludeRegexSet);

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

std::unordered_map<std::string, fbzmq::thrift::Counter> prepareSubmitCounters(
    const std::unordered_map<std::string, int64_t>& counters);

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
getUnixTimeStamp() noexcept {
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
    const folly::Optional<std::string>& value);

/**
 * TO BE DEPRECATED SOON: Backward compatible with empty remoteIfName
 * Translate remote interface name from local interface name
 * This is only applicable when remoteIfName is empty from peer adjacency update
 * It returns remoteIfName if it is there else constructs one from localIfName
 */
std::string getRemoteIfName(const thrift::Adjacency& adj);

/**
 * Given list of nextHops returns the list of best nextHops (nextHops with
 * lowest metric value).
 */
std::vector<thrift::NextHopThrift> getBestNextHopsUnicast(
    std::vector<thrift::NextHopThrift> const& nextHops);

/**
 * Given list of nextHops for mpls route, validate nexthops and return nextHops
 * with lowest metric value and of same MplsActionCode.
 */
std::vector<thrift::NextHopThrift> getBestNextHopsMpls(
    std::vector<thrift::NextHopThrift> const& nextHops);

/**
 * Create deprecated nexthops
 */
std::vector<thrift::BinaryAddress> createDeprecatedNexthops(
    std::vector<thrift::NextHopThrift> const& nextHops);

/**
 * Find delta between two route databases
 */
thrift::RouteDatabaseDelta findDeltaRoutes(
    const thrift::RouteDatabase& newRouteDb,
    const thrift::RouteDatabase& oldRouteDb);

thrift::BuildInfo getBuildInfoThrift() noexcept;

folly::Optional<std::string> maybeGetTcpEndpoint(
    const std::string& addr, const int32_t port);

/**
 * Get forwarding type from list of prefixes. We're taking map as input for
 * efficiency purpose.
 * It is feasible that multiple nodes will advertise a same prefix and will ask
 * to forward on different modes. We will make sure that MPLS is used if and
 * only if everyone says MPLS else forwarding type will be IP.
 */
thrift::PrefixForwardingType getPrefixForwardingType(
    const std::unordered_map<std::string, thrift::PrefixEntry>& nodePrefixes);

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
inline void
checkMplsAction(thrift::MplsAction const& mplsAction) {
  switch (mplsAction.action) {
  case thrift::MplsActionCode::PUSH:
    // Swap label shouldn't be set
    CHECK(not mplsAction.swapLabel.hasValue());
    // Push labels should be set
    CHECK(mplsAction.pushLabels.hasValue());
    // there should be atleast one push label
    CHECK(not mplsAction.pushLabels->empty());
    for (auto const& label : mplsAction.pushLabels.value()) {
      CHECK(isMplsLabelValid(label));
    }
    break;
  case thrift::MplsActionCode::SWAP:
    // Swap label should be set
    CHECK(mplsAction.swapLabel.hasValue());
    CHECK(isMplsLabelValid(mplsAction.swapLabel.value()));
    // Push labels shouldn't be set
    CHECK(not mplsAction.pushLabels.hasValue());
    break;
  case thrift::MplsActionCode::PHP:
  case thrift::MplsActionCode::POP_AND_LOOKUP:
    // Swap label should not be set
    CHECK(not mplsAction.swapLabel.hasValue());
    CHECK(not mplsAction.pushLabels.hasValue());
    break;
  default:
    CHECK(false) << "Unknown action code";
  }
}

inline thrift::Adjacency
createAdjacency(
    const std::string& nodeName,
    const std::string& ifName,
    const std::string& remoteIfName,
    const std::string& nextHopV6,
    const std::string& nextHopV4,
    int32_t metric,
    int32_t adjLabel,
    int64_t weight = Constants::kDefaultAdjWeight) {
  auto now = std::chrono::system_clock::now();
  int64_t timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  return thrift::Adjacency(
      apache::thrift::FRAGILE,
      nodeName,
      ifName,
      toBinaryAddress(folly::IPAddress(nextHopV6)),
      toBinaryAddress(folly::IPAddress(nextHopV4)),
      metric,
      adjLabel,
      false /* overload bit status */,
      metric * 100,
      timestamp,
      weight,
      remoteIfName);
}

inline thrift::AdjacencyDatabase
createAdjDb(
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    int32_t nodeLabel) {
  auto adjDb = thrift::AdjacencyDatabase(
      apache::thrift::FRAGILE,
      nodeName,
      false /* overload bit status */,
      adjs,
      nodeLabel,
      thrift::PerfEvents());
  adjDb.perfEvents = folly::none;
  return adjDb;
}

inline thrift::PrefixDatabase
createPrefixDb(
    const std::string& nodeName,
    const std::vector<thrift::PrefixEntry>& prefixEntries) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName = nodeName;
  prefixDb.prefixEntries = prefixEntries;
  return prefixDb;
}

inline thrift::PrefixEntry
createPrefixEntry(
    thrift::IpPrefix prefix,
    thrift::PrefixType type = thrift::PrefixType::LOOPBACK,
    const std::string& data = "",
    thrift::PrefixForwardingType forwardingType =
        thrift::PrefixForwardingType::IP,
    folly::Optional<bool> ephemeral = folly::none,
    folly::Optional<thrift::MetricVector> mv = folly::none) {
  thrift::PrefixEntry prefixEntry;
  prefixEntry.prefix = prefix;
  prefixEntry.type = type;
  prefixEntry.data = data;
  prefixEntry.forwardingType = forwardingType;
  prefixEntry.ephemeral = ephemeral;
  prefixEntry.mv = mv;
  return prefixEntry;
}

inline thrift::NextHopThrift
createNextHop(
    thrift::BinaryAddress addr,
    const std::string& ifName,
    int32_t metric,
    folly::Optional<thrift::MplsAction> maybeMplsAction = folly::none) {
  thrift::NextHopThrift nextHop;
  nextHop.address = addr;
  nextHop.address.ifName = ifName;
  nextHop.metric = metric;
  nextHop.mplsAction = maybeMplsAction;
  return nextHop;
}

inline thrift::MplsAction
createMplsAction(
    thrift::MplsActionCode const mplsActionCode,
    folly::Optional<int32_t> maybeSwapLabel = folly::none,
    folly::Optional<std::vector<int32_t>> maybePushLabels = folly::none) {
  thrift::MplsAction mplsAction;
  mplsAction.action = mplsActionCode;
  mplsAction.swapLabel = maybeSwapLabel;
  mplsAction.pushLabels = maybePushLabels;
  checkMplsAction(mplsAction); // sanity checks
  return mplsAction;
}

inline thrift::UnicastRoute
createUnicastRoute(
    thrift::IpPrefix dest, std::vector<thrift::NextHopThrift> nextHops) {
  thrift::UnicastRoute unicastRoute;
  unicastRoute.dest = std::move(dest);
  std::sort(nextHops.begin(), nextHops.end());
  unicastRoute.nextHops = std::move(nextHops);
  return unicastRoute;
}

inline thrift::MplsRoute
createMplsRoute(int32_t topLabel, std::vector<thrift::NextHopThrift> nextHops) {
  // Sanity checks
  CHECK(isMplsLabelValid(topLabel));
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.mplsAction.hasValue());
  }

  thrift::MplsRoute mplsRoute;
  mplsRoute.topLabel = topLabel;
  std::sort(nextHops.begin(), nextHops.end());
  mplsRoute.nextHops = std::move(nextHops);
  return mplsRoute;
}

inline std::vector<thrift::UnicastRoute>
createUnicastRoutesWithBestNexthops(
    const std::vector<thrift::UnicastRoute>& routes) {
  // Build routes to be programmed
  std::vector<thrift::UnicastRoute> newRoutes;

  for (auto const& route : routes) {
    auto newRoute =
        createUnicastRoute(route.dest, getBestNextHopsUnicast(route.nextHops));
    // NOTE: remove after UnicastRoute.deprecatedNexthops is removed
    newRoute.deprecatedNexthops = createDeprecatedNexthops(newRoute.nextHops);
    newRoutes.emplace_back(std::move(newRoute));
  }

  return newRoutes;
}

inline std::vector<thrift::MplsRoute>
createMplsRoutesWithBestNextHops(const std::vector<thrift::MplsRoute>& routes) {
  // Build routes to be programmed
  std::vector<thrift::MplsRoute> newRoutes;

  for (auto const& route : routes) {
    newRoutes.emplace_back(
        createMplsRoute(route.topLabel, getBestNextHopsMpls(route.nextHops)));
  }

  return newRoutes;
}

} // namespace openr
