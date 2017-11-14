/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.Decision

include "Fib.thrift"
include "Lsdb.thrift"

typedef map<string, Lsdb.AdjacencyDatabase>
  (cpp.type = "std::unordered_map<std::string, AdjacencyDatabase>") AdjDbs

typedef map<string, Lsdb.PrefixDatabase>
  (cpp.type = "std::unordered_map<std::string, PrefixDatabase>") PrefixDbs


// query info in Decision
enum DecisionCommand {
  // route database for a given node
  ROUTE_DB_GET = 1,
  // adjacency databases
  ADJ_DB_GET = 2,
  // prefix databases
  PREFIX_DB_GET = 3,
}

struct DecisionRequest {
  1: DecisionCommand cmd
  // If nodeName is empty then current node's routes will be returned in
  // response. Only applies to ROUTE_DB_GET
  2: string nodeName
}

struct DecisionReply {
  1: Fib.RouteDatabase routeDb
  2: AdjDbs adjDbs
  3: PrefixDbs prefixDbs
}
