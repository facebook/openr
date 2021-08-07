/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/NetworkUtil.h>

namespace std {

/**
 * Make IpPrefix hashable
 */
size_t
hash<openr::thrift::IpPrefix>::operator()(
    openr::thrift::IpPrefix const& ipPrefix) const {
  return hash<string>()(*ipPrefix.prefixAddress_ref()->addr_ref()) +
      *ipPrefix.prefixLength_ref();
}

/**
 * Make BinaryAddress hashable
 */
size_t
hash<openr::thrift::BinaryAddress>::operator()(
    openr::thrift::BinaryAddress const& addr) const {
  size_t res = hash<string>()(*addr.addr_ref());
  if (addr.ifName_ref().has_value()) {
    res += hash<string>()(addr.ifName_ref().value());
  }
  return res;
}

/**
 * Make MplsAction hashable
 */
size_t
hash<openr::thrift::MplsAction>::operator()(
    openr::thrift::MplsAction const& mplsAction) const {
  size_t res = hash<int8_t>()(static_cast<int8_t>(*mplsAction.action_ref()));
  if (mplsAction.swapLabel_ref().has_value()) {
    res += hash<int32_t>()(mplsAction.swapLabel_ref().value());
  }
  if (mplsAction.pushLabels_ref().has_value()) {
    for (auto const& pushLabel : mplsAction.pushLabels_ref().value()) {
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
  size_t res = hash<openr::thrift::BinaryAddress>()(*nextHop.address_ref());
  res += hash<int32_t>()(*nextHop.weight_ref());
  res += hash<int32_t>()(*nextHop.metric_ref());
  if (nextHop.mplsAction_ref().has_value()) {
    res += hash<openr::thrift::MplsAction>()(nextHop.mplsAction_ref().value());
  }
  return res;
}

/**
 * Make UnicastRoute hashable
 */
size_t
hash<openr::thrift::UnicastRoute>::operator()(
    openr::thrift::UnicastRoute const& route) const {
  size_t res = hash<openr::thrift::IpPrefix>()(*route.dest_ref());
  for (const auto& nh : *route.nextHops_ref()) {
    res += hash<openr::thrift::BinaryAddress>()(*nh.address_ref());
    res += hash<int32_t>()(*nh.weight_ref());
    if (nh.mplsAction_ref().has_value()) {
      res += hash<openr::thrift::MplsAction>()(nh.mplsAction_ref().value());
    }
  }
  return res;
}

} // namespace std

namespace openr {

std::string
getRemoteIfName(const thrift::Adjacency& adj) {
  if (not adj.otherIfName_ref()->empty()) {
    return *adj.otherIfName_ref();
  }
  return fmt::format("neigh-{}", *adj.ifName_ref());
}

} // namespace openr
