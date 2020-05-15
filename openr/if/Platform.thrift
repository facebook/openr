/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace py openr.Platform
namespace py3 openr.thrift
namespace lua openr.Platform

include "fb303/thrift/fb303_core.thrift"
include "Network.thrift"

/**
 * We provide simple API to publish link/address updating events
 * through PUB-SUB mechanism to all of its subscriber modules in OpenR
 */
struct LinkEntry {
  1: string ifName;
  2: i64 ifIndex;
  3: bool isUp;
  4: i64 weight = 1; // used for weighted ecmp
}

struct AddrEntry {
  1: string ifName;
  2: Network.IpPrefix ipPrefix;
  3: bool isValid;
}

struct Link {
  1: i64 ifIndex;
  2: bool isUp;
  3: list<Network.IpPrefix> networks;
  4: string ifName;
  5: i64 weight = 1; // used for weighted ecmp
}

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
  FIB_SYNCED = 3,
  EXITING = 4
}

/**
 * Message sent over to subscriber of Platform Event.
 * eventType to indicate type of netlink event to be updated
 * eventData to indicate exact object entry to be updated
 * Notice: when sending out PlatformEvent make sure to send multi part messages:
 * part1: header to indicate event type,
 * which is 2 byte of PlatformEventType cast to unsigned int
 * part2: real message
 */
 enum PlatformEventType {
   /*
    * Command type to publish changes of link/address
    */
   LINK_EVENT = 1,
   ADDRESS_EVENT = 2,
 }

struct PlatformEvent {
  1: PlatformEventType eventType;
  2: binary eventData;
}

exception PlatformError {
  1: string message
} ( message = "message" )

/**
 * Thrift Service API definitions for on-box system information like links,
 * addresses and neighbors. OpenR leverages links and address information as
 * a part of link discovery and uses it to perform neighbor discovery on
 * retrieved links. There is also PUB/SUB mechanism over which updates can be
 * relayed to OpenR in realtime.
 */
service SystemService {
  /**
   * Get all the links on system. It is similar to `ip link show` commands.
   * Returns the list of interfaces with it's state, name and addresses
   */
  list<Link> getAllLinks()
    throws (1: PlatformError error)

  /**
   * Backward compatibility has been considered
   * As of now all the production platforms use our own SystemHandler
   * New platforms need implement those interfaces based on the platform APIs
   */
  void addIfaceAddresses(
    1: string iface,
    2: list<Network.IpPrefix> addrs)
    throws (1: PlatformError error)

  void removeIfaceAddresses(
    1: string iface,
    2: list<Network.IpPrefix> addrs)
    throws (1: PlatformError error)

  void syncIfaceAddresses(
    1: string iface,
    2: i16 family,
    3: i16 scope,
    4: list<Network.IpPrefix> addrs)
    throws (1: PlatformError error)

  list<Network.IpPrefix> getIfaceAddresses(
    1: string iface, 2: i16 family, 3: i16 scope)
    throws (1: PlatformError error)
}

// static mapping of clientId => protocolId, priority same of admin distance
// For Open/R.
//    ClientId: 786 => ProtocolId: 99, Priority: 10
// For BGP
//    ClientId: 0 => ProtocolId: 253, Priority: 20
// NOTE: protocolID must be less than 254
const map<i16, i16> clientIdtoProtocolId = {
    786: 99,  // Open/R
    0: 253,   // BGP
}
const map<i16, i16> protocolIdtoPriority = {
    99: 10,   // Open/R
    253: 20,  // BGP
}
const i16 kUnknowProtAdminDistance = 255

/**
 * Interface to on-box Fib.
 */
service FibService extends fb303_core.BaseService {

  /*
  * get run state
  */
  SwitchRunState getSwitchRunState()

  //
  // Unicast Routes API
  //
  void addUnicastRoute(
    1: i16 clientId,
    2: Network.UnicastRoute route,
  ) throws (1: PlatformError error)

  void deleteUnicastRoute(
    1: i16 clientId,
    2: Network.IpPrefix prefix,
  ) throws (1: PlatformError error)

  void addUnicastRoutes(
    1: i16 clientId,
    2: list<Network.UnicastRoute> routes,
  ) throws (1: PlatformError error)

  void deleteUnicastRoutes(
    1: i16 clientId,
    2: list<Network.IpPrefix> prefixes,
  ) throws (1: PlatformError error)

  void syncFib(
    1: i16 clientId,
    2: list<Network.UnicastRoute> routes,
  ) throws (1: PlatformError error)

  // Retrieve list of unicast routes per client
  list<Network.UnicastRoute> getRouteTableByClient(
    1: i16 clientId
  ) throws (1: PlatformError error)

  //
  // MPLS routes API
  //
  void addMplsRoutes(
    1: i16 clientId,
    2: list<Network.MplsRoute> routes,
  ) throws (1: PlatformError error)

  void deleteMplsRoutes(
    1: i16 clientId,
    2: list<i32> topLabels,
  ) throws (1: PlatformError error)

  // Flush previous routes and install new routes without disturbing
  // traffic. Similar to syncFib API
  void syncMplsFib(
    1: i16 clientId,
    2: list<Network.MplsRoute> routes,
  ) throws (1: PlatformError error)

  // Retrieve list of MPLS routes per client
  list<Network.MplsRoute> getMplsRouteTableByClient(
    1: i16 clientId
  ) throws (1: PlatformError error)

  void registerForNeighborChanged()
    throws (1: PlatformError error) (thread='eb')

  void sendNeighborDownInfo(
    1: list<string> neighborIp
    )
    throws (1: PlatformError error)

  //
  // FBOSS Agent API (for emulation only)
  //
  list<Network.LinkNeighborThrift> getLldpNeighbors()
    throws (1: PlatformError error)

  Network.PortInfoThrift getPortInfo(
    1: i32 portId
    )
    throws (1: PlatformError error)

  map<i32, Network.PortInfoThrift> getAllPortInfo()
  throws (1: PlatformError error)
}

service NeighborListenerClientForFibagent {
  /*
   * Sends list of neighbors that have changed to the subscriber.
   *
   * These come in the form of ip address strings which have been added
   * since the last notification. Changes are not queued between
   * subscriptions.
   */
  void neighborsChanged(1: list<string> added, 2: list<string> removed)
    throws (1: PlatformError error)
}
