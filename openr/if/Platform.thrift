/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
namespace wiki Open_Routing.Thrift_APIs.Platform

include "fb303/thrift/fb303_core.thrift"
include "Network.thrift"

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

exception PlatformError {
  1: string message;
} (message = "message")

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

  list<Network.RouteDetails> getRouteTableDetails() throws (
    1: PlatformError error,
  );

  //
  // MPLS routes API
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

  //
  // BGP API (for emulation only)
  //
  void registerForNeighborChanged() throws (1: PlatformError error) (
    thread = 'eb',
  );

  void sendNeighborDownInfo(1: list<string> neighborIp) throws (
    1: PlatformError error,
  );

  //
  // FBOSS Agent API (for emulation only)
  //

  list<Network.LinkNeighborThrift> getLldpNeighbors() throws (
    1: PlatformError error,
  );

  Network.PortInfoThrift getPortInfo(1: i32 portId) throws (
    1: PlatformError error,
  );

  map<i32, Network.PortInfoThrift> getAllPortInfo() throws (
    1: PlatformError error,
  );

  // Dummy API - Returns empty result
  map<i32, Network.PortStatus> getPortStatus(1: list<i32> ports) throws (
    1: PlatformError error,
  );

  // Dummy API - Returns empty result
  list<Network.AggregatePortThrift> getAggregatePortTable() throws (
    1: PlatformError error,
  );
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
