/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/Types.h>

#include <openr/common/NetworkUtil.h>

namespace openr {

KeyPrefix::KeyPrefix(std::vector<std::string> const& keyPrefixList) {
  if (keyPrefixList.empty()) {
    return;
  }
  re2::RE2::Options re2Options;
  re2Options.set_case_sensitive(true);
  keyPrefix_ =
      std::make_unique<re2::RE2::Set>(re2Options, re2::RE2::ANCHOR_START);
  std::string re2AddError{};

  for (auto const& keyPrefix : keyPrefixList) {
    if (keyPrefix_->Add(keyPrefix, &re2AddError) < 0) {
      LOG(FATAL) << "Failed to add prefixes to RE2 set: '" << keyPrefix << "', "
                 << "error: '" << re2AddError << "'";
      return;
    }
  }
  if (!keyPrefix_->Compile()) {
    LOG(FATAL) << "Failed to compile re2 set";
    keyPrefix_.reset();
  }
}

bool
KeyPrefix::keyMatch(std::string const& key) const {
  if (!keyPrefix_) {
    return true;
  }
  std::vector<int> matches;
  return keyPrefix_->Match(key, &matches);
}

PrefixKey::PrefixKey(
    std::string const& node,
    folly::CIDRNetwork const& prefix,
    const std::string& area)
    : node_(node),
      prefix_(prefix),
      prefixArea_(area),
      prefixKeyString_(folly::sformat(
          "{}{}:{}:[{}/{}]",
          Constants::kPrefixDbMarker.toString(),
          node_,
          prefixArea_,
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
    return folly::makeUnexpected(folly::sformat("Invalid key format {}", key));
  }

  try {
    ipaddress =
        folly::IPAddress::createNetwork(folly::sformat("{}/{}", ipstr, plen));
  } catch (const folly::IPAddressFormatException& e) {
    LOG(INFO) << "Exception in converting to Prefix. " << e.what();
    return folly::makeUnexpected(std::string("Invalid IP address in key"));
  }
  return PrefixKey(node, ipaddress, area);
}

std::string
PrefixKey::getNodeName() const {
  return node_;
};

folly::CIDRNetwork
PrefixKey::getCIDRNetwork() const {
  return prefix_;
}

std::string
PrefixKey::getPrefixKey() const {
  return prefixKeyString_;
}

std::string
PrefixKey::getPrefixArea() const {
  return prefixArea_;
}

thrift::IpPrefix
PrefixKey::getIpPrefix() const {
  return toIpPrefix(prefix_);
}

} // namespace openr
