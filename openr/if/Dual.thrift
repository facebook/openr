/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.Dual
namespace py openr.Dual
namespace py3 openr.thrift
namespace lua openr.Dual
namespace rust openr_dual_thrift

/**
 * DUAL message type
 */
include "thrift/annotation/cpp.thrift"

enum DualMessageType {
  UPDATE = 1,
  QUERY = 2,
  REPLY = 3,
}

/**
 * A single DUAL message
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
 * Container representing multiple dual messages
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
 * Number of packets and dual-messages sent/recv for a neighbor
 * one packet may contain multiple messages
 */
struct DualPerNeighborCounters {
  1: i64 pktSent = 0;
  2: i64 pktRecv = 0;
  3: i64 msgSent = 0;
  4: i64 msgRecv = 0;
}

/**
 * Dual exchange message counters for a given root per neighbor
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
 * Map of neighbor-node to neighbor-counters
 */
@cpp.Type{
  name = "std::unordered_map<std::string, /* neighbor */ openr::thrift::DualPerNeighborCounters>",
}
typedef map<string, DualPerNeighborCounters> NeighborCounters

/**
 * Map of root-node to root-counters
 */
@cpp.Type{
  name = "std::unordered_map<std::string, /* root */ std::map<std::string /* neighbor */, openr::thrift::DualPerRootCounters>>",
}
typedef map<string, map<string, DualPerRootCounters>> RootCounters

/**
 * All DUAL related counters
 */
struct DualCounters {
  1: NeighborCounters neighborCounters;
  2: RootCounters rootCounters;
}
