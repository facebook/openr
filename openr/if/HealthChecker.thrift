/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.HealthChecker

include "IpPrefix.thrift"

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

enum HealthCheckerCmd {
  PEEK = 0,
}

struct HealthCheckerRequest {
  1: HealthCheckerCmd cmd
  // If nodeName is empty then current node's routes will be returned in
  // response.
  2: string nodeName
}

struct NodeHealthInfo {
  1: list<string>  neighbors
  2: IpPrefix.BinaryAddress ipAddress
  3: i64 lastValSent
  4: i64 lastAckFromNode
  5: i64 lastAckToNode
}

struct HealthCheckerPeekReply {
  1: map<string, NodeHealthInfo> nodeInfo
}
