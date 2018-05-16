/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.Lsdb
namespace php OpenR_Lsdb

include "IpPrefix.thrift"

//
// Performance measurement related structs
//

struct PerfEvent {
  1: string nodeName;
  2: string eventDescr;
  3: i64 unixTs = 0;
}

struct PerfEvents {
  1: list<PerfEvent> events;
}

//
// Interfaces
//

//
// InterfaceDb is the entire interface state
// for this system providing link status and
// Ipv4 / IPv6 LinkLocal addresses
// Spark uses this to initiate sessions with neighbors
// and the addresses are used as nextHops
// Fib uses interface state to update RouteDb and program
// next best routes
//
struct InterfaceInfo {
  1: required bool isUp
  2: required i64 ifIndex
  // TO BE DEPRECATED SOON
  3: required list<IpPrefix.BinaryAddress> v4Addrs
  // TO BE DEPRECATED SOON
  4: required list<IpPrefix.BinaryAddress> v6LinkLocalAddrs
  // ip prefixes of all v4 and v6 link local addresses
  5: list<IpPrefix.IpPrefix> networks
}

//
// InterfaceDatabase is a map if interfaces
// keyed by ifName as string
//
struct InterfaceDatabase {
  1: string thisNodeName
  2: map<string, InterfaceInfo> interfaces

  // Optional attribute to measure convergence performance
  3: optional PerfEvents perfEvents;
}

//
// Adjacencies
//

// describes a specific adjacency to a neighbor node
struct Adjacency {
  // must match a name bound to another node
  1: string otherNodeName

  // interface for the peer
  2: string ifName

  // peer's link-local addresses
  3: IpPrefix.BinaryAddress nextHopV6
  5: IpPrefix.BinaryAddress nextHopV4

  // metric to reach to the neighbor
  4: i32 metric

  // SR Adjacency Segment label associated with this adjacency. This is
  // node-local label and programmed by originator only assigned from
  // non-global space. 0 is invalid value
  6: i32 adjLabel = 0

  // Overloaded bit for adjacency. Indicates that this adjacency is not
  // available for any kind of transit traffic
  7: bool isOverloaded = 0

  // rtt to neighbor in us
  8: i32 rtt

  // timestamp at creation time in s since epoch, used to indicate uptime
  9: i64 timestamp;

  // weight of this adj in the network when weighted ECMP is enabled
  10: i64 weight = 1

  // interface the originator (peer) discover this node
  11: string otherIfName = ""
}

// full link state information of a single router
// announced under keys starting with "adjacencies:"
struct AdjacencyDatabase {
  // must use the same name as used in the key
  1: string thisNodeName

  // overload bit. Indicates if node should be use for transit(false)
  // or not(true).
  2: bool isOverloaded = 0

  // all adjacent neighbors for this node
  3: list<Adjacency> adjacencies

  // SR Nodal Segment label associated with this node. This is globally unique
  // label assigned from global static space. 0 is invalid value
  4: i32 nodeLabel

  // Optional attribute to measure convergence performance
  5: optional PerfEvents perfEvents;
}

//
// Prefixes
//

enum PrefixType {
  LOOPBACK = 1,
  DEFAULT = 2,
  BGP = 3,
  PREFIX_ALLOCATOR = 4,
  BREEZE = 5,   // Prefixes injected via breeze

  // Placeholder Types
  TYPE_1 = 21,
  TYPE_2 = 22,
  TYPE_3 = 23,
  TYPE_4 = 24,
  TYPE_5 = 25,
}

struct PrefixEntry {
  1: IpPrefix.IpPrefix prefix
  2: PrefixType type
  // optional additional metadata (encoding depends on PrefixType)
  3: binary data
}

// all prefixes that are bound to a given router
// announced under keys starting with "prefixes:"
struct PrefixDatabase {
  // must be the same as used in the key
  1: string thisNodeName
  // numbering is intentional
  3: list<PrefixEntry> prefixEntries

  // Optional attribute to measure convergence performance
  4: optional PerfEvents perfEvents;
}
