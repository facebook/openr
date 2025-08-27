/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.Platform
namespace py openr.Platform
namespace py3 openr.thrift
namespace lua openr.Platform
namespace rust openr_thrift
namespace wiki Open_Routing.Thrift_APIs.Platform

include "fb303/thrift/fb303_core.thrift"
include "openr/if/Network.thrift"
include "thrift/annotation/thrift.thrift"

/**
 * Enum to keep track of Client name to Client-ID mapping. Indicates which
 * client-ids are used and which are available to use.
 */
enum FibClient {
  // OpenR Client
  OPENR = 786,

  // BGP Client
  BGP = 0,

  // Some Placeholder Clients
  CLIENT_1 = 1,
  CLIENT_2 = 2,
  CLIENT_3 = 3,
  CLIENT_4 = 4,
  CLIENT_5 = 5,
}

// SwSwitch run states. SwSwitch moves forward from a
// lower numbered state to the next
enum SwitchRunState {
  UNINITIALIZED = 0,
  INITIALIZED = 1,
  CONFIGURED = 2,
  EXITING = 4,
}

struct NextHopStatus {
  // True if there is an open/r route that resolves the nexthop.
  // False otherwise.
  1: bool isReachable;
  // If isReachable is true, this is the resolving route's distance.
  2: optional i32 igpCost;
}

struct NextHopRegistrationRequest {
  // List of nexthop IP addresseses to register.
  // Each binary string contains the nexthop IP address bytes in network byte order.
  // 4 bytes for IPv4, and 16 bytes for IPv6.
  1: list<binary> nexthops;
}

struct NextHopRegistrationResponse {
  // nexthop->status
  // Each binary string contains the nexthop IP address bytes in network byte order.
  // 4 bytes for IPv4, and 16 bytes for IPv6.
  1: map<binary, NextHopStatus> nexthopStatuses;
}

struct NextHopDeregistrationRequest {
  // List of nexthop IP addresseses to deregister.
  // Each binary string contains the nexthop IP address bytes in network byte order.
  // 4 bytes for IPv4, and 16 bytes for IPv6.
  1: list<binary> nexthops;
}

struct NextHopDeregistrationResponse {}

struct StreamNextHopStatusRequest {
  // If true, FibAgent will stream nexthop status for all loopback
  // routes even if there are no registered nexthops.
  1: optional bool stream_all_loopbacks;
}

struct StreamNextHopStatusResponse {
  // nexthop->status
  // Each binary string contains the nexthop IP address bytes in network byte order.
  // 4 bytes for IPv4, and 16 bytes for IPv6.
  1: map<binary, NextHopStatus> nexthopStatuses;
}

struct ConnectedNextHopStatus {
  // Next Hop IP address
  1: binary remoteAddress;
  // Next Hop Interface name
  2: optional string interfaceName;
  // status of whether nexthop is reachable or not
  3: bool isReachable;
}

struct ConnectedNextHopStatusRequest {
  1: list<ConnectedNextHopStatus> nextHopStatuses;
}

struct ConnectedNextHopStatusResponse {}

exception PlatformError {
  @thrift.ExceptionMessage
  1: string message;
}

exception PlatformFibUpdateError {
  1: map<i32, list<Network.IpPrefix>> vrf2failedAddUpdatePrefixes;
  2: map<i32, list<Network.IpPrefix>> vrf2failedDeletePrefixes;
  3: list<i32> failedAddUpdateMplsLabels;
  4: list<i32> failedDeleteMplsLabels;
}

// static mapping of clientId => protocolId, priority same of admin distance
// For Open/R.
//    ClientId: 786 => ProtocolId: 99, Priority: 10
// For BGP
//    ClientId: 0 => ProtocolId: 253, Priority: 20
// NOTE: protocolID must be less than 254
const map<i16, i16> clientIdtoProtocolId = {786: 99, 0: 253}; // Open/R
const map<i16, i16> protocolIdtoPriority = {99: 10, 253: 20}; // Open/R
const i16 kUnknowProtAdminDistance = 255;

/**
 * Interface to on-box Fib.
 */
service FibService extends fb303_core.BaseService {
  /*
  * get run state
  */
  SwitchRunState getSwitchRunState();

  //
  // Unicast Routes API
  // NOTE: FibAgent may throw `PlatformFibUpdateError` for Add and Sync
  // APIs only. Delete should never fail, since it does not consume HW resources
  // but rather frees them. Agent already protect against spurious deletes e.g.
  // trying to delete non-existing route (aka double deletes).
  //

  void addUnicastRoute(1: i16 clientId, 2: Network.UnicastRoute route) throws (
    1: PlatformError error,
    2: PlatformFibUpdateError fibError,
  );

  void deleteUnicastRoute(1: i16 clientId, 2: Network.IpPrefix prefix) throws (
    1: PlatformError error,
  );

  void addUnicastRoutes(
    1: i16 clientId,
    2: list<Network.UnicastRoute> routes,
  ) throws (1: PlatformError error, 2: PlatformFibUpdateError fibError);

  void deleteUnicastRoutes(
    1: i16 clientId,
    2: list<Network.IpPrefix> prefixes,
  ) throws (1: PlatformError error);

  void syncFib(1: i16 clientId, 2: list<Network.UnicastRoute> routes) throws (
    1: PlatformError error,
    2: PlatformFibUpdateError fibError,
  );

  // Retrieve list of unicast routes per client
  list<Network.UnicastRoute> getRouteTableByClient(1: i16 clientId) throws (
    1: PlatformError error,
  );

  //
  // MPLS routes API
  // NOTE: FibAgent may throw `PlatformFibUpdateError` for Add and Sync
  // APIs only. Delete should never fail, since it does not consume HW resources
  // but rather frees them. Agent already protect against spurious deletes e.g.
  // trying to delete non-existing route (aka double deletes).
  //

  void addMplsRoutes(
    1: i16 clientId,
    2: list<Network.MplsRoute> routes,
  ) throws (1: PlatformError error, 2: PlatformFibUpdateError fibError);

  void deleteMplsRoutes(1: i16 clientId, 2: list<i32> topLabels) throws (
    1: PlatformError error,
  );

  // Flush previous routes and install new routes without disturbing
  // traffic. Similar to syncFib API
  void syncMplsFib(1: i16 clientId, 2: list<Network.MplsRoute> routes) throws (
    1: PlatformError error,
    2: PlatformFibUpdateError fibError,
  );

  // Retrieve list of MPLS routes per client
  list<Network.MplsRoute> getMplsRouteTableByClient(1: i16 clientId) throws (
    1: PlatformError error,
  );

  void sendNeighborDownInfo(1: list<string> neighborIp) throws (
    1: PlatformError error,
  );

  NextHopRegistrationResponse registerNextHops(
    NextHopRegistrationRequest req,
  ) throws (1: PlatformError error);

  NextHopDeregistrationResponse deregisterNextHops(
    NextHopDeregistrationRequest req,
  ) throws (1: PlatformError error);

  stream<StreamNextHopStatusResponse> streamNextHopStatus(
    StreamNextHopStatusRequest req,
  ) throws (1: PlatformError error);

  ConnectedNextHopStatusResponse updateConnectedNextHopStatus(
    ConnectedNextHopStatusRequest request,
  ) throws (1: PlatformError error);
}

service NeighborListenerClientForFibagent {
  /*
   * Sends list of neighbors that have changed to the subscriber.
   *
   * These come in the form of ip address strings which have been added
   * since the last notification. Changes are not queued between
   * subscriptions.
   */
  void neighborsChanged(1: list<string> added, 2: list<string> removed) throws (
    1: PlatformError error,
  );
}
