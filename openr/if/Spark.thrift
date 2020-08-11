/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace php Openr
namespace py openr.Spark
namespace py3 openr.thrift
namespace lua openr.Spark

include "Network.thrift"

//
// Data structure to send with SparkNeighborEvent to convey
// info for a single unique neighbor for upper module usage
//
struct SparkNeighbor {
  // the name of the node sending hello packets
  1: string nodeName

  // our transport addresses (right now - link local)
  4: Network.BinaryAddress transportAddressV6
  5: Network.BinaryAddress transportAddressV4

  // neighbor's kvstore global pub/cmd ports
  7: i32 openrCtrlThriftPort = 0
  8: i32 kvStoreCmdPort = 0

  // the interface name of the node sending hello packets over
  9: string ifName
}

//
// Describe timestamp information about send/recv of hello
// packets. We use this to determine RTT of a node
//
struct ReflectedNeighborInfo {
  // Last sequence number we heard from neighbor
  1: i64 seqNum = 0

  // Timestamp of last hello packet sent by sender to neighbor from which hello
  // packet is received
  2: i64 lastNbrMsgSentTsInUs = 0

  // Timestamp when the last packet was received by neighbor from which hello
  // packet is received
  3: i64 lastMyMsgRcvdTsInUs = 0
}

//
// OpenR version
//
typedef i32 OpenrVersion

//
// Spark will define 3 types of msg and fit into SparkPacket thrift structure:
// 1. SparkHelloMsg;
//    - Functionality:
//      1) To advertise its own existence and basic neighbor information;
//      2) To ask for immediate response for quick adjacency establishment;
//      3) To notify for its own "RESTART" to neighbors;
//    - SparkHelloMsg will be sent per interface;
// 2. SparkHeartbeatMsg;
//    - Functionality:
//      To notify its own aliveness by advertising msg periodically;
//    - SparkHeartbeatMsg will be sent per interface;
// 3. SparkHandshakeMsg;
//    - Functionality:
//      To exchange param information to establish adjacency;
//    - SparkHandshakeMsg will be sent per (interface, neighbor)
//
struct SparkHelloMsg {
  1: string domainName
  2: string nodeName
  3: string ifName
  4: i64 seqNum
  5: map<string, ReflectedNeighborInfo> neighborInfos
  6: OpenrVersion version
  7: bool solicitResponse = 0
  8: bool restarting = 0
  9: i64 sentTsInUs;
}

struct SparkHeartbeatMsg {
  1: string nodeName
  2: i64 seqNum
}

struct SparkHandshakeMsg {
  // name of the node originating this handshake message
  1: string nodeName

  // used as signal to keep/stop sending handshake msg
  2: bool isAdjEstablished

  // heartbeat expiration time
  3: i64 holdTime

  // graceful-restart expiration time
  4: i64 gracefulRestartTime

  // our transport addresses (right now - link local)
  5: Network.BinaryAddress transportAddressV6
  6: Network.BinaryAddress transportAddressV4

  // neighbor's kvstore global pub/cmd ports
  7: i32 openrCtrlThriftPort
  9: i32 kvStoreCmdPort

  // area identifier
  10: string area

  // Recipient neighbor node for this handshake message.
  // Other nodes will ignore. If not set, then this will
  // be treated as a multicast and all nodes will process it.
  //
  // TODO: Remove optional qualifier after AREA negotiation
  //       is fully in use
  11: optional string neighborNodeName
}

struct SparkHelloPacket {
  // - Msg to announce node's presence on link with its
  //   own params;
  // - Send out periodically and on receipt of hello msg
  //   with solicitation flag set;
  3: optional SparkHelloMsg helloMsg

  // - Msg to announce nodes's aliveness.
  // - Send out periodically on intf where there is at
  //   least one neighbor in ESTABLISHED state;
  4: optional SparkHeartbeatMsg heartbeatMsg

  // - Msg to exchange params to establish adjacency
  //   with neighbors;
  // - Send out periodically and on receipt of handshake msg;
  5: optional SparkHandshakeMsg handshakeMsg
}

enum SparkNeighborEventType {
  NEIGHBOR_UP         = 1,
  NEIGHBOR_DOWN       = 2,
  NEIGHBOR_RESTARTED  = 3,
  NEIGHBOR_RTT_CHANGE = 4,
  NEIGHBOR_RESTARTING = 5,
}

//
// SparkNeighborEvent wraps up info comsumed by
// upper level module for neighbor event defined
// in `SparkNeighborEventType`
//
// TODO: Migrate SparkNeighborEvent to NOT use thrift structure
struct SparkNeighborEvent {
  1: required SparkNeighborEventType eventType
  2: required string ifName
  3: required SparkNeighbor neighbor
  4: required i64 rttUs
  5: required i32 label   // Derived based off of ifIndex (local per node)
  // support flood optimization or not
  6: bool supportFloodOptimization = 0
  // area ID
  7: string area
}

//
// Spark result status
//
struct SparkIfDbUpdateResult {
  1: bool isSuccess
  2: string errString
}
