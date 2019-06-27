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
#include <sys/stat.h>
#include <unistd.h>

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

  for (auto const& keyPrefix : keyPrefixList) {
    if (keyPrefix_->Add(keyPrefix, &re2AddError) < 0) {
      LOG(FATAL) << "Failed to add prefixes to RE2 set: '" << keyPrefix << "', "
                 << "error: '" << re2AddError << "'";
      return;
    }
  }
  if (!keyPrefix_->Compile()) {
    LOG(FATAL) << "Failed to compile re2 set";
    keyPrefix_.reset();
  }
}

// match the key with the list of prefixes
bool
KeyPrefix::keyMatch(std::string const& key) const {
  if (!keyPrefix_) {
    return true;
  }
  std::vector<int> matches;
  return keyPrefix_->Match(key, &matches);
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
matchRegexSet(
    const std::string& name, const std::unique_ptr<re2::RE2::Set>& regexSet) {
  if (!regexSet) {
    return false;
  }

  std::vector<int> matches;
  return regexSet->Match(name, &matches);
}

bool
checkIncludeExcludeRegex(
    const std::string& name,
    const std::unique_ptr<re2::RE2::Set>& includeRegexSet,
    const std::unique_ptr<re2::RE2::Set>& excludeRegexSet) {
  return (
      not matchRegexSet(name, excludeRegexSet) and
      matchRegexSet(name, includeRegexSet));
}

std::vector<std::string>
splitByComma(const std::string& input) {
  std::vector<std::string> output;
  folly::split(",", input, output);

  return output;
}

// TODO replace with `std::filesystem::exists(...) once transitioned to cpp17
bool
fileExists(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  SCOPE_EXIT {
    ::close(fd);
  };
  return 0 <= fd;
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
    if (::strcmp(ifName.c_str(), ifa->ifa_name) || ifa->ifa_addr == nullptr ||
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
      results.emplace_back(folly::CIDRNetwork{ifaceAddr, prefixLength});
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
      results.emplace_back(folly::CIDRNetwork{ifaceAddr, prefixLength});
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

folly::Expected<std::chrono::milliseconds, std::string>
getDurationBetweenPerfEvents(
    const thrift::PerfEvents& perfEvents,
    const std::string& firstName,
    const std::string& secondName) noexcept {
  auto search = std::find_if(
      perfEvents.events.begin(),
      perfEvents.events.end(),
      [&firstName](const thrift::PerfEvent& event) {
        return event.eventDescr == firstName;
      });
  if (search == perfEvents.events.end()) {
    return folly::makeUnexpected(
        folly::sformat("Could not find first event: {}", firstName));
  }
  int64_t first = search->unixTs;
  search = std::find_if(
      search + 1,
      perfEvents.events.end(),
      [&secondName](const thrift::PerfEvent& event) {
        return event.eventDescr == secondName;
      });
  if (search == perfEvents.events.end()) {
    return folly::makeUnexpected(
        folly::sformat("Could not find second event: {}", secondName));
  }
  int64_t second = search->unixTs;
  if (second < first) {
    return folly::makeUnexpected(
        std::string{"Negative duration between first and second event"});
  }
  return std::chrono::milliseconds(second - first);
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

std::vector<thrift::NextHopThrift>
getBestNextHopsUnicast(std::vector<thrift::NextHopThrift> const& allNextHops) {
  // Optimization
  if (allNextHops.size() <= 1) {
    return allNextHops;
  }
  // Find minimum cost
  int32_t minCost = std::numeric_limits<int32_t>::max();
  for (auto const& nextHop : allNextHops) {
    minCost = std::min(minCost, nextHop.metric);
  }

  // Find nextHops with the minimum cost
  std::vector<thrift::NextHopThrift> bestNextHops;
  for (auto const& nextHop : allNextHops) {
    if (nextHop.metric == minCost or nextHop.useNonShortestRoute) {
      bestNextHops.emplace_back(nextHop);
    }
  }

  return bestNextHops;
}

std::vector<thrift::NextHopThrift>
getBestNextHopsMpls(std::vector<thrift::NextHopThrift> const& allNextHops) {
  // Optimization for single nexthop case
  if (allNextHops.size() <= 1) {
    return allNextHops;
  }
  // Find minimum cost and mpls action
  int32_t minCost = std::numeric_limits<int32_t>::max();
  thrift::MplsActionCode mplsActionCode{thrift::MplsActionCode::SWAP};
  for (auto const& nextHop : allNextHops) {
    CHECK(nextHop.mplsAction.hasValue());
    // Action can't be push (we don't push labels in MPLS routes)
    // or POP with multiple nexthops
    CHECK(thrift::MplsActionCode::PUSH != nextHop.mplsAction->action);
    CHECK(thrift::MplsActionCode::POP_AND_LOOKUP != nextHop.mplsAction->action);

    if (nextHop.metric <= minCost) {
      minCost = nextHop.metric;
      if (nextHop.mplsAction->action == thrift::MplsActionCode::PHP) {
        mplsActionCode = thrift::MplsActionCode::PHP;
      }
    }
  }

  // Find nextHops with the minimum cost and required mpls action
  std::vector<thrift::NextHopThrift> bestNextHops;
  for (auto const& nextHop : allNextHops) {
    if (nextHop.metric == minCost and
        nextHop.mplsAction->action == mplsActionCode) {
      bestNextHops.emplace_back(nextHop);
    }
  }

  return bestNextHops;
}

std::vector<thrift::BinaryAddress>
createDeprecatedNexthops(const std::vector<thrift::NextHopThrift>& nextHops) {
  std::vector<thrift::BinaryAddress> deprecatedNexthops;
  for (auto const& nextHop : nextHops) {
    deprecatedNexthops.emplace_back(nextHop.address);
  }
  return deprecatedNexthops;
}

thrift::RouteDatabaseDelta
findDeltaRoutes(
    const thrift::RouteDatabase& newRouteDb,
    const thrift::RouteDatabase& oldRouteDb) {
  DCHECK(newRouteDb.thisNodeName == oldRouteDb.thisNodeName);

  // Find unicast routes to be added/updated or removed
  std::vector<thrift::UnicastRoute> unicastRoutesToUpdate;
  std::set_difference(
      newRouteDb.unicastRoutes.begin(),
      newRouteDb.unicastRoutes.end(),
      oldRouteDb.unicastRoutes.begin(),
      oldRouteDb.unicastRoutes.end(),
      std::inserter(unicastRoutesToUpdate, unicastRoutesToUpdate.begin()));
  std::vector<thrift::UnicastRoute> unicastRoutesToDelete;
  std::set_difference(
      oldRouteDb.unicastRoutes.begin(),
      oldRouteDb.unicastRoutes.end(),
      newRouteDb.unicastRoutes.begin(),
      newRouteDb.unicastRoutes.end(),
      std::inserter(unicastRoutesToDelete, unicastRoutesToDelete.begin()));

  // Find mpls routes to be added/updated or removed
  std::vector<thrift::MplsRoute> mplsRoutesToUpdate;
  std::set_difference(
      newRouteDb.mplsRoutes.begin(),
      newRouteDb.mplsRoutes.end(),
      oldRouteDb.mplsRoutes.begin(),
      oldRouteDb.mplsRoutes.end(),
      std::inserter(mplsRoutesToUpdate, mplsRoutesToUpdate.begin()));
  std::vector<thrift::MplsRoute> mplsRoutesToDelete;
  std::set_difference(
      oldRouteDb.mplsRoutes.begin(),
      oldRouteDb.mplsRoutes.end(),
      newRouteDb.mplsRoutes.begin(),
      newRouteDb.mplsRoutes.end(),
      std::inserter(mplsRoutesToDelete, mplsRoutesToDelete.begin()));

  // Find entry of prefix to be removed
  std::set<thrift::IpPrefix> prefixesToRemove;
  for (const auto& route : unicastRoutesToDelete) {
    prefixesToRemove.emplace(route.dest);
  }
  for (const auto& route : unicastRoutesToUpdate) {
    prefixesToRemove.erase(route.dest);
  }

  // Find labels to be removed
  std::set<int32_t> labelsToRemove;
  for (const auto& route : mplsRoutesToDelete) {
    labelsToRemove.emplace(route.topLabel);
  }
  for (const auto& route : mplsRoutesToUpdate) {
    labelsToRemove.erase(route.topLabel);
  }

  // Build routes to be programmed.
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.thisNodeName = newRouteDb.thisNodeName;
  routeDbDelta.unicastRoutesToUpdate = std::move(unicastRoutesToUpdate);
  routeDbDelta.unicastRoutesToDelete = {prefixesToRemove.begin(),
                                        prefixesToRemove.end()};
  routeDbDelta.mplsRoutesToUpdate = std::move(mplsRoutesToUpdate);
  routeDbDelta.mplsRoutesToDelete = {labelsToRemove.begin(),
                                     labelsToRemove.end()};

  return routeDbDelta;
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
      BuildInfo::getBuildMode());
}

folly::Optional<std::string>
maybeGetTcpEndpoint(const std::string& addr, const int32_t port) {
  return (-1 == port)
      ? folly::none
      : folly::Optional<std::string>{folly::sformat("tcp://{}:{}", addr, port)};
}

thrift::PrefixForwardingType
getPrefixForwardingType(
    const std::unordered_map<std::string, thrift::PrefixEntry>& nodePrefixes) {
  for (auto const& kv : nodePrefixes) {
    if (kv.second.forwardingType == thrift::PrefixForwardingType::IP) {
      return thrift::PrefixForwardingType::IP;
    }
    DCHECK(kv.second.forwardingType == thrift::PrefixForwardingType::SR_MPLS);
  }
  return thrift::PrefixForwardingType::SR_MPLS;
}

std::unique_ptr<openr::thrift::OpenrCtrlAsyncClient>
getOpenrCtrlClient(const std::string& ipAddr, folly::EventBase& evb) {
  LOG(INFO) << "Create new openr thrift client";

  std::unique_ptr<openr::thrift::OpenrCtrlAsyncClient> client = nullptr;
  try {
    auto socket = apache::thrift::async::TAsyncSocket::newSocket(
        &evb, ipAddr, openr::Constants::kOpenrCtrlPort);
    auto channel = apache::thrift::HeaderClientChannel::newChannel(socket);
    client = std::make_unique<openr::thrift::OpenrCtrlAsyncClient>(
        std::move(channel));
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to create openr thrift client. Exception: "
               << ex.what();
  }
  return client;
}

} // namespace openr
