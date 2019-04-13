/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.OpenrCtrl

include "common/fb303/if/fb303.thrift"
include "Decision.thrift"
include "Fib.thrift"

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
}
