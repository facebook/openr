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

/**
 * @deprecated - DUAL message type
 */
enum DualMessageType {
  UPDATE = 1,
  QUERY = 2,
  REPLY = 3,
}

/**
 * @deprecated - A single DUAL message
 */
struct DualMessage {
  /**
   * destination-id
   */
  1: string dstId;

  /**
   * report-distance towards dst-id
   */
  2: i64 distance;

  /**
   * message type
   */
  3: DualMessageType type;
}

/**
 * @deprecated - Container representing multiple dual messages
 */
struct DualMessages {
  /**
   * sender node-id
   */
  1: string srcId;

  /**
   * List of dual-messages
   */
  2: list<DualMessage> messages;
}

/**
 * @deprecated - Number of packets and dual-messages sent/recv for a neighbor
 * one packet may contain multiple messages
 */
struct DualPerNeighborCounters {
  1: i64 pktSent = 0;
  2: i64 pktRecv = 0;
  3: i64 msgSent = 0;
  4: i64 msgRecv = 0;
}

/**
 * @deprecated - Dual exchange message counters for a given root per neighbor
 */
struct DualPerRootCounters {
  1: i64 querySent = 0;
  2: i64 queryRecv = 0;
  3: i64 replySent = 0;
  4: i64 replyRecv = 0;
  5: i64 updateSent = 0;
  6: i64 updateRecv = 0;
  7: i64 totalSent = 0;
  8: i64 totalRecv = 0;
}

/**
 * @deprecated - Map of neighbor-node to neighbor-counters
 */
typedef map<string, DualPerNeighborCounters>
  (cpp.type =
    "std::unordered_map<std::string, /* neighbor */ openr::thrift::DualPerNeighborCounters>")
  NeighborCounters

/**
 * @deprecated - Map of root-node to root-counters
 */
typedef map<string, map<string, DualPerRootCounters>>
  (cpp.type =
    "std::unordered_map<std::string, /* root */ std::map<std::string /* neighbor */, openr::thrift::DualPerRootCounters>>")
  RootCounters

/**
 * @deprecated - All DUAL related counters
 */
struct DualCounters {
  1: NeighborCounters neighborCounters;
  2: RootCounters rootCounters;
}
