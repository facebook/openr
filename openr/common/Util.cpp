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

PrefixKey::PrefixKey(
    std::string const& node,
    folly::CIDRNetwork const& prefix,
    const std::string& area)
    : node_(node),
      prefix_(prefix),
      prefixArea_(area),
      prefixKeyString_(folly::sformat(
          "{}{}:{}:[{}/{}]",
          Constants::kPrefixDbMarker.toString(),
          node_,
          prefixArea_,
          prefix_.first.str(),
          prefix_.second)) {}

folly::Expected<PrefixKey, std::string>
PrefixKey::fromStr(const std::string& key) {
  int plen{0};
  std::string area{};
  std::string node{};
  std::string ipstr{};
  folly::CIDRNetwork ipaddress;
  auto patt = RE2::FullMatch(key, getPrefixRE2(), &node, &area, &ipstr, &plen);
  if (!patt) {
    return folly::makeUnexpected(folly::sformat("Invalid key format {}", key));
  }

  try {
    ipaddress =
        folly::IPAddress::createNetwork(folly::sformat("{}/{}", ipstr, plen));
  } catch (const folly::IPAddressFormatException& e) {
    LOG(INFO) << "Exception in converting to Prefix. " << e.what();
    return folly::makeUnexpected(std::string("Invalid IP address in key"));
  }
  return PrefixKey(node, ipaddress, area);
}

std::string
PrefixKey::getNodeName() const {
  return node_;
};

folly::CIDRNetwork
PrefixKey::getCIDRNetwork() const {
  return prefix_;
}

std::string
PrefixKey::getPrefixKey() const {
  return prefixKeyString_;
}

std::string
PrefixKey::getPrefixArea() const {
  return prefixArea_;
}

thrift::IpPrefix
PrefixKey::getIpPrefix() const {
  return toIpPrefix(prefix_);
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
    const std::string& name, std::shared_ptr<const re2::RE2::Set> regexSet) {
  if (not regexSet) {
    return false;
  }

  std::vector<int> matches;
  return regexSet->Match(name, &matches);
}

bool
checkIncludeExcludeRegex(
    const std::string& name,
    std::shared_ptr<const re2::RE2::Set> includeRegexSet,
    std::shared_ptr<const re2::RE2::Set> excludeRegexSet) {
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

void
addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept {
  thrift::PerfEvent event(
      apache::thrift::FRAGILE, nodeName, eventDescr, getUnixTimeStampMs());
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
    CHECK(nextHop.mplsAction_ref().has_value());
    // Action can't be push (we don't push labels in MPLS routes)
    // or POP with multiple nexthops
    CHECK(thrift::MplsActionCode::PUSH != nextHop.mplsAction_ref()->action);
    CHECK(
        thrift::MplsActionCode::POP_AND_LOOKUP !=
        nextHop.mplsAction_ref()->action);

    if (nextHop.metric <= minCost) {
      minCost = nextHop.metric;
      if (nextHop.mplsAction_ref()->action == thrift::MplsActionCode::PHP) {
        mplsActionCode = thrift::MplsActionCode::PHP;
      }
    }
  }

  // Find nextHops with the minimum cost and required mpls action
  std::vector<thrift::NextHopThrift> bestNextHops;
  for (auto const& nextHop : allNextHops) {
    if (nextHop.metric == minCost and
        nextHop.mplsAction_ref()->action == mplsActionCode) {
      bestNextHops.emplace_back(nextHop);
    }
  }

  return bestNextHops;
}

thrift::RouteDatabaseDelta
findDeltaRoutes(
    const thrift::RouteDatabase& newRouteDb,
    const thrift::RouteDatabase& oldRouteDb) {
  DCHECK(newRouteDb.thisNodeName == oldRouteDb.thisNodeName);
  // verify the input is sorted.
  CHECK(
      std::is_sorted(
          newRouteDb.unicastRoutes.begin(), newRouteDb.unicastRoutes.end()) &&
      std::is_sorted(
          oldRouteDb.unicastRoutes.begin(), oldRouteDb.unicastRoutes.end()) &&
      std::is_sorted(
          newRouteDb.mplsRoutes.begin(), newRouteDb.mplsRoutes.end()) &&
      std::is_sorted(
          oldRouteDb.mplsRoutes.begin(), oldRouteDb.mplsRoutes.end()));

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

thrift::PrefixForwardingType
getPrefixForwardingType(const thrift::PrefixEntries& prefixEntries) {
  if (prefixEntries.empty()) {
    return thrift::PrefixForwardingType::IP;
  }

  for (auto const& [_, areaToPrefixEntries] : prefixEntries) {
    for (auto const& [_, prefixEntry] : areaToPrefixEntries) {
      if (prefixEntry.forwardingType == thrift::PrefixForwardingType::IP) {
        return thrift::PrefixForwardingType::IP;
      }
      DCHECK(
          prefixEntry.forwardingType == thrift::PrefixForwardingType::SR_MPLS);
    }
  }
  return thrift::PrefixForwardingType::SR_MPLS;
}

thrift::PrefixForwardingAlgorithm
getPrefixForwardingAlgorithm(const thrift::PrefixEntries& prefixEntries) {
  if (prefixEntries.empty()) {
    return thrift::PrefixForwardingAlgorithm::SP_ECMP;
  }

  for (auto const& [_, areaToPrefixEntries] : prefixEntries) {
    for (auto const& [_, prefixEntry] : areaToPrefixEntries) {
      if (prefixEntry.forwardingAlgorithm ==
          thrift::PrefixForwardingAlgorithm::SP_ECMP) {
        return thrift::PrefixForwardingAlgorithm::SP_ECMP;
      }
      DCHECK(
          prefixEntry.forwardingAlgorithm ==
          thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP);
    }
  }
  return thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
}

void
checkMplsAction(thrift::MplsAction const& mplsAction) {
  switch (mplsAction.action) {
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
    bool supportFloodOptimization) {
  thrift::PeerSpec peerSpec;
  peerSpec.cmdUrl = cmdUrl;
  peerSpec.peerAddr = peerAddr;
  peerSpec.ctrlPort = port;
  peerSpec.supportFloodOptimization = supportFloodOptimization;
  return peerSpec;
}

thrift::SparkNeighborEvent
createSparkNeighborEvent(
    thrift::SparkNeighborEventType eventType,
    const std::string& ifName,
    const thrift::SparkNeighbor& originator,
    int64_t rttUs,
    int32_t label,
    bool supportFloodOptimization,
    const std::string& area) {
  thrift::SparkNeighborEvent event;
  event.eventType = eventType;
  event.ifName = ifName;
  event.neighbor = originator;
  event.rttUs = rttUs;
  event.label = label;
  event.supportFloodOptimization = supportFloodOptimization;
  event.area = area;
  return event;
}

thrift::SparkNeighbor
createSparkNeighbor(
    const std::string& nodeName,
    const thrift::BinaryAddress& v4Addr,
    const thrift::BinaryAddress& v6Addr,
    int64_t kvStoreCmdPort,
    int64_t openrCtrlThriftPort,
    const std::string& ifName) {
  thrift::SparkNeighbor neighbor;
  neighbor.nodeName = nodeName;
  neighbor.transportAddressV4 = v4Addr;
  neighbor.transportAddressV6 = v6Addr;
  neighbor.kvStoreCmdPort = kvStoreCmdPort;
  neighbor.openrCtrlThriftPort = openrCtrlThriftPort;
  neighbor.ifName = ifName;
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
    const std::string& remoteIfName) {
  thrift::Adjacency adj;
  adj.otherNodeName = nodeName;
  adj.ifName = ifName;
  adj.nextHopV6 = toBinaryAddress(folly::IPAddress(nextHopV6));
  adj.nextHopV4 = toBinaryAddress(folly::IPAddress(nextHopV4));
  adj.metric = metric;
  adj.adjLabel = adjLabel;
  adj.isOverloaded = isOverloaded;
  adj.rtt = rtt;
  adj.timestamp = timestamp;
  adj.weight = weight;
  adj.otherIfName = remoteIfName;
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
    int64_t weight) {
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
      remoteIfName);
}

thrift::AdjacencyDatabase
createAdjDb(
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    int32_t nodeLabel,
    bool overLoadBit,
    const std::string& area) {
  thrift::AdjacencyDatabase adjDb;
  adjDb.thisNodeName = nodeName;
  adjDb.isOverloaded = overLoadBit;
  adjDb.adjacencies = adjs;
  adjDb.nodeLabel = nodeLabel;
  adjDb.area = area;
  return adjDb;
}

thrift::PrefixDatabase
createPrefixDb(
    const std::string& nodeName,
    const std::vector<thrift::PrefixEntry>& prefixEntries,
    const std::string& area) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName = nodeName;
  prefixDb.prefixEntries = prefixEntries;
  prefixDb.area_ref() = area;
  return prefixDb;
}

thrift::PrefixEntry
createPrefixEntry(
    thrift::IpPrefix prefix,
    thrift::PrefixType type,
    const std::string& data,
    thrift::PrefixForwardingType forwardingType,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    std::optional<bool> ephemeral,
    std::optional<thrift::MetricVector> mv,
    std::optional<int64_t> minNexthop) {
  thrift::PrefixEntry prefixEntry;
  prefixEntry.prefix = prefix;
  prefixEntry.type = type;
  if (not data.empty()) {
    prefixEntry.data_ref() = data;
  }
  prefixEntry.forwardingType = forwardingType;
  prefixEntry.forwardingAlgorithm = forwardingAlgorithm;
  fromStdOptional(prefixEntry.ephemeral_ref(), ephemeral);
  fromStdOptional(prefixEntry.mv_ref(), mv);
  fromStdOptional(prefixEntry.minNexthop_ref(), minNexthop);
  return prefixEntry;
}

thrift::Value
createThriftValue(
    int64_t version,
    std::string originatorId,
    std::optional<std::string> data,
    int64_t ttl,
    int64_t ttlVersion,
    std::optional<int64_t> hash) {
  thrift::Value value;
  value.version = version;
  value.originatorId = originatorId;
  fromStdOptional(value.value_ref(), data);
  value.ttl = ttl;
  value.ttlVersion = ttlVersion;
  if (hash.has_value()) {
    fromStdOptional(value.hash_ref(), hash);
  } else {
    value.hash_ref() = generateHash(version, originatorId, data);
  }

  return value;
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
  pub.keyVals = kv;
  pub.expiredKeys = expiredKeys;
  fromStdOptional(pub.nodeIds_ref(), nodeIds);
  fromStdOptional(pub.tobeUpdatedKeys_ref(), keysToUpdate);
  fromStdOptional(pub.floodRootId_ref(), floodRootId);
  pub.area = area;
  return pub;
}

thrift::InterfaceInfo
createThriftInterfaceInfo(
    const bool isUp,
    const int ifIndex,
    const std::vector<thrift::IpPrefix>& networks) {
  thrift::InterfaceInfo interfaceInfo;
  interfaceInfo.isUp = isUp;
  interfaceInfo.ifIndex = ifIndex;
  interfaceInfo.networks = networks;
  return interfaceInfo;
}

thrift::NextHopThrift
createNextHop(
    thrift::BinaryAddress addr,
    std::optional<std::string> ifName,
    int32_t metric,
    std::optional<thrift::MplsAction> maybeMplsAction,
    bool useNonShortestRoute,
    const std::string& area) {
  thrift::NextHopThrift nextHop;
  nextHop.address = addr;
  fromStdOptional(nextHop.address.ifName_ref(), std::move(ifName));
  nextHop.metric = metric;
  fromStdOptional(nextHop.mplsAction_ref(), maybeMplsAction);
  nextHop.useNonShortestRoute = useNonShortestRoute;
  nextHop.area_ref() = area;
  return nextHop;
}

thrift::MplsAction
createMplsAction(
    thrift::MplsActionCode const mplsActionCode,
    std::optional<int32_t> maybeSwapLabel,
    std::optional<std::vector<int32_t>> maybePushLabels) {
  thrift::MplsAction mplsAction;
  mplsAction.action = mplsActionCode;
  fromStdOptional(mplsAction.swapLabel_ref(), maybeSwapLabel);
  fromStdOptional(mplsAction.pushLabels_ref(), maybePushLabels);
  checkMplsAction(mplsAction); // sanity checks
  return mplsAction;
}

thrift::PrefixEntry
createBgpWithdrawEntry(const thrift::IpPrefix& prefix) {
  thrift::PrefixEntry pfx;
  pfx.type = thrift::PrefixType::BGP;
  pfx.prefix = prefix;
  return pfx;
}

thrift::UnicastRoute
createUnicastRoute(
    thrift::IpPrefix dest, std::vector<thrift::NextHopThrift> nextHops) {
  thrift::UnicastRoute unicastRoute;
  unicastRoute.dest = std::move(dest);
  std::sort(nextHops.begin(), nextHops.end());
  unicastRoute.nextHops = std::move(nextHops);
  return unicastRoute;
}

thrift::MplsRoute
createMplsRoute(int32_t topLabel, std::vector<thrift::NextHopThrift> nextHops) {
  // Sanity checks
  CHECK(isMplsLabelValid(topLabel));
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.mplsAction_ref().has_value());
  }

  thrift::MplsRoute mplsRoute;
  mplsRoute.topLabel = topLabel;
  std::sort(nextHops.begin(), nextHops.end());
  mplsRoute.nextHops = std::move(nextHops);
  return mplsRoute;
}

std::vector<thrift::UnicastRoute>
createUnicastRoutesWithBestNexthops(
    const std::vector<thrift::UnicastRoute>& routes) {
  // Build routes to be programmed
  std::vector<thrift::UnicastRoute> newRoutes;

  for (auto const& route : routes) {
    auto newRoute =
        createUnicastRoute(route.dest, getBestNextHopsUnicast(route.nextHops));
    newRoutes.emplace_back(std::move(newRoute));
  }

  return newRoutes;
}

std::vector<thrift::MplsRoute>
createMplsRoutesWithBestNextHops(const std::vector<thrift::MplsRoute>& routes) {
  // Build routes to be programmed
  std::vector<thrift::MplsRoute> newRoutes;

  for (auto const& route : routes) {
    newRoutes.emplace_back(
        createMplsRoute(route.topLabel, getBestNextHopsMpls(route.nextHops)));
  }

  return newRoutes;
}

std::vector<thrift::UnicastRoute>
createUnicastRoutesWithBestNextHopsMap(
    const std::unordered_map<thrift::IpPrefix, thrift::UnicastRoute>&
        unicastRoutes) {
  // Build routes to be programmed
  std::vector<thrift::UnicastRoute> newRoutes;

  for (auto const& route : unicastRoutes) {
    auto newRoute = createUnicastRoute(
        route.first, getBestNextHopsUnicast(route.second.nextHops));
    newRoutes.emplace_back(std::move(newRoute));
  }

  return newRoutes;
}

std::vector<thrift::MplsRoute>
createMplsRoutesWithBestNextHopsMap(
    const std::unordered_map<uint32_t, thrift::MplsRoute>& mplsRoutes) {
  // Build routes to be programmed
  std::vector<thrift::MplsRoute> newRoutes;

  for (auto const& route : mplsRoutes) {
    newRoutes.emplace_back(createMplsRoute(
        route.first, getBestNextHopsMpls(route.second.nextHops)));
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

std::string
createPeerSyncId(const std::string& node, const std::string& area) {
  return folly::to<std::string>(node, "::TCP::SYNC::", area);
};

namespace MetricVectorUtils {

std::optional<const openr::thrift::MetricEntity>
getMetricEntityByType(const openr::thrift::MetricVector& mv, int64_t type) {
  for (auto& me : mv.metrics) {
    if (me.type == type) {
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

  me.type = type;
  me.priority = priority;
  me.op = op;
  me.isBestPathTieBreaker = isBestPathTieBreaker;
  me.metric = metric;

  return me;
}

CompareResult operator!(CompareResult mv) {
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
  for (auto const& ent : mv.metrics) {
    if (ent.priority > priorPriority) {
      return false;
    }
    priorPriority = ent.priority;
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
      const_cast<std::vector<thrift::MetricEntity>&>(mv.metrics);
  std::sort(
      metrics.begin(),
      metrics.end(),
      [](thrift::MetricEntity& l, thrift::MetricEntity& r) {
        return l.priority > r.priority;
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
  if (thrift::CompareType::WIN_IF_PRESENT == entity.op) {
    return entity.isBestPathTieBreaker ? CompareResult::TIE_WINNER
                                       : CompareResult::WINNER;
  } else if (thrift::CompareType::WIN_IF_NOT_PRESENT == entity.op) {
    return entity.isBestPathTieBreaker ? CompareResult::TIE_LOOSER
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

  if (l.version != r.version) {
    return CompareResult::ERROR;
  }

  sortMetricVector(l);
  sortMetricVector(r);

  auto lIter = l.metrics.begin();
  auto rIter = r.metrics.begin();
  while (!isDecisive(result) &&
         (lIter != l.metrics.end() && rIter != r.metrics.end())) {
    if (lIter->type == rIter->type) {
      if (lIter->isBestPathTieBreaker != rIter->isBestPathTieBreaker) {
        maybeUpdate(result, CompareResult::ERROR);
      } else {
        maybeUpdate(
            result,
            compareMetrics(
                lIter->metric, rIter->metric, lIter->isBestPathTieBreaker));
      }
      ++lIter;
      ++rIter;
    } else if (lIter->priority > rIter->priority) {
      maybeUpdate(result, resultForLoner(*lIter));
      ++lIter;
    } else if (lIter->priority < rIter->priority) {
      maybeUpdate(result, !resultForLoner(*rIter));
      ++rIter;
    } else {
      // priorities are the same but types are different
      maybeUpdate(result, CompareResult::ERROR);
    }
  }
  while (!isDecisive(result) && lIter != l.metrics.end()) {
    maybeUpdate(result, resultForLoner(*lIter));
    ++lIter;
  }
  while (!isDecisive(result) && rIter != r.metrics.end()) {
    maybeUpdate(result, !resultForLoner(*rIter));
    ++rIter;
  }
  return result;
}

} // namespace MetricVectorUtils

} // namespace openr
