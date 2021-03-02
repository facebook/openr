/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/mocks/PrefixGenerator.h>

#include <folly/Random.h>

namespace openr {

folly::IPAddressV6
PrefixGenerator::randIpv6() {
  /* Generate random Ipv6 */
  // Generate random bytes
  std::array<char, 16> randomBytes;
  folly::Random::secureRandom(randomBytes.data(), randomBytes.size());

  // Starting with fc to avoid invalid address
  randomBytes[0] = static_cast<char>(0xfc);

  // Convert bytes to string
  auto randomBytesAsString =
      std::string(randomBytes.data(), randomBytes.size());

  // Return IPV6
  return folly::IPAddressV6::fromBinary(
      folly::StringPiece(randomBytesAsString));
}

std::vector<thrift::IpPrefix>
PrefixGenerator::ipv6PrefixGenerator(
    const uint32_t numOfPrefixes, const uint8_t bitMaskLen) {
  /* Generate random IpPrefixes */
  std::vector<thrift::IpPrefix> ipPrefixes;
  ipPrefixes.reserve(numOfPrefixes);

  for (uint32_t iteration = 0; iteration < numOfPrefixes; iteration++) {
    // Generate a random  IPv6
    auto ipv6Addr = randIpv6();
    // Mask with bitMaskLen
    auto prefix = ipv6Addr.mask(bitMaskLen);
    // Push back into ipPrefixes
    ipPrefixes.push_back(toIpPrefix(std::make_pair(prefix, bitMaskLen)));
  }

  return ipPrefixes;
};

std::vector<thrift::NextHopThrift>
PrefixGenerator::getRandomNextHops(
    const uint8_t numOfNextHops, const std::string& ifname) {
  /* Generate random nextHops. */
  std::vector<thrift::NextHopThrift> nextHops;
  nextHops.reserve(numOfNextHops);

  for (uint32_t index = 0; index < numOfNextHops; index++) {
    // Random local IPV6
    auto ipv6Addr = folly::IPAddress(folly::sformat(
        "fe80::{}", folly::sformat("{:02x}", folly::Random::rand32() >> 16)));

    // Create nexthop
    const auto path = createNextHop(toBinaryAddress(ipv6Addr), ifname, 1);
    nextHops.push_back(path);
  }
  return nextHops;
}

std::vector<thrift::NextHopThrift>
PrefixGenerator::getRandomNextHopsUnicast(
    const uint8_t numOfNextHops, const std::string& ifname) {
  /* Generate random nextHops for one prefix */
  // Random number in [1, 127] of nexthops of one prefix
  auto numOfNextHopsPrefix = folly::Random::rand32() % (numOfNextHops - 1) + 1;
  // Generate random nexthops
  auto nextHops = getRandomNextHops(numOfNextHops, ifname);

  // Resize vector
  nextHops.resize(numOfNextHopsPrefix);

  return nextHops;
}
} // namespace openr
