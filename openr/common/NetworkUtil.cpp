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
  return hash<string>()(ipPrefix.prefixAddress.addr.toStdString()) +
      ipPrefix.prefixLength;
}

/**
 * Make BinaryAddress hashable
 */
size_t
hash<openr::thrift::BinaryAddress>::operator()(
    openr::thrift::BinaryAddress const& addr) const {
  size_t res = hash<string>()(addr.addr.toStdString());
  if (addr.ifName.hasValue()) {
    res += hash<string>()(addr.ifName.value());
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
  for (const auto& nh : route.nexthops) {
    res += hash<openr::thrift::BinaryAddress>()(nh);
  }
  return res;
}

} // namespace std
