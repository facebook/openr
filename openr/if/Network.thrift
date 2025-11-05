/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package "meta.com/openr"

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.Network
namespace py openr.Network
namespace py3 openr.thrift
namespace php Openr
namespace lua openr.Network
namespace rust openr_network_thrift
namespace wiki Open_Routing.Thrift_APIs.Network

// Using the defaults from here:
// https://en.wikipedia.org/wiki/Administrative_distance
include "thrift/annotation/cpp.thrift"

enum AdminDistance {
  DIRECTLY_CONNECTED = 0,
  STATIC_ROUTE = 1,
  EBGP = 20,
  IBGP = 200,
  NETLINK_LISTENER = 225,
  MAX_ADMIN_DISTANCE = 255,
}

enum MplsActionCode {
  PUSH = 0,
  SWAP = 1,
  PHP = 2, # Pen-ultimate hop popping => POP and FORWARD
  POP_AND_LOOKUP = 3,
  NOOP = 4,
}

@cpp.MinimizePadding
struct MplsAction {
  1: MplsActionCode action;
  2: optional i32 swapLabel; // Required if action == SWAP
  // front() (index=0) in list will be bottom of stack and back()
  // element is top of the stack
  3: optional list<i32> pushLabels; // Required if action == PUSH
}

@cpp.MinimizePadding
struct BinaryAddress {
  1: binary addr;
  3: optional string ifName;
}

@cpp.MinimizePadding
struct IpPrefix {
  1: BinaryAddress prefixAddress;
  2: i16 prefixLength;
}

@cpp.MinimizePadding
struct NextHopThrift {
  1: BinaryAddress address;
  // Default weight of 0 represents an ECMP route.
  // This default is chosen for two reasons:
  // 1) We rely on the arithmetic properties of 0 for ECMP vs UCMP route
  //    resolution calculations. A 0 weight next hop being present at a variety
  //    of layers in a route resolution tree will cause the entire route
  //    resolution to use ECMP.
  // 2) A client which does not set a value will result in
  //    0 being populated even with strange behavior in the client language
  //    which is consistent with C++
  2: i32 weight = 0;
  // MPLS encapsulation information for IP->MPLS and MPLS routes
  3: optional MplsAction mplsAction;

  // Metric (aka cost) associated with this nexthop
  51: i32 metric = 0;

  //
  // TODO: Define internal representation of NextHop within Open/R. We shouldn't
  // expose internal attributes to outside. `metric` though internal as of now,
  // is going to be useful when FBOSS supports it underneath
  //

  // Area field associated with next-hop. This is derived from an adjacency,
  // from where the transport address is also derived. This can be none for
  // imported routes.
  53: optional string area;

  // Name of next-hop device
  54: optional string neighborNodeName;

  // Connection status of next-hop.
  55: optional bool isConnected;
}

@cpp.MinimizePadding
struct MplsRoute {
  1: i32 topLabel;
  3: optional AdminDistance adminDistance;
  4: list<NextHopThrift> nextHops;
}

enum PrefixType {
  LOOPBACK = 1,
  DEFAULT = 2,
  BGP = 3,
  PREFIX_ALLOCATOR = 4,
  BREEZE = 5, // Prefixes injected via breeze
  RIB = 6,
  SLO_PREFIX_ALLOCATOR = 7,
  CONFIG = 8, // Route Origination
  VIP = 9, // VIP injected by vip service
  CPE = 10, // Customer-Premises Equipment

  // Placeholder Types
  TYPE_1 = 21,
  TYPE_2 = 22,
  TYPE_3 = 23,
  TYPE_4 = 24,
  TYPE_5 = 25,
}

// Route counter ID type
typedef string RouteCounterID

@cpp.MinimizePadding
struct UnicastRoute {
  1: IpPrefix dest;
  3: optional AdminDistance adminDistance;
  4: list<NextHopThrift> nextHops;
  7: optional RouteCounterID counterID;
}
