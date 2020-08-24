/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.Dual
namespace php Openr
namespace py openr.Dual
namespace py3 openr.thrift
namespace lua openr.Dual

// DUAL message type
enum DualMessageType {
  UPDATE = 1,
  QUERY = 2,
  REPLY = 3,
}

// a single DUAL message
struct DualMessage {
  // destination-id
  1: string dstId;
  // report-distance towards dst-id
  2: i64 distance;
  // message type
  3: DualMessageType type;
}

struct DualMessages {
  // sender node-id
  1: string srcId;
  // a list of dual-messages
  2: list<DualMessage> messages;
}

// number of packets and dual-messages sent/recv for a neighbor
// one packet may contain multiple messages
struct DualPerNeighborCounters {
  1: i64 pktSent = 0;
  2: i64 pktRecv = 0;
  3: i64 msgSent = 0;
  4: i64 msgRecv = 0;
}

// dual exchange message counters for a given root per neighbor
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

typedef map<string, DualPerNeighborCounters>
  (cpp.type =
    "std::unordered_map<std::string, /* neighbor */ openr::thrift::DualPerNeighborCounters>")
  NeighborCounters

typedef map<string, map<string, DualPerRootCounters>>
  (cpp.type =
    "std::unordered_map<std::string, /* root */ std::map<std::string /* neighbor */, openr::thrift::DualPerRootCounters>>")
  RootCounters

// all DUAL related counters
struct DualCounters {
  1: NeighborCounters neighborCounters;
  2: RootCounters rootCounters;
}
