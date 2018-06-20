/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <thrift/lib/cpp2/Thrift.h>

#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/IpPrefix_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace openr {

template <class IPAddressVx>
thrift::BinaryAddress
toBinaryAddressImpl(const IPAddressVx& addr) {
  thrift::BinaryAddress result;
  result.addr.append(
      reinterpret_cast<const char*>(addr.bytes()), IPAddressVx::byteCount());
  return result;
}

inline thrift::BinaryAddress
toBinaryAddress(const folly::IPAddress& addr) {
  return addr.isV4() ? toBinaryAddressImpl(addr.asV4())
                     : addr.isV6() ? toBinaryAddressImpl(addr.asV6())
                                   : thrift::BinaryAddress();
}

inline thrift::BinaryAddress
toBinaryAddress(const std::string& addr) {
  return toBinaryAddress(folly::IPAddress(addr));
}

template <typename T>
inline folly::IPAddress
toIPAddress(const T& input) {
  return input.type != decltype(input.type)::VUNSPEC
      ? folly::IPAddress(input.addr)
      : folly::IPAddress();
}

inline folly::IPAddress
toIPAddress(const thrift::BinaryAddress& addr) {
  return folly::IPAddress::fromBinary(folly::ByteRange(
      reinterpret_cast<const unsigned char*>(addr.addr.data()),
      addr.addr.size()));
}

inline folly::CIDRNetwork
toIPNetwork(const thrift::IpPrefix& prefix, bool applyMask = true) {
  return folly::IPAddress::createNetwork(
      toIPAddress(prefix.prefixAddress).str(), prefix.prefixLength, applyMask);
}

inline thrift::IpPrefix
toIpPrefix(const folly::CIDRNetwork& network) {
  return thrift::IpPrefix(
      apache::thrift::FRAGILE, toBinaryAddress(network.first), network.second);
}

inline thrift::IpPrefix
toIpPrefix(const std::string& prefix) {
  return toIpPrefix(folly::IPAddress::createNetwork(prefix));
}

inline std::string
toString(const thrift::BinaryAddress& addr) {
  return toIPAddress(addr).str();
}

inline std::string
toString(const thrift::IpPrefix& ipPrefix) {
  return folly::sformat(
      "{}/{}", toString(ipPrefix.prefixAddress), ipPrefix.prefixLength);
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

inline thrift::Path
createPath(
    thrift::BinaryAddress addr, const std::string& ifName, int32_t metric) {
  thrift::Path path;
  path.nextHop = addr;
  path.ifName = ifName;
  path.metric = metric;
  return path;
}

} // namespace openr
