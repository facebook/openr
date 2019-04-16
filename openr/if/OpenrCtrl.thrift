/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace py openr.OpenrCtrl
namespace php Openr

include "common/fb303/if/fb303.thrift"
include "Decision.thrift"
include "Dual.thrift"
include "Fib.thrift"
include "HealthChecker.thrift"
include "KvStore.thrift"

enum OpenrModuleType {
  DECISION = 1,
  FIB = 2,
  HEALTH_CHECKER = 3,
  KVSTORE = 4,
  LINK_MONITOR = 5,
  PREFIX_ALLOCATOR = 6,
  PREFIX_MANAGER = 7,
  SPARK = 8,
}

exception OpenrError {
  1: string message
} ( message = "message" )

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
   * Get raw key-values from KvStore. If `filterKeys` is empty then all key-vals
   * are returned else only requested keys will be returned.
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
  KvStore.SptInfo getSpanningTreeInfo() throws (1: OpenrError error);

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

}
