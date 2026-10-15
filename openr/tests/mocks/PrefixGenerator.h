/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>

#pragma once

#include <folly/IPAddressV4.h>

namespace openr {
class PrefixGenerator {
 public:
  // Generate random IPv4 IpPrefixes.
  static std::vector<thrift::IpPrefix> ipv4PrefixGenerator(
      const uint32_t numOfPrefixes, const uint8_t bitMaskLen);

  // Generate random IpPrefixes
  static std::vector<thrift::IpPrefix> ipv6PrefixGenerator(
      const uint32_t numOfPrefixes, const uint8_t bitMaskLen);

  // Generate random nextHops for one prefix
  static std::vector<thrift::NextHopThrift> getRandomNextHopsUnicast(
      const uint8_t numOfNextHops, const std::string& ifname);

 private:
  // Generate random IPv4 in 10.0.0.0/8.
  static folly::IPAddressV4 randIpv4();

  // Generate random Ipv6
  static folly::IPAddressV6 randIpv6();

  // Generate all nextHops.
  static std::vector<thrift::NextHopThrift> getRandomNextHops(
      const uint8_t numOfNextHops, const std::string& ifname);
};
} // namespace openr
