/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/NetworkUtil.h"

namespace std {

/**
 * Make IpPrefix hashable
 */
size_t
hash<openr::thrift::IpPrefix>::operator()(
    openr::thrift::IpPrefix const& ipPrefix) const {
  return hash<string>()(ipPrefix.prefixAddress.addr) + ipPrefix.prefixLength;
}

/**
 * Make BinaryAddress hashable
 */
size_t
hash<openr::thrift::BinaryAddress>::operator()(
    openr::thrift::BinaryAddress const& addr) const {
  size_t res = hash<string>()(addr.addr);
  if (addr.ifName.has_value()) {
    res += hash<string>()(addr.ifName.value());
  }
  return res;
}

/**
 * Make MplsAction hashable
 */
size_t
hash<openr::thrift::MplsAction>::operator()(
    openr::thrift::MplsAction const& mplsAction) const {
  size_t res = hash<int8_t>()(static_cast<int8_t>(mplsAction.action));
  if (mplsAction.swapLabel.has_value()) {
    res += hash<int32_t>()(mplsAction.swapLabel.value());
  }
  if (mplsAction.pushLabels.has_value()) {
    for (auto const& pushLabel : mplsAction.pushLabels.value()) {
      res += hash<int32_t>()(pushLabel);
    }
  }
  return res;
}

/**
 * Make NextHopThrift hashable
 */
size_t
hash<openr::thrift::NextHopThrift>::operator()(
    openr::thrift::NextHopThrift const& nextHop) const {
  size_t res = hash<openr::thrift::BinaryAddress>()(nextHop.address);
  res += hash<int32_t>()(nextHop.weight);
  res += hash<int32_t>()(nextHop.metric);
  if (nextHop.mplsAction.has_value()) {
    res += hash<openr::thrift::MplsAction>()(nextHop.mplsAction.value());
  }
  return res;
}

/**
 * Make UnicastRoute hashable
 */
size_t
hash<openr::thrift::UnicastRoute>::operator()(
    openr::thrift::UnicastRoute const& route) const {
  size_t res = hash<openr::thrift::IpPrefix>()(route.dest);
  for (const auto& nh : route.nextHops) {
    res += hash<openr::thrift::BinaryAddress>()(nh.address);
    res += hash<int32_t>()(nh.weight);
    if (nh.mplsAction.has_value()) {
      res += hash<openr::thrift::MplsAction>()(nh.mplsAction.value());
    }
  }
  return res;
}

} // namespace std
