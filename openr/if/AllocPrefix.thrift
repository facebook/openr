/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.AllocPrefix

include "IpPrefix.thrift"

struct AllocPrefix {
  // prefix to allocate prefixes from
  1: IpPrefix.IpPrefix seedPrefix
  // allocated prefix length
  2: i64 allocPrefixLen
  // my allocated prefix, i.e., index within seed prefix
  3: i64 allocPrefixIndex
}

struct StaticAllocation {
  1: map<string /* node-name */, IpPrefix.IpPrefix> nodePrefixes;
}
