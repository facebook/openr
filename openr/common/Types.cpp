/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>
#include <openr/common/Types.h>

#include <openr/common/NetworkUtil.h>

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
      LOG(FATAL) << "Failed to add prefixes to RE2 set: '" << keyPrefix << "', "
                 << "error: '" << re2AddError << "'";
      return;
    }
  }
  if (!regexSet_->Compile()) {
    LOG(FATAL) << "Failed to compile re2 set";
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
    const std::string& area,
    bool isPrefixKeyV2)
    : nodeAndArea_(node, area),
      prefix_(prefix),
      isPrefixKeyV2_(isPrefixKeyV2),
      prefixKeyString_(fmt::format(
          "{}{}:{}:[{}/{}]",
          Constants::kPrefixDbMarker.toString(),
          node,
          area,
          prefix_.first.str(),
          prefix_.second)),
      prefixKeyStringV2_(fmt::format(
          "{}{}:[{}/{}]",
          Constants::kPrefixDbMarker.toString(),
          node,
          prefix_.first.str(),
          prefix_.second)) {}

bool
PrefixKey::isPrefixKeyV2Str(const std::string& key) {
  int64_t plen{0};
  std::string node{};
  std::string ipStr{};
  folly::CIDRNetwork ipAddress;
  auto patt =
      RE2::FullMatch(key, PrefixKey::getPrefixRE2(), &node, &ipStr, &plen);
  auto pattV2 =
      RE2::FullMatch(key, PrefixKey::getPrefixRE2V2(), &node, &ipStr, &plen);

  return (not patt) and pattV2;
}

folly::Expected<PrefixKey, std::string>
PrefixKey::fromStr(const std::string& key, const std::string& areaIn) {
  bool isV2PrefixKey{false};
  int plen{0};
  std::string area{};
  std::string node{};
  std::string ipStr{};
  folly::CIDRNetwork network;

  auto patt = RE2::FullMatch(key, getPrefixRE2(), &node, &area, &ipStr, &plen);
  if (not patt) {
    auto pattV2 =
        RE2::FullMatch(key, PrefixKey::getPrefixRE2V2(), &node, &ipStr, &plen);
    if (not pattV2) {
      return folly::makeUnexpected(
          fmt::format("Invalid format for key: {}.", key));
    }
    // this is a v2 format prefix key
    isV2PrefixKey = true;
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
  return PrefixKey(
      node,
      network,
      isV2PrefixKey ? areaIn : area, /* use passed in area for v2 format */
      isV2PrefixKey);
}

} // namespace openr
