/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/DeterministicPrefixGenerator.h>

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <folly/Conv.h>
#include <folly/IPAddressV6.h>
#include <folly/String.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>

#include <openr/common/LsdbTypes.h>
#include <openr/common/NetworkUtil.h>

#include "tools/cxx/Resources.h"

using namespace openr;

namespace {

/*
 * The golden vectors are the anti-drift guard between this generator and its
 * Python mirror (openr/tests/scale/scripts/scale_key_names.py). Both languages
 * assert the same file, so if either implementation changes its output its own
 * test fails here.
 */
const std::string kGoldenResource =
    "openr/tests/scale/tests/testdata/seeded_prefix_golden.json";

constexpr uint8_t kHostMaskLen = 128;
constexpr uint8_t kUlaFirstByte = 0xfc;

folly::dynamic
loadGolden() {
  return folly::parseJson(build::getResourceAsString(kGoldenResource));
}

/*
 * The key layout is owned by PrefixKey, so the golden keys are asserted against
 * the production formatter rather than against a copy of its format string.
 */
std::string
prefixKeyFor(
    uint64_t seed,
    const std::string& nodeName,
    uint32_t index,
    uint8_t bitMaskLen) {
  const auto prefix =
      DeterministicPrefixGenerator::deriveAddress(seed, nodeName, index)
          .mask(bitMaskLen);
  return PrefixKey(
             nodeName,
             folly::CIDRNetwork(folly::IPAddress(prefix), bitMaskLen),
             Constants::kDefaultArea.toString())
      .getPrefixKeyV2();
}

std::set<std::string>
addressSet(uint64_t seed, const std::string& nodeName, uint32_t numPrefixes) {
  std::set<std::string> addresses;
  for (const auto& ipPrefix : DeterministicPrefixGenerator::generate(
           seed, nodeName, numPrefixes, kHostMaskLen)) {
    addresses.insert(toString(ipPrefix));
  }
  return addresses;
}

} // namespace

/*
 * Every checked-in (seed, node, index) vector must reproduce byte-for-byte,
 * both as an address and as the resulting KvStore key name.
 */
TEST(DeterministicPrefixGeneratorTest, ReproducesGoldenDerivationVectors) {
  const auto golden = loadGolden();
  ASSERT_FALSE(golden["derivation"].empty());

  for (const auto& entry : golden["derivation"]) {
    const auto seed = folly::to<uint64_t>(entry["seed"].asString());
    const auto nodeName = entry["node"].asString();
    const auto index = folly::to<uint32_t>(entry["index"].asInt());
    const auto bitMaskLen = folly::to<uint8_t>(entry["bitMaskLen"].asInt());
    const auto why = entry["why"].asString();

    const auto address =
        DeterministicPrefixGenerator::deriveAddress(seed, nodeName, index)
            .mask(bitMaskLen);
    EXPECT_EQ(entry["address"].asString(), address.str())
        << "seed=" << seed << " node=" << nodeName << " index=" << index << " /"
        << static_cast<int>(bitMaskLen) << ": " << why;
    EXPECT_EQ(
        entry["key"].asString(),
        prefixKeyFor(seed, nodeName, index, bitMaskLen))
        << "seed=" << seed << " node=" << nodeName << " index=" << index << " /"
        << static_cast<int>(bitMaskLen) << ": " << why;
  }
}

/*
 * folly renders IPv6 through inet_ntop while the Python mirror uses the
 * ipaddress module. RFC 5952 zero-run compression is the one place the two can
 * disagree, and a disagreement would make every derived key name differ. At
 * /128 a compressible run is far too rare to arise from a digest (~1.6e-9), so
 * these vectors exercise the rules directly -- which also pre-covers the scale
 * path ever moving off /128, where masking makes compression hit every key.
 */
TEST(DeterministicPrefixGeneratorTest, FormatsAddressesPerRfc5952) {
  const auto golden = loadGolden();
  ASSERT_FALSE(golden["addressFormatting"].empty());

  for (const auto& entry : golden["addressFormatting"]) {
    const auto bytesHex = entry["bytesHex"].asString();
    const auto raw = folly::unhexlify(bytesHex);
    const auto address = folly::IPAddressV6::fromBinary(
        folly::ByteRange(folly::StringPiece(raw)));
    EXPECT_EQ(entry["address"].asString(), address.str())
        << bytesHex << ": " << entry["why"].asString();
  }
}

/*
 * The property the whole design rests on: re-running with the same seed
 * reproduces the same prefixes, so re-injection overwrites the previous run's
 * keys instead of inserting a fresh disjoint set.
 */
TEST(DeterministicPrefixGeneratorTest, SameSeedReproducesIdenticalPrefixes) {
  const auto first =
      DeterministicPrefixGenerator::generate(7, "leaf-3", 64, kHostMaskLen);
  const auto second =
      DeterministicPrefixGenerator::generate(7, "leaf-3", 64, kHostMaskLen);
  EXPECT_EQ(first, second);
}

TEST(DeterministicPrefixGeneratorTest, DifferentSeedsProduceDisjointPrefixes) {
  const auto fromSeed7 = addressSet(7, "leaf-3", 64);
  const auto fromSeed8 = addressSet(8, "leaf-3", 64);

  ASSERT_EQ(64, fromSeed7.size());
  ASSERT_EQ(64, fromSeed8.size());
  std::set<std::string> shared;
  std::set_intersection(
      fromSeed7.begin(),
      fromSeed7.end(),
      fromSeed8.begin(),
      fromSeed8.end(),
      std::inserter(shared, shared.begin()));
  EXPECT_TRUE(shared.empty());
}

/*
 * Node name must be part of the derivation, or every node in the fabric would
 * advertise the same prefixes and the per-node key sets would collide.
 */
TEST(
    DeterministicPrefixGeneratorTest,
    DifferentNodeNamesProduceDisjointPrefixes) {
  const auto fromLeaf = addressSet(7, "leaf-3", 64);
  const auto fromSpine = addressSet(7, "spine-3", 64);

  std::set<std::string> shared;
  std::set_intersection(
      fromLeaf.begin(),
      fromLeaf.end(),
      fromSpine.begin(),
      fromSpine.end(),
      std::inserter(shared, shared.begin()));
  EXPECT_TRUE(shared.empty());
}

/*
 * fc00::/8 keeps derived addresses out of the globally routable space, matching
 * PrefixGenerator's convention.
 */
TEST(DeterministicPrefixGeneratorTest, EveryAddressIsUniqueLocal) {
  for (uint32_t index = 0; index < 256; ++index) {
    const auto address =
        DeterministicPrefixGenerator::deriveAddress(42, "leaf-0", index);
    EXPECT_EQ(kUlaFirstByte, address.bytes()[0]) << "index=" << index;
  }
}

TEST(DeterministicPrefixGeneratorTest, MasksEveryPrefixToRequestedLength) {
  constexpr uint8_t kSubnetMaskLen = 64;
  const auto prefixes =
      DeterministicPrefixGenerator::generate(7, "leaf-3", 8, kSubnetMaskLen);

  ASSERT_EQ(8, prefixes.size());
  for (const auto& ipPrefix : prefixes) {
    EXPECT_EQ(kSubnetMaskLen, *ipPrefix.prefixLength());
    const auto network = toIPNetwork(ipPrefix);
    EXPECT_EQ(network.first, network.first.mask(kSubnetMaskLen));
  }
}

TEST(DeterministicPrefixGeneratorTest, GeneratesRequestedCount) {
  EXPECT_TRUE(
      DeterministicPrefixGenerator::generate(7, "leaf-3", 0, kHostMaskLen)
          .empty());
  EXPECT_EQ(
      11,
      DeterministicPrefixGenerator::generate(7, "leaf-3", 11, kHostMaskLen)
          .size());
}
