/*
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
namespace lua openr.OpenrCtrl

include "fb303/thrift/fb303_core.thrift"
include "fbzmq/service/if/Monitor.thrift"
include "Decision.thrift"
include "Dual.thrift"
include "Fib.thrift"
include "KvStore.thrift"
include "LinkMonitor.thrift"
include "Lsdb.thrift"
include "Network.thrift"
include "OpenrConfig.thrift"

exception OpenrError {
  1: string message
} ( message = "message" )

struct StaticRoutes {
  1: map<i32,list<Network.NextHopThrift>> mplsRoutes;
}


/**
 * Thrift service - exposes RPC APIs for interaction with all of Open/R's
 * modules.
 */
service OpenrCtrl extends fb303_core.BaseService {

  //
  // Config APIs
  //
  /**
   * get string config
   */
  string getRunningConfig()

  /**
   * get config in thrift
   */
  OpenrConfig.OpenrConfig getRunningConfigThrift()

  /**
   * Load file config and do validation. Throws exception upon error.
   * Return - loaded config content.
   *
   * NOTE: json seriliazer ommit extra fields.
   * loaded content is returned so user could verify the difference between
   * file content and loaded content
   */
  string dryrunConfig(1: string file)
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
   * topology information, any node can be retrieved.
   *
   * NOTE: Current node's routes are returned if `nodeName` is empty.
   */
  Fib.RouteDatabase getRouteDbComputed(1: string nodeName)
    throws (1: OpenrError error)

  /**
   * Get unicast routes after applying a list of prefix filter.
   * Perform longest prefix match for each input filter among the prefixes
   * in from FIB module.
   * Return all unicast routes if the input list is empty.
   */
  list<Network.UnicastRoute> getUnicastRoutesFiltered(1: list<string> prefixes)
    throws (1: OpenrError error)

  /**
   * Get all unicast routes of the current node, retrieved from FIB module.
   */
  list<Network.UnicastRoute> getUnicastRoutes()
    throws (1: OpenrError error)

  /**
   * Get Mpls routes after applying a list of prefix filter.
   * Return all Mpls routes if the input list is empty.
   */
  list<Network.MplsRoute> getMplsRoutesFiltered(1: list<i32> labels)
    throws (1: OpenrError error)

  /**
   * Get all Mpls routes of the current node, retrieved from FIB module.
   */
  list<Network.MplsRoute> getMplsRoutes()
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
  // Get area feature configuration
  //
  KvStore.AreasConfig getAreasConfig()
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
   * with area option
   */
  KvStore.Publication getKvStoreKeyValsArea(
    1: list<string> filterKeys,
    2: string area =  KvStore.kDefaultArea
  ) throws (1: OpenrError error)

  /**
   * Get raw key-values from KvStore with more control over filter
   */
  KvStore.Publication getKvStoreKeyValsFiltered(1: KvStore.KeyDumpParams filter)
    throws (1: OpenrError error)

  /**
   * Get raw key-values from KvStore with more control over filter with 'area'
   * option
   */
  KvStore.Publication getKvStoreKeyValsFilteredArea(
    1: KvStore.KeyDumpParams filter,
    2: string area = KvStore.kDefaultArea
  ) throws (1: OpenrError error)

  /**
   * Get kvstore metadata (no values) with filter
   */
  KvStore.Publication getKvStoreHashFiltered(1: KvStore.KeyDumpParams filter)
    throws (1: OpenrError error)

  /**
   * with area
   */
  KvStore.Publication getKvStoreHashFilteredArea(
    1: KvStore.KeyDumpParams filter,
    2: string area =  KvStore.kDefaultArea
  ) throws (1: OpenrError error)

  /**
   * Set/Update key-values in KvStore.
   */
  void setKvStoreKeyVals(
    1: KvStore.KeySetParams setParams,
    2: string area = KvStore.kDefaultArea
  ) throws (1: OpenrError error)

  /**
   * Long poll API to get KvStore
   * Will return true/false with our own KeyVal snapshot provided
   */
  bool longPollKvStoreAdj(1: KvStore.KeyVals snapshot)
    throws (1: OpenrError error)

  /**
   * Send Dual message
   */
  void processKvStoreDualMessage(
    1: Dual.DualMessages messages
    2: string area = KvStore.kDefaultArea
  ) throws (1: OpenrError error)

  /**
   * Set flood-topology parameters. Called by neighbors
   */
  void updateFloodTopologyChild(
    1: KvStore.FloodTopoSetParams params,
    2: string area = KvStore.kDefaultArea
  ) throws (1: OpenrError error)

  /**
   * Get spanning tree information
   */
  KvStore.SptInfos getSpanningTreeInfos(
    1: string area
  ) throws (1: OpenrError error);

  /**
   * Get KvStore peers
   */
  KvStore.PeersMap getKvStorePeers() throws (1: OpenrError error)

  KvStore.PeersMap getKvStorePeersArea(
    1: string area
  ) throws (1: OpenrError error)

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
   * Get the current adjacencies information
   */
  Lsdb.AdjacencyDatabase getLinkMonitorAdjacencies()
    throws (1: OpenrError error)

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

  // Get Openr Node Name
  string getMyNodeName()
}
