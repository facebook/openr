/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.AllocPrefix
namespace py openr.AllocPrefix
namespace py3 openr.thrift
namespace lua openr.AllocPrefix
namespace wiki Open_Routing.Thrift_APIs

include "Network.thrift"

struct AllocPrefix {
  // prefix to allocate prefixes from
  1: Network.IpPrefix seedPrefix
  // allocated prefix length
  2: i64 allocPrefixLen
  // my allocated prefix, i.e., index within seed prefix
  3: i64 allocPrefixIndex
}

struct StaticAllocation {
  1: map<string /* node-name */, Network.IpPrefix> nodePrefixes;
}
