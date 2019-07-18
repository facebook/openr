/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace py openr.OpenrCtrl
namespace py3 openr.thrift
namespace php Openr

include "common/fb303/if/fb303.thrift"
include "fbzmq/service/if/Monitor.thrift"
include "Decision.thrift"
include "Dual.thrift"
include "Fib.thrift"
include "HealthChecker.thrift"
include "KvStore.thrift"
include "LinkMonitor.thrift"
include "Lsdb.thrift"
include "Network.thrift"

enum OpenrModuleType {
  DECISION = 1,
  FIB = 2,
  HEALTH_CHECKER = 3,
  KVSTORE = 4,
  LINK_MONITOR = 5,
  PERSISTENT_STORE = 9,
  PREFIX_ALLOCATOR = 6,
  PREFIX_MANAGER = 7,
  SPARK = 8,
}

exception OpenrError {
  1: string message
} ( message = "message" )

/**
 * Thrift service - exposes RPC APIs for interaction with all of Open/R's
 * modules.
 */
service OpenrCtrl extends fb303.FacebookService {

  //
  // Raw APIs to directly interact with Open/R modules
  // @deprecated - do not use these directly. Instead use specific thrift APIs
  //

  binary command(1: OpenrModuleType module, 2: binary request)
    throws (1: OpenrError error)

  bool hasModule(1: OpenrModuleType module)
    throws (1: OpenrError error)

  //
  // PrefixManager APIs
  //

  /**
   * Advertise or Update prefixes
   */
  void advertisePrefixes(1: list<Lsdb.PrefixEntry> prefixes)
    throws (1: OpenrError error)

  /**
   * Withdraw previously advertised prefixes. Only relevant attributes for this
   * operation are `prefix` and `type`
   */
  void withdrawPrefixes(1: list<Lsdb.PrefixEntry> prefixes)
    throws (1: OpenrError error)

  /**
   * Withdraw prefixes in bulk by type (aka client-id)
   */
  void withdrawPrefixesByType(1: Network.PrefixType prefixType)
    throws (1: OpenrError error)

  /**
   * Sync prefixes by type. This operation set the new state for given type.
   * PrefixType specified in API parameter must match with individual
   * PrefixEntry object
   */
  void syncPrefixesByType(
    1: Network.PrefixType prefixType,
    2: list<Lsdb.PrefixEntry> prefixes) throws (1: OpenrError error)

  /**
   * Get all prefixes being advertised
   */
  list<Lsdb.PrefixEntry> getPrefixes() throws (1: OpenrError error)

  /**
   * Get prefixes of specific types
   */
  list<Lsdb.PrefixEntry> getPrefixesByType(1: Network.PrefixType prefixType)
    throws (1: OpenrError error)

  //
  // Route APIs
  //

  /**
   * Get route database of the current node. It is retrieved from FIB module.
   */
  Fib.RouteDatabase getRouteDb()
    throws (1: OpenrError error)

  /**
   * Get route database from decision module. Since Decision has global
   * topology information, any node can be retrieved
   */
  Fib.RouteDatabase getRouteDbComputed(1: string nodeName)
    throws (1: OpenrError error)

  /**
   * Get route database of the current node which are not installable.
   */
  Fib.RouteDatabase getRouteDbUnInstallable()
    throws (1: OpenrError error)

  //
  // Performance stats APIs
  //

  /**
   * Get latest performance events. Useful for debugging delays and performance
   * of Open/R.
   */

  Fib.PerfDatabase getPerfDb()
    throws (1: OpenrError error)

  //
  // Decision APIs
  //

  /**
   * Get adjacency databases of all nodes, representing topology from Decision
   * module. This represents currently active nodes (includes bi-directional
   * check)
   */
  Decision.AdjDbs getDecisionAdjacencyDbs() throws (1: OpenrError error)

  /**
   * Get global prefix databases. This represents prefixes of actives nodes
   * only. While KvStore can represent dead node's information until their keys
   * expires
   */
  Decision.PrefixDbs getDecisionPrefixDbs() throws (1: OpenrError error)

  //
  // HealthChecker APIs
  //

  /**
   * Get health-checker statistics per node
   */
  HealthChecker.HealthCheckerInfo getHealthCheckerInfo()
    throws (1: OpenrError error)

  //
  // KvStore APIs
  //

  /**
   * Get specific key-values from KvStore. If `filterKeys` is empty then no
   * keys will be returned
   */
  KvStore.Publication getKvStoreKeyVals(1: list<string> filterKeys)
    throws (1: OpenrError error)

  /**
   * Get raw key-values from KvStore with more control over filter
   */
  KvStore.Publication getKvStoreKeyValsFiltered(1: KvStore.KeyDumpParams filter)
    throws (1: OpenrError errror)

  /**
   * Get kvstore metadata (no values) with filter
   */
  KvStore.Publication getKvStoreHashFiltered(1: KvStore.KeyDumpParams filter)
    throws (1: OpenrError error)

  /**
   * Set/Update key-values in KvStore.
   */
  void setKvStoreKeyVals(1: KvStore.KeySetParams setParams)
    throws (1: OpenrError error)
  oneway void setKvStoreKeyValsOneWay(1: KvStore.KeySetParams setParams);

  /**
   * Send Dual message
   */
  void processKvStoreDualMessage(1: Dual.DualMessages messages)
    throws (1: OpenrError error)

  /**
   * Set flood-topology parameters. Called by neighbors
   */
  void updateFloodTopologyChild(1: KvStore.FloodTopoSetParams params)
    throws (1: OpenrError error)

  /**
   * Get spanning tree information
   */
  KvStore.SptInfos getSpanningTreeInfos() throws (1: OpenrError error);

  /**
   * Add/Update KvStore peer - usually not to be used by external peers unless
   * you know what you're doing.
   */
  void addUpdateKvStorePeers(1: KvStore.PeersMap peers)
    throws (1: OpenrError error)

  /**
   * Delete KvStore peers
   */
  void deleteKvStorePeers(1: list<string> peerNames)
    throws (1: OpenrError error)

  /**
   * Get KvStore peers
   */
  KvStore.PeersMap getKvStorePeers() throws (1: OpenrError error)

  //
  // LinkMonitor APIs
  //

  /**
   * Commands to set/unset overload bit. If overload bit is set then the node
   * will not do any transit traffic. However node will still be reachable in
   * the network from other nodes.
   */
  void setNodeOverload() throws (1: OpenrError error)
  void unsetNodeOverload() throws (1: OpenrError error)

  /**
   * Command to set/unset overload bit for interface. If overload bit is set
   * then no transit traffic will pass through the interface which is equivalent
   * to hard drain on the interface.
   */
  void setInterfaceOverload(1: string interfaceName)
    throws (1: OpenrError error)
  void unsetInterfaceOverload(1: string interfaceName)
    throws (1: OpenrError error)

  /**
   * Command to override metric for adjacencies over specific interfaces. This
   * can be used to emulate soft-drain of interfaces by using higher metric
   * value for link.
   *
   * Request must have valid `interfaceName` and `overrideMetric` values.
   */
  void setInterfaceMetric(1: string interfaceName, 2: i32 overrideMetric)
    throws (1: OpenrError error)
  void unsetInterfaceMetric(1: string interfaceName)
    throws (1: OpenrError error)

  /**
   * Command to override metric for specific adjacencies. Request must have
   * valid interface name, adjacency-node name and override metric value.
   */
  void setAdjacencyMetric(
    1: string interfaceName,
    2: string adjNodeName,
    3: i32 overrideMetric,
  ) throws (1: OpenrError error)
  void unsetAdjacencyMetric(1: string interfaceName, 2: string adjNodeName)
    throws (1: OpenrError error)

  /**
   * Get the current link status information
   */
  LinkMonitor.DumpLinksReply getInterfaces() throws (1: OpenrError error)

  /**
   * Command to request OpenR version
   */
  LinkMonitor.OpenrVersions getOpenrVersion() throws (1: OpenrError error)

  /**
   * Command to request build information
   * @deprecated - instead use getRegexExportedValues("build.*") API
   */
  LinkMonitor.BuildInfo getBuildInfo() throws (1: OpenrError error)

  //
  // PersistentStore APIs (query / alter dynamic configuration)
  //

  /**
   * Set new config key - you will never need to use it
   * NOTE: This API should only be accessible from local node
   */
  void setConfigKey(1: string key, 2: binary value) throws (1: OpenrError error)

  /**
   * Erase key from config
   * NOTE: This API should only be accessible from local node
   */
  void eraseConfigKey(1: string key) throws (1: OpenrError error)

  /**
   * Get config key
   */
  binary getConfigKey(1: string key) throws (1: OpenrError error)

  //
  // ZMQ Monitor APIs (get counters / log events)
  //

  /**
   * Get ZMQ log events
   */
  list<Monitor.EventLog> getEventLogs() throws (1: OpenrError error)
}
