/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Util.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <unistd.h>

namespace std {

/**
 * Make IpPrefix hashable
 */
size_t
hash<openr::thrift::IpPrefix>::operator()(
    openr::thrift::IpPrefix const& ipPrefix) const {
  return hash<string>()(ipPrefix.prefixAddress.addr.toStdString()) +
      ipPrefix.prefixLength;
}

/**
 * Make BinaryAddress hashable
 */
size_t
hash<openr::thrift::BinaryAddress>::operator()(
    openr::thrift::BinaryAddress const& addr) const {
  size_t res = hash<string>()(addr.addr.toStdString());
  if (addr.ifName.hasValue()) {
    res += hash<string>()(addr.ifName.value());
  }
  return res;
}

/**
 * Make UnicastRoute hashable
 */
size_t
hash<openr::thrift::UnicastRoute>::operator()(
    openr::thrift::UnicastRoute const& route) const {
  size_t res = hash<openr::thrift::IpPrefix>()(route.dest);
  for (const auto& nh : route.nexthops) {
    res += hash<openr::thrift::BinaryAddress>()(nh);
  }
  return res;
}

} // namespace std

namespace openr {

// create RE2 set for the list of key prefixes
KeyPrefix::KeyPrefix(std::vector<std::string> const& keyPrefixList) {
  if (keyPrefixList.empty()) {
    return;
  }
  re2::RE2::Options re2Options;
  re2Options.set_case_sensitive(true);
  keyPrefix_ =
    std::make_unique<re2::RE2::Set>(re2Options, re2::RE2::ANCHOR_START);
  std::string re2AddError{};

  for (auto const& keyPrefix: keyPrefixList) {
    if (keyPrefix_->Add(keyPrefix, &re2AddError) < 0) {
      LOG(FATAL) << "Failed to add prefixes to RE2 set: '" << keyPrefix << "', "
                 << "error: '" << re2AddError << "'";
      return;
    }
  }
  if (!keyPrefix_->Compile()) {
    LOG(FATAL) << "Failed to compile re2 set" ;
    keyPrefix_.reset();
  }
}

// match the key with the list of prefixes
bool KeyPrefix::keyMatch(std::string const& key) const {
  if (!keyPrefix_) {
    return true;
  }
  std::vector<int> matches;
  return keyPrefix_->Match(key, &matches);
}

// need operator< for creating std::set of these thrift types. we need ordered
// set for set algebra. thrift declares but does not define these for us.

bool
thrift::BinaryAddress::operator<(const thrift::BinaryAddress& other) const {
  if (addr != other.addr) {
    return addr < other.addr;
  }
  return ifName < other.ifName;
}

bool
thrift::IpPrefix::operator<(const thrift::IpPrefix& other) const {
  if (prefixAddress != other.prefixAddress) {
    return prefixAddress < other.prefixAddress;
  }
  return prefixLength < other.prefixLength;
}

bool
thrift::Path::operator<(const openr::thrift::Path& other) const {
  if (metric != other.metric) {
    return metric < other.metric;
  }
  if (ifName != other.ifName) {
    return ifName < other.ifName;
  }
  return nextHop < other.nextHop;
}

bool
thrift::Route::operator<(const openr::thrift::Route& other) const {
  if (prefix != other.prefix) {
    return prefix < other.prefix;
  }
  auto myPaths = paths;
  auto otherPaths = other.paths;
  std::sort(myPaths.begin(), myPaths.end());
  std::sort(otherPaths.begin(), otherPaths.end());
  return myPaths < otherPaths;
}

int
executeShellCommand(const std::string& command) {
  int ret = system(command.c_str());
  ret = WEXITSTATUS(ret);
  if (ret != 0) {
    LOG(ERROR) << "Failed to execute command: '" << command << "', "
               << "exitCode: '" << ret << "'";
  }
  return ret;
}

bool
checkIncludeExcludeRegex(
    const std::string& name,
    const std::unique_ptr<re2::RE2::Set>& includeRegexList,
    const std::unique_ptr<re2::RE2::Set>& excludeRegexList) {
  if (!includeRegexList) {
    return false;
  }
  std::vector<int> matches;
  if (excludeRegexList && excludeRegexList->Match(name, &matches)) {
    return false;
  }

  return includeRegexList->Match(name, &matches);
}

std::vector<std::string>
splitByComma(const std::string& input) {
  std::vector<std::string> output;
  folly::split(",", input, output);

  return output;
}

folly::IPAddress
createLoopbackAddr(const folly::CIDRNetwork& prefix) noexcept {
  auto addr = prefix.first.mask(prefix.second);

  // Set last bit to `1` if prefix length is not full
  if (prefix.second != prefix.first.bitCount()) {
    auto bytes = std::string(
      reinterpret_cast<const char*>(addr.bytes()), addr.byteCount());
    bytes[bytes.size() - 1] |= 0x01;    // Set last bit to 1
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

int
maskToPrefixLen(const struct sockaddr_in6* mask) {
  int bits = 0;
  const struct in6_addr* addr = &(mask->sin6_addr);
  for (int i = 0; i < 16; i++) {
    if (addr->s6_addr[i] == (uint8_t)'\xFF') {
      bits += 8;
    } else {
      switch ((uint8_t)addr->s6_addr[i]) {
      case (uint8_t)'\xFE':
        bits += 7;
        break;
      case (uint8_t)'\xFC':
        bits += 6;
        break;
      case (uint8_t)'\xF8':
        bits += 5;
        break;
      case (uint8_t)'\xF0':
        bits += 4;
        break;
      case (uint8_t)'\xE0':
        bits += 3;
        break;
      case (uint8_t)'\xC0':
        bits += 2;
        break;
      case (uint8_t)'\x80':
        bits += 1;
        break;
      case (uint8_t)'\x00':
        bits += 0;
        break;
      default:
        return 0;
      }
      break;
    }
  }
  return bits;
}

int
maskToPrefixLen(const struct sockaddr_in* mask) {
  int bits = 32;
  uint32_t search = 1;
  while (!(mask->sin_addr.s_addr & search)) {
    --bits;
    search <<= 1;
  }
  return bits;
}

std::vector<folly::CIDRNetwork>
getIfacePrefixes(std::string ifName, sa_family_t afNet) {
  struct ifaddrs* ifaddr{nullptr};
  std::vector<folly::CIDRNetwork> results;

  auto ret = ::getifaddrs(&ifaddr);
  SCOPE_EXIT {
    freeifaddrs(ifaddr);
  };

  if (ret < 0) {
    LOG(ERROR) << folly::sformat(
        "failure listing interfacs: {}", folly::errnoStr(errno));
    return std::vector<folly::CIDRNetwork>{};
  }

  struct ifaddrs* ifa = ifaddr;
  struct in6_addr ip6;
  int prefixLength{0};

  for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (::strcmp(ifName.c_str(), ifa->ifa_name) ||
        ifa->ifa_addr == nullptr ||
        (afNet != AF_UNSPEC && ifa->ifa_addr->sa_family != afNet)) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET6) {
      ::memcpy(
          &ip6,
          &((struct sockaddr_in6*)ifa->ifa_addr)->sin6_addr,
          sizeof(in6_addr));
      prefixLength = maskToPrefixLen((struct sockaddr_in6*)ifa->ifa_netmask);
      folly::IPAddressV6 ifaceAddr(ip6);
      if (ifaceAddr.isLoopback() or ifaceAddr.isLinkLocal()) {
        continue;
      }
      results.emplace_back(
          folly::CIDRNetwork{ifaceAddr, prefixLength});
    }
    if (ifa->ifa_addr->sa_family == AF_INET) {
      struct in_addr ip;
      ::memcpy(
          &ip,
          &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr,
          sizeof(in_addr));
      prefixLength = maskToPrefixLen((struct sockaddr_in*)ifa->ifa_netmask);
      folly::IPAddressV4 ifaceAddr(ip);
      if (ifaceAddr.isLoopback()) {
        continue;
      }
      results.emplace_back(
          folly::CIDRNetwork{ifaceAddr, prefixLength});
    }
  }
  return results;
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
  const int32_t allocBits = std::min(
      32, static_cast<int32_t>(seedHostBitLen - allocHostBitLen));
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

std::unordered_map<std::string, fbzmq::thrift::Counter>
prepareSubmitCounters(
    const std::unordered_map<std::string, int64_t>& counters) {
  std::unordered_map<std::string, fbzmq::thrift::Counter> thriftCounters;
  for (const auto& kv : counters) {
    fbzmq::thrift::Counter counter;
    counter.value = kv.second;
    counter.valueType = fbzmq::thrift::CounterValueType::GAUGE;
    auto now = std::chrono::system_clock::now();
    // current unixtime in ms
    counter.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    thriftCounters.emplace(kv.first, counter);
  }
  return thriftCounters;
}

void
addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept {
  thrift::PerfEvent event(
      apache::thrift::FRAGILE, nodeName, eventDescr, getUnixTimeStamp());
  perfEvents.events.emplace_back(std::move(event));
}

std::vector<std::string>
sprintPerfEvents(const thrift::PerfEvents& perfEvents) noexcept {
  const auto& events = perfEvents.events;
  if (events.empty()) {
    return {};
  }

  std::vector<std::string> eventStrs;
  auto recentTs = events.front().unixTs;
  for (auto const& event : events) {
    auto durationMs = event.unixTs - recentTs;
    recentTs = event.unixTs;
    eventStrs.emplace_back(folly::sformat(
        "node: {}, event: {}, duration: {}ms, unix-timestamp: {}",
        event.nodeName,
        event.eventDescr,
        durationMs,
        event.unixTs));
  }
  return eventStrs;
}

std::chrono::milliseconds
getTotalPerfEventsDuration(const thrift::PerfEvents& perfEvents) noexcept {
  if (perfEvents.events.empty()) {
    return std::chrono::milliseconds(0);
  }

  auto recentTs = perfEvents.events.front().unixTs;
  auto latestTs = perfEvents.events.back().unixTs;
  return std::chrono::milliseconds(latestTs - recentTs);
}

int64_t
generateHash(
    const int64_t version,
    const std::string& originatorId,
    const folly::Optional<std::string>& value) {
  size_t seed = 0;
  boost::hash_combine(seed, version);
  boost::hash_combine(seed, originatorId);
  if (value.hasValue()) {
    boost::hash_combine(seed, value.value());
  }
  return static_cast<int64_t>(seed);
}

std::string
getRemoteIfName(const thrift::Adjacency& adj) {
  if (not adj.otherIfName.empty()) {
    return adj.otherIfName;
  }
  return folly::sformat("neigh-{}", adj.ifName);
}

std::vector<thrift::Path>
getBestPaths(std::vector<thrift::Path> const& paths) {
  // Find minimum cost
  int32_t minCost = std::numeric_limits<int32_t>::max();
  for (auto const& path : paths) {
    minCost = std::min(minCost, path.metric);
  }

  // Find paths with the minimum cost
  std::vector<thrift::Path> ret;
  for (auto const& path : paths) {
    if (path.metric == minCost) {
      ret.push_back(path);
    }
  }

  return ret;
}

std::vector<thrift::UnicastRoute>
createUnicastRoutes(
    const std::vector<thrift::Route>& routes) {
  // Build routes to be programmed.
  std::vector<thrift::UnicastRoute> newRoutes;

  for (auto const& route : routes) {
    std::vector<thrift::BinaryAddress> nexthops;
    for (auto const& path : getBestPaths(route.paths)) {
      nexthops.push_back(path.nextHop);
      auto& nexthop = nexthops.back();
      nexthop.ifName = path.ifName;
    }

    // Create thrift::UnicastRoute object in-place
    newRoutes.emplace_back(
        apache::thrift::FRAGILE, route.prefix, std::move(nexthops));
  } // for ... routes

  return newRoutes;
}

std::pair<std::vector<thrift::UnicastRoute>, std::vector<thrift::IpPrefix>>
findDeltaRoutes(
  const thrift::RouteDatabase& newRouteDb,
  const thrift::RouteDatabase& oldRouteDb) {
  std::pair<
    std::vector<thrift::UnicastRoute>, std::vector<thrift::IpPrefix>> res;

  DCHECK(newRouteDb.thisNodeName == oldRouteDb.thisNodeName);

  // Find new routes to be added/updated/removed
  std::vector<thrift::Route> routesToAddUpdate;
  std::set_difference(
    newRouteDb.routes.begin(), newRouteDb.routes.end(),
    oldRouteDb.routes.begin(), oldRouteDb.routes.end(),
    std::inserter(routesToAddUpdate, routesToAddUpdate.begin()));
  std::vector<thrift::Route> routesToRemoveOrUpdate;
  std::set_difference(
    oldRouteDb.routes.begin(), oldRouteDb.routes.end(),
    newRouteDb.routes.begin(), newRouteDb.routes.end(),
    std::inserter(routesToRemoveOrUpdate, routesToRemoveOrUpdate.begin()));

  // Find entry of prefix to be removed
  std::set<thrift::IpPrefix> prefixesToRemove;
  for (const auto& route : routesToRemoveOrUpdate) {
    prefixesToRemove.emplace(route.prefix);
  }
  for (const auto& route : routesToAddUpdate) {
    prefixesToRemove.erase(route.prefix);
  }

  // Build routes to be programmed.
  res.first = createUnicastRoutes(routesToAddUpdate);
  res.second = {prefixesToRemove.begin(), prefixesToRemove.end()};

  return res;
}

thrift::BuildInfo
getBuildInfoThrift() noexcept {
 return thrift::BuildInfo(
   apache::thrift::FRAGILE,
   BuildInfo::getBuildUser(),
   BuildInfo::getBuildTime(),
   static_cast<int64_t>(BuildInfo::getBuildTimeUnix()),
   BuildInfo::getBuildHost(),
   BuildInfo::getBuildPath(),
   BuildInfo::getBuildRevision(),
   static_cast<int64_t>(BuildInfo::getBuildRevisionCommitTimeUnix()),
   BuildInfo::getBuildUpstreamRevision(),
   BuildInfo::getBuildUpstreamRevisionCommitTimeUnix(),
   BuildInfo::getBuildPackageName(),
   BuildInfo::getBuildPackageVersion(),
   BuildInfo::getBuildPackageRelease(),
   BuildInfo::getBuildPlatform(),
   BuildInfo::getBuildRule(),
   BuildInfo::getBuildType(),
   BuildInfo::getBuildTool(),
   BuildInfo::getBuildMode()
 );
}

folly::IPAddress
toIPAddress(const thrift::fbbinary& binAddr) {
  return folly::IPAddress::fromBinary(folly::ByteRange(
      reinterpret_cast<const uint8_t*>(binAddr.data()), binAddr.size()));
}

} // namespace openr
