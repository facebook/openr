/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <folly/IPAddressV6.h>

#include <openr/if/gen-cpp2/Network_types.h>

namespace openr {

/*
 * Derives a node's advertised prefixes as a pure function of
 * (seed, nodeName, prefixIndex), so an off-box tool can compute the same
 * prefixes -- and therefore the same KvStore key names -- from the topology
 * flags alone, without observing the injector.
 *
 * Contrast with PrefixGenerator, which draws from folly::Random and is what
 * every benchmark and unit test wants. Because those prefixes are unknowable
 * off-box, a test can only count injected keys, never name them; and each run
 * inserts a fresh disjoint set rather than overwriting the previous one.
 *
 * Deliberately NOT a seeded RNG stream. A reimplementation in another language
 * would have to stay bug-compatible with folly::Random forever. Hashing a
 * canonical string instead makes the contract portable:
 *
 *   digest = SHA256(utf8("<seed>|<nodeName>|<prefixIndex>"))
 *   bytes  = 0xfc || digest[0..14]
 *   addr   = IPv6(bytes), masked to bitMaskLen by generate()
 *
 * The 0xfc first byte keeps addresses inside fc00::/8 (ULA), matching
 * PrefixGenerator. Every input is a topology parameter: nothing run-varying
 * (time, hostname, iteration order) may enter, or the two implementations
 * silently diverge.
 *
 * The Python mirror lives at openr/tests/scale/scripts/scale_key_names.py, and
 * both sides are pinned to the golden vectors in
 * openr/tests/scale/tests/testdata/seeded_prefix_golden.json.
 */
class DeterministicPrefixGenerator {
 public:
  /*
   * The unmasked address for one (seed, nodeName, index). index is 0-based.
   */
  static folly::IPAddressV6 deriveAddress(
      uint64_t seed, const std::string& nodeName, uint32_t index);

  /*
   * numPrefixes prefixes for nodeName, from index 0, each masked to
   * bitMaskLen. The masked address is what ends up in the KvStore key, so a
   * caller changing bitMaskLen changes the key names.
   */
  static std::vector<thrift::IpPrefix> generate(
      uint64_t seed,
      const std::string& nodeName,
      uint32_t numPrefixes,
      uint8_t bitMaskLen);
};

} // namespace openr
