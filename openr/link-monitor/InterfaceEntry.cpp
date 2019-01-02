/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterfaceEntry.h"

#include <folly/gen/Base.h>

#include <openr/common/AddressUtil.h>

namespace openr {

thrift::InterfaceInfo
InterfaceEntry::getInterfaceInfo() const {

  std::vector<thrift::IpPrefix> networks;
  for (const auto& network : networks_) {
    networks.emplace_back(toIpPrefix(network));
  }

  return thrift::InterfaceInfo(
      apache::thrift::FRAGILE,
      isUp_,
      ifIndex_,
      // TO BE DEPERECATED SOON
      folly::gen::from(getV4Addrs()) |
        folly::gen::map(
          [](const folly::IPAddress& ip) {
            return toBinaryAddress(ip);
          }) |
        folly::gen::as<std::vector>(),
      // TO BE DEPRECATED SOON
      folly::gen::from(getV6LinkLocalAddrs()) |
        folly::gen::map(
          [](const folly::IPAddress& ip) {
            return toBinaryAddress(ip);
          }) |
        folly::gen::as<std::vector>(),
      networks);
}

} // namespace openr
