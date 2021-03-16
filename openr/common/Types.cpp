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
    const std::string& area)
    : nodeAndArea_(node, area),
      prefix_(prefix),
      prefixKeyString_(fmt::format(
          "{}{}:{}:[{}/{}]",
          Constants::kPrefixDbMarker.toString(),
          node,
          area,
          prefix_.first.str(),
          prefix_.second)) {}

folly::Expected<PrefixKey, std::string>
PrefixKey::fromStr(const std::string& key) {
  int plen{0};
  std::string area{};
  std::string node{};
  std::string ipstr{};
  folly::CIDRNetwork ipaddress;
  auto patt = RE2::FullMatch(key, getPrefixRE2(), &node, &area, &ipstr, &plen);
  if (!patt) {
    return folly::makeUnexpected(fmt::format("Invalid key format {}", key));
  }

  try {
    ipaddress =
        folly::IPAddress::createNetwork(fmt::format("{}/{}", ipstr, plen));
  } catch (const folly::IPAddressFormatException& e) {
    LOG(INFO) << "Exception in converting to Prefix. " << e.what();
    return folly::makeUnexpected(std::string("Invalid IP address in key"));
  }
  return PrefixKey(node, ipaddress, area);
}

NodeAndArea const&
PrefixKey::getNodeAndArea() const {
  return nodeAndArea_;
}

std::string const&
PrefixKey::getNodeName() const {
  return nodeAndArea_.first;
};

folly::CIDRNetwork const&
PrefixKey::getCIDRNetwork() const {
  return prefix_;
}

std::string const&
PrefixKey::getPrefixKey() const {
  return prefixKeyString_;
}

std::string const&
PrefixKey::getPrefixArea() const {
  return nodeAndArea_.second;
}

thrift::IpPrefix
PrefixKey::getIpPrefix() const {
  return toIpPrefix(prefix_);
}

} // namespace openr
