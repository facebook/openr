/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.Types
namespace py openr.Types
namespace py3 openr.thrift
namespace lua openr.Types

include "Network.thrift"

/**
 * @deprecated - Allocated prefix information. This is stored in the persistent
 * store and can be read via config get thrift API.
 */
struct AllocPrefix {
  /**
   * Seed prefix from which sub-prefixes are allocated
   */
  1: Network.IpPrefix seedPrefix

  /**
   * Allocated prefix length
   */
  2: i64 allocPrefixLen

  /**
   * My allocated prefix, i.e., index within seed prefix
   */
  3: i64 allocPrefixIndex
}

/**
 * @deprecated - Prefix allocation configuration. This is set in KvStore by
 * remote controller. The PrefixAllocator learns its own prefix, assign it on
 * the interface, and advertise it in the KvStore.
 *
 * See PrefixAllocator documentation for static configuration mode.
 */
struct StaticAllocation {
  /**
   * Map of node to allocated prefix. This map usually contains entries for all
   * the nodes in the network.
   */
  1: map<string /* node-name */, Network.IpPrefix> nodePrefixes;
}
