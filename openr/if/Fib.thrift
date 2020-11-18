/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.Fib
namespace php Openr
namespace py openr.Fib
namespace py3 openr.thrift
namespace lua openr.Fib
namespace wiki Open_Routing.Thrift_APIs

include "Network.thrift"
include "Lsdb.thrift"

struct RouteDatabase {
  1: string thisNodeName
  3: optional Lsdb.PerfEvents perfEvents;
  4: list<Network.UnicastRoute> unicastRoutes
  5: list<Network.MplsRoute> mplsRoutes
}

struct RouteDatabaseDelta {
  2: list<Network.UnicastRoute> unicastRoutesToUpdate
  3: list<Network.IpPrefix> unicastRoutesToDelete;
  4: list<Network.MplsRoute> mplsRoutesToUpdate
  5: list<i32> mplsRoutesToDelete
  6: optional Lsdb.PerfEvents perfEvents;
}

// Perf log buffer maintained by Fib
struct PerfDatabase {
  1: string thisNodeName
  2: list<Lsdb.PerfEvents> eventInfo
}
