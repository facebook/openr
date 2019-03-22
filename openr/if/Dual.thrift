/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace php Openr
namespace py openr.Dual

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
