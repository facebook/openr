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
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>

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
 * Make MplsAction hashable
 */
template <>
struct hash<openr::thrift::MplsAction> {
  size_t operator()(openr::thrift::MplsAction const&) const;
};

/**
 * Make NextHopThrift hashable
 */
template <>
struct hash<openr::thrift::NextHopThrift> {
  size_t operator()(openr::thrift::NextHopThrift const&) const;
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
toIPAddress(const std::string& binAddr) {
  return folly::IPAddress::fromBinary(folly::ByteRange(
      reinterpret_cast<const uint8_t*>(binAddr.data()), binAddr.size()));
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

inline std::string
toString(const thrift::NextHopThrift& nextHop) {
  return folly::sformat(
      "via {} dev {} weight {}",
      toIPAddress(nextHop.address).str(),
      nextHop.address.ifName.value_or("N/A"),
      nextHop.weight);
}

} // namespace openr
