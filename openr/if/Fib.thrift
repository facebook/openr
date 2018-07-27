/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace php Openr
namespace py openr.Fib

include "IpPrefix.thrift"
include "Lsdb.thrift"

//
// The following is used to publish routes
// computed for the FIB module
//

//
// Loop free path
//
struct Path {
  1: IpPrefix.BinaryAddress nextHop
  2: string ifName
  3: i32 metric
}

// There could be multiple routes to same prefix
// with different next-hops and metrics. All of
// them are guaranteed to be loop-free, that is,
// all could be used in multipath fashion
struct Route {
  1: IpPrefix.IpPrefix prefix
  // in theory, the same path may repeat multiple times
  // and this could be used for weighted load-sharing..
  2: list<Path> paths
}

// announced under keys starting with "routes:"
struct RouteDatabase {
  // must match the name in the key
  1: string thisNodeName
  2: list<Route> routes
  3: optional Lsdb.PerfEvents perfEvents;
}

// Perf log buffer maintained by Fib
struct PerfDatabase {
  1: string thisNodeName
  2: list<Lsdb.PerfEvents> eventInfo
}

enum FibCommand {
  ROUTE_DB_GET = 1,
  PERF_DB_GET = 2,
}

struct FibRequest {
  1: FibCommand cmd
}
