/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fmt/core.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/Thrift.h>

#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

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
  result.addr()->append(
      reinterpret_cast<const char*>(addr.bytes()), IPAddressVx::byteCount());
  return result;
}

inline thrift::BinaryAddress
toBinaryAddress(const folly::IPAddress& addr) {
  return addr.isV4() ? toBinaryAddressImpl(addr.asV4())
      : addr.isV6()  ? toBinaryAddressImpl(addr.asV6())
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
  return folly::IPAddress::fromBinary(
      folly::ByteRange(
          reinterpret_cast<const uint8_t*>(binAddr.data()), binAddr.size()));
}

inline folly::IPAddress
toIPAddress(const thrift::BinaryAddress& addr) {
  return folly::IPAddress::fromBinary(
      folly::ByteRange(
          reinterpret_cast<const unsigned char*>(addr.addr()->data()),
          addr.addr()->size()));
}

// construct thrift::IpPrefix
inline thrift::IpPrefix
createIpPrefix(
    thrift::BinaryAddress const& prefixAddress, int16_t prefixLength) {
  thrift::IpPrefix ipPrefix;
  ipPrefix.prefixAddress() = prefixAddress;
  ipPrefix.prefixLength() = prefixLength;
  return ipPrefix;
}

inline thrift::IpPrefix
toIpPrefix(const folly::CIDRNetwork& network) {
  return createIpPrefix(toBinaryAddress(network.first), network.second);
}

inline thrift::IpPrefix
toIpPrefix(const std::string& prefix) {
  thrift::IpPrefix ipPrefix;
  try {
    ipPrefix = toIpPrefix(folly::IPAddress::createNetwork(prefix));
  } catch (const folly::IPAddressFormatException& e) {
    throw thrift::OpenrError(
        fmt::format("Invalid IPAddress: {}, exception: {}", prefix, e.what()));
  }
  return ipPrefix;
}

inline std::string
toString(const thrift::BinaryAddress& addr) {
  return addr.addr()->empty() ? "" : toIPAddress(addr).str();
}

inline std::string
toString(const thrift::IpPrefix& ipPrefix) {
  return fmt::format(
      "{}/{}", toString(*ipPrefix.prefixAddress()), *ipPrefix.prefixLength());
}

inline std::string
toString(const thrift::MplsAction& mplsAction) {
  return fmt::format(
      "mpls {} {}{}",
      apache::thrift::util::enumNameSafe(*mplsAction.action()),
      mplsAction.swapLabel() ? std::to_string(*mplsAction.swapLabel()) : "",
      mplsAction.pushLabels() ? folly::join("/", *mplsAction.pushLabels())
                              : "");
}

inline std::string
toString(const thrift::NextHopThrift& nextHop) {
  return fmt::format(
      "via {} dev {} weight {} metric {} area {} {}",
      toIPAddress(*nextHop.address()).str(),
      nextHop.address()->ifName().value_or("N/A"),
      *nextHop.weight(),
      *nextHop.metric(),
      nextHop.area().value_or("N/A"),
      nextHop.mplsAction().has_value() ? toString(nextHop.mplsAction().value())
                                       : "");
}

inline std::string
toString(const folly::IPAddress& addr) {
  return addr.str();
}

inline std::string
toString(const thrift::UnicastRoute& route) {
  std::vector<std::string> lines;
  lines.emplace_back(fmt::format("> Prefix: {}", toString(*route.dest())));
  for (const auto& nh : *route.nextHops()) {
    lines.emplace_back("  " + toString(nh));
  }
  return folly::join("\n", lines);
}

inline std::string
toString(const thrift::MplsRoute& route) {
  std::vector<std::string> lines;
  lines.emplace_back(fmt::format("> Label: {}", *route.topLabel()));
  for (const auto& nh : *route.nextHops()) {
    lines.emplace_back("  " + toString(nh));
  }
  return folly::join("\n", lines);
}

inline folly::CIDRNetwork
toIPNetwork(const thrift::IpPrefix& prefix, bool applyMask = true) {
  folly::CIDRNetwork network;
  try {
    network = folly::IPAddress::createNetwork(
        toIPAddress(*prefix.prefixAddress()).str(),
        *prefix.prefixLength(),
        applyMask);
  } catch (const folly::IPAddressFormatException& e) {
    throw thrift::OpenrError(
        fmt::format(
            "Invalid IPAddress: {}, exception: {}",
            toString(prefix),
            e.what()));
  }
  return network;
}

} // namespace openr
