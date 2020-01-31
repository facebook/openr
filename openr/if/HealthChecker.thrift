/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace py openr.HealthChecker
namespace py3 openr.thrift

include "Network.thrift"

enum HealthCheckerMessageType {
  PING = 0,
  ACK = 1,
}

// HealthCheck ping options
enum HealthCheckOption {
  PingNeighborOfNeighbor = 0,
  PingTopology = 1,
  PingRandom = 2,
}

// structure of udp health checker message
struct HealthCheckerMessage {
  1: string fromNodeName
  2: HealthCheckerMessageType type
  3: i64 seqNum
}

struct NodeHealthInfo {
  1: list<string>  neighbors
  2: Network.BinaryAddress ipAddress
  3: i64 lastValSent
  4: i64 lastAckFromNode
  5: i64 lastAckToNode
}

struct HealthCheckerInfo {
  1: map<string, NodeHealthInfo> nodeInfo
}
