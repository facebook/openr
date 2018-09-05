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
#include <folly/String.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <re2/re2.h>
#include <re2/set.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/BuildInfo.h>
#include <openr/common/Constants.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/AllocPrefix_types.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/IpPrefix_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace std {

/**
 * Make IpPrefix hashable
 */
template <>
struct hash<openr::thrift::IpPrefix> {
  size_t operator()(openr::thrift::IpPrefix const&) const;
};

/**
 * Make BinaryAddress hashable
 */
template <>
struct hash<openr::thrift::BinaryAddress> {
  size_t operator()(openr::thrift::BinaryAddress const&) const;
};

/**
 * Make UnicastRoute hashable
 */
template <>
struct hash<openr::thrift::UnicastRoute> {
  size_t operator()(openr::thrift::UnicastRoute const&) const;
};

} // namespace std

namespace openr {

/**
 * Class to store re2 objects, provides API to match string with regex
 */
class KeyPrefix{
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

// report all IPv6/IPv4 prefixes configured on the interface
std::vector<folly::CIDRNetwork> getIfacePrefixes(
    std::string ifName, sa_family_t afNet);

bool checkIncludeExcludeRegex(
    const std::string& name,
    const std::unique_ptr<re2::RE2::Set>& includeRegexList,
    const std::unique_ptr<re2::RE2::Set>& excludeRegexList);

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
folly::IPAddress
createLoopbackAddr(const folly::CIDRNetwork& prefix) noexcept;

folly::CIDRNetwork
createLoopbackPrefix(const folly::CIDRNetwork& prefix) noexcept;

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

/**
 * Print perf event and return total convergence time
 */
std::vector<std::string> sprintPerfEvents(
    const thrift::PerfEvents& perfEvents) noexcept;
std::chrono::milliseconds getTotalPerfEventsDuration(
    const thrift::PerfEvents& perfEvents) noexcept;

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
std::string getRemoteIfName(
  const thrift::Adjacency& adj);

/**
 * Given list of paths returns the list of best paths (paths with lowest
 * metric value).
 */
std::vector<thrift::Path> getBestPaths(std::vector<thrift::Path> const& paths);

/**
 * Transform `thrift::Route` object to `thrift::UnicastRoute` object
 * Only best nexthops are retained
 */
std::vector<thrift::UnicastRoute> createUnicastRoutes(
    const std::vector<thrift::Route>& routes);

/**
 * Find delta between two route databases
 * Return type is a pair of <RoutesToBeUpdate, routesToRemove>
 */
std::pair<std::vector<thrift::UnicastRoute>, std::vector<thrift::IpPrefix>>
findDeltaRoutes(
    const thrift::RouteDatabase& newRouteDb,
    const thrift::RouteDatabase& oldRouteDb);

thrift::BuildInfo getBuildInfoThrift() noexcept;

folly::IPAddress
toIPAddress(const thrift::fbbinary& binAddr);

} // namespace openr
