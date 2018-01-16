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

namespace {
// number of bits in ip v6
const uint8_t kBitCount = 128;
} // namespace

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

} // namespace std

namespace openr {

// need operator< for creating std::set of these thrift types. we need ordered
// set for set algebra. thrift declares but does not define these for us.

bool
thrift::BinaryAddress::operator<(const thrift::BinaryAddress& other) const {
  if (addr != other.addr) {
    return addr < other.addr;
  }
  if (ifName != other.ifName) {
    return ifName < other.ifName;
  }
  return port < other.port;
}

bool
thrift::IpPrefix::operator<(const thrift::IpPrefix& other) const {
  if (prefixAddress != other.prefixAddress) {
    return prefixAddress < other.prefixAddress;
  }
  return prefixLength < other.prefixLength;
}

bool
thrift::UnicastRoute::operator<(const thrift::UnicastRoute& other) const {
  if (dest != other.dest) {
    return dest < other.dest;
  }
  auto myNhs = nexthops;
  auto otherNhs = other.nexthops;
  std::sort(myNhs.begin(), myNhs.end());
  std::sort(otherNhs.begin(), otherNhs.end());
  return myNhs < otherNhs;
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
    const std::vector<std::regex>& includeRegexList,
    const std::vector<std::regex>& excludeRegexList) {
  for (const auto& regex : excludeRegexList) {
    if (std::regex_match(name, regex)) {
      return false;
    }
  }
  for (const auto& regex : includeRegexList) {
    if (std::regex_match(name, regex)) {
      return true;
    }
  }
  return false;
}

std::vector<std::string>
splitByComma(const std::string& input) {
  std::vector<std::string> output;
  folly::split(",", input, output);

  return output;
}

bool
flushIfaceAddrs(
    const std::string& ifName,
    const folly::CIDRNetwork& prefix,
    bool flushAllGlobalAddrs) {
  std::string command;
  if (flushAllGlobalAddrs) {
    command = folly::sformat(
        "ip -{} addr flush dev {} scope global",
        prefix.first.version(),
        ifName);
  } else {
    command = folly::sformat(
        "ip -{} addr flush dev {} to {}",
        prefix.first.version(),
        ifName,
        folly::IPAddress::networkToString(prefix));
  }
  return executeShellCommand(command) == 0;
}

bool
addIfaceAddr(const std::string& ifName, const folly::IPAddress& addr) {
  auto command = folly::sformat(
      "ip -{} addr add {} dev {}", addr.version(), addr.str(), ifName);
  return executeShellCommand(command) == 0;
}

folly::IPAddress
createLoopbackAddr(const folly::CIDRNetwork& prefix) noexcept {
  CHECK(prefix.first.isV6()) << "V4 is not supported";
  auto addr = prefix.first.mask(prefix.second);

  // Set last bit to `1` if prefix length is less than 128
  if (prefix.second < 128) {
    auto bytes = addr.asV6().toByteArray();
    bytes[15]++;
    addr = folly::IPAddressV6(bytes);
  }

  return addr;
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
getIfacePrefixes(std::string ifName) {
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
    if (::strcmp(ifName.c_str(), ifa->ifa_name) || ifa->ifa_addr == nullptr) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET6) {
      ::memcpy(
          &ip6,
          &((struct sockaddr_in6*)ifa->ifa_addr)->sin6_addr,
          sizeof(in6_addr));
      prefixLength = maskToPrefixLen((struct sockaddr_in6*)ifa->ifa_netmask);
      results.emplace_back(
          folly::CIDRNetwork{folly::IPAddressV6(ip6), prefixLength});
    }
    if (ifa->ifa_addr->sa_family == AF_INET) {
      struct in_addr ip;
      ::memcpy(
          &ip,
          &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr,
          sizeof(in_addr));
      prefixLength = maskToPrefixLen((struct sockaddr_in*)ifa->ifa_netmask);
      results.emplace_back(
          folly::CIDRNetwork{folly::IPAddressV4(ip), prefixLength});
    }
  }
  return results;
}

folly::CIDRNetwork
getNthPrefix(
    const folly::CIDRNetwork& seedPrefix,
    uint32_t allocPrefixLen,
    uint32_t prefixIndex,
    bool mask) {
  // get underlying byte array representing IP
  auto seedIp = seedPrefix.first;
  auto seedIpV6 = seedIp.asV6();
  auto ipBytes = seedIpV6.toByteArray();

  // host number bit length
  // in seed prefix
  auto seedHostBitLen = kBitCount - seedPrefix.second;
  // in allocated prefix
  auto allocHostBitLen = kBitCount - allocPrefixLen;

  // using bits (seedHostBitLen-allocHostBitLen-1)..0 of @prefixIndex to
  // set bits (seedHostBitLen - 1)..allocHostBitLen of ipBytes
  for (uint8_t i = 0; i < seedHostBitLen - allocHostBitLen; ++i) {
    // global bit index across bytes
    auto idx = i + allocHostBitLen;
    // byte index: network byte order, i.e., big-endian
    auto byteIdx = kBitCount / 8 - idx / 8 - 1;
    // bit index inside the byte
    auto bitIdx = idx % 8;

    if (prefixIndex & (0x1 << i)) {
      // set
      ipBytes[byteIdx] |= (0x1 << bitIdx);
    } else {
      // clear
      ipBytes[byteIdx] &= ~(0x1 << bitIdx);
    }
  }

  // convert back to CIDR
  folly::IPAddress allocPrefixIp{folly::IPAddressV6{ipBytes}};
  return {mask ? allocPrefixIp.mask(allocPrefixLen) : allocPrefixIp,
          allocPrefixLen};
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
} // namespace openr
