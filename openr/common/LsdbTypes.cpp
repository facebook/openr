/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>

#include <openr/common/LsdbTypes.h>
#include <re2/re2.h>

namespace openr {

PrefixKey::PrefixKey(
    std::string const& node,
    folly::CIDRNetwork const& prefix,
    const std::string& area)
    : nodeAndArea_(node, area),
      prefix_(prefix),
      prefixKeyStringV2_(
          fmt::format(
              "{}{}:[{}/{}]",
              Constants::kPrefixDbMarker.toString(),
              node,
              prefix_.first.str(),
              prefix_.second)) {}

folly::Expected<PrefixKey, std::string>
PrefixKey::fromStr(const std::string& key, const std::string& areaIn) {
  int plen{0};
  std::string node{};
  std::string ipStr{};
  folly::CIDRNetwork network;

  auto pattV2 =
      RE2::FullMatch(key, PrefixKey::getPrefixRE2V2(), &node, &ipStr, &plen);
  if (!pattV2) {
    return folly::makeUnexpected(
        fmt::format("Invalid format for key: {}.", key));
  }

  try {
    network =
        folly::IPAddress::createNetwork(fmt::format("{}/{}", ipStr, plen));
  } catch (const folly::IPAddressFormatException& e) {
    return folly::makeUnexpected(
        fmt::format(
            "Exception in converting string to IP address for key: {}. Exception: {}",
            key,
            e.what()));
  }
  return PrefixKey(node, network, areaIn);
}

} // namespace openr
