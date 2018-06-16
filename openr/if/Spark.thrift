/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace php Openr
namespace py openr.Spark

include "IpPrefix.thrift"

//
// The below uses "required" a lot. This helps with
// strict protocol message parsing, as we bork if
// a field is missing. This is kind of a simple way
// of avoiding to check for default values
//

//
// Describe a single neighbors
//
struct SparkNeighbor {
  // the name of the domain to which this neighbor belongs to
  6: required string domainName

  // the name of the node sending hello packets
  1: required string nodeName

  // how long to retain our data for in milliseconds
  2: required i32 holdTime

  // DEPRECATED: our public key
  3: binary publicKey = ""

  // our transport addresses (right now - link local)
  4: IpPrefix.BinaryAddress transportAddressV6
  5: IpPrefix.BinaryAddress transportAddressV4

  // neighbor's kvstore global pub/cmd ports
  7: required i32 kvStorePubPort
  8: required i32 kvStoreCmdPort

  // the interface name of the node sending hello packets over
  9: string ifName = ""
}

//
// Describe time-stamp information about send/recv of hello
// packets. We use this to determine RTT of a node
//
struct ReflectedNeighborInfo {
  // Last sequence number we heard from neighbor
  1: i64 seqNum = 0;

  // Timestamp of last hello packet sent by sender to neighbor from which hello
  // packet is received
  2: i64 lastNbrMsgSentTsInUs = 0;

  // Timestamp when the last packet was received by neighbor from which hello
  // packet is received
  3: i64 lastMyMsgRcvdTsInUs = 0;
}

//
// OpenR version
//
typedef i32 OpenrVersion

//
// This is the data embedded in the payload of hello packet
//
struct SparkPayload {
  7: OpenrVersion version = 20180307

  1: required SparkNeighbor originator

  // the senders sequence number, inremented on each hello
  3: required i64 seqNum

  // neighbor to hello packet time-stamp information
  4: required map<string, ReflectedNeighborInfo> neighborInfos;

  // current timestamp of this packet. This will be reflected back to neighbor
  // in next hello packet just like sequence number in neighborInfos
  5: i64 timestamp;

  // solicit for an immediate hello packet back cause I am in fast initial state
  6: bool solicitResponse = 0;
}

//
// This is used to create a new timer
//
struct SparkHelloPacket {
  1: required SparkPayload payload
  2: required binary signature
}

enum SparkNeighborEventType {
  NEIGHBOR_UP         = 1,
  NEIGHBOR_DOWN       = 2,
  NEIGHBOR_RESTART    = 3,
  NEIGHBOR_RTT_CHANGE = 4,
}

//
// This is used to inform clients of new neighbor
//
struct SparkNeighborEvent {
  1: required SparkNeighborEventType eventType
  2: required string ifName
  3: required SparkNeighbor neighbor
  4: required i64 rttUs
  5: required i32 label   // Derived based off of ifIndex (local per node)
}

//
// Spark result status
//
struct SparkIfDbUpdateResult {
  1: bool isSuccess
  2: string errString
}
