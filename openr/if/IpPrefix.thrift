/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.IpPrefix
namespace php OpenR_IpPrefix

# fbstring uses the small internal buffer to store the data
# if the data is small enough (< 24 bytes).
typedef binary (cpp.type = "::folly::fbstring") fbbinary

struct BinaryAddress {
  1: required fbbinary addr,
  3: optional string ifName,
}

struct IpPrefix {
  1: BinaryAddress prefixAddress
  2: i16 prefixLength
}

struct UnicastRoute {
  1: required IpPrefix dest,
  2: required list<BinaryAddress> nexthops,
}
