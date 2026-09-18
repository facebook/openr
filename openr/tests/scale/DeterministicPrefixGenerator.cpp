/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/DeterministicPrefixGenerator.h>

#include <algorithm>
#include <array>
#include <utility>

#include <fmt/core.h>
#include <folly/Range.h>
#include <folly/ssl/OpenSSLHash.h>

#include <openr/common/NetworkUtil.h>

namespace openr {

namespace {

/*
 * Keeps derived addresses inside fc00::/8, the same ULA convention
 * PrefixGenerator::randIpv6() uses.
 */
constexpr uint8_t kUlaFirstByte = 0xfc;

constexpr size_t kSha256Bytes = 32;
constexpr size_t kIpv6Bytes = 16;

} // namespace

folly::IPAddressV6
DeterministicPrefixGenerator::deriveAddress(
    uint64_t seed, const std::string& nodeName, uint32_t index) {
  /*
   * The exact byte sequence hashed here is the cross-language contract. The
   * separator cannot appear in a node name, so no two distinct inputs can
   * produce the same string.
   */
  const auto input = fmt::format("{}|{}|{}", seed, nodeName, index);

  std::array<uint8_t, kSha256Bytes> digest{};
  folly::ssl::OpenSSLHash::sha256(
      folly::MutableByteRange(digest.data(), digest.size()),
      folly::ByteRange(folly::StringPiece(input)));

  std::array<uint8_t, kIpv6Bytes> addrBytes{};
  addrBytes[0] = kUlaFirstByte;
  std::copy(
      digest.begin(), digest.begin() + (kIpv6Bytes - 1), addrBytes.begin() + 1);

  return folly::IPAddressV6::fromBinary(
      folly::ByteRange(addrBytes.data(), addrBytes.size()));
}

std::vector<thrift::IpPrefix>
DeterministicPrefixGenerator::generate(
    uint64_t seed,
    const std::string& nodeName,
    uint32_t numPrefixes,
    uint8_t bitMaskLen) {
  std::vector<thrift::IpPrefix> ipPrefixes;
  ipPrefixes.reserve(numPrefixes);

  for (uint32_t index = 0; index < numPrefixes; ++index) {
    const auto prefix = deriveAddress(seed, nodeName, index).mask(bitMaskLen);
    ipPrefixes.push_back(toIpPrefix(std::make_pair(prefix, bitMaskLen)));
  }

  return ipPrefixes;
}

} // namespace openr
