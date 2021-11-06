/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>
#include <folly/logging/xlog.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>

namespace openr {

RegexSet::RegexSet(std::vector<std::string> const& keyPrefixList) {
  if (keyPrefixList.empty()) {
    return;
  }
  re2::RE2::Options re2Options;
  re2Options.set_case_sensitive(true);
  regexSet_ =
      std::make_unique<re2::RE2::Set>(re2Options, re2::RE2::ANCHOR_START);
  std::string re2AddError{};

  for (auto const& keyPrefix : keyPrefixList) {
    if (regexSet_->Add(keyPrefix, &re2AddError) < 0) {
      XLOG(FATAL) << "Failed to add prefixes to RE2 set: '" << keyPrefix
                  << "', "
                  << "error: '" << re2AddError << "'";
      return;
    }
  }
  if (!regexSet_->Compile()) {
    XLOG(FATAL) << "Failed to compile re2 set";
  }
}

bool
RegexSet::match(std::string const& key) const {
  CHECK(regexSet_);
  std::vector<int> matches;
  return regexSet_->Match(key, &matches);
}

PrefixKey::PrefixKey(
    std::string const& node,
    folly::CIDRNetwork const& prefix,
    const std::string& area)
    : nodeAndArea_(node, area),
      prefix_(prefix),
      prefixKeyStringV2_(fmt::format(
          "{}{}:[{}/{}]",
          Constants::kPrefixDbMarker.toString(),
          node,
          prefix_.first.str(),
          prefix_.second)) {}

folly::Expected<PrefixKey, std::string>
PrefixKey::fromStr(const std::string& key, const std::string& areaIn) {
  bool isV2PrefixKey{false};
  int plen{0};
  std::string node{};
  std::string ipStr{};
  folly::CIDRNetwork network;

  auto pattV2 =
      RE2::FullMatch(key, PrefixKey::getPrefixRE2V2(), &node, &ipStr, &plen);
  if (not pattV2) {
    return folly::makeUnexpected(
        fmt::format("Invalid format for key: {}.", key));
  }

  try {
    network =
        folly::IPAddress::createNetwork(fmt::format("{}/{}", ipStr, plen));
  } catch (const folly::IPAddressFormatException& e) {
    return folly::makeUnexpected(fmt::format(
        "Exception in converting string to IP address for key: {}. Exception: {}",
        key,
        e.what()));
  }
  return PrefixKey(node, network, areaIn);
}

} // namespace openr
