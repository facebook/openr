/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.LinkMonitor
namespace php Openr
namespace py openr.LinkMonitor
namespace py3 openr.thrift
namespace lua openr.LinkMonitor
namespace wiki Open_Routing.Thrift_APIs

include "Lsdb.thrift"

struct InterfaceDetails {
  1: Lsdb.InterfaceInfo info
  2: bool isOverloaded
  3: optional i32 metricOverride
  4: optional i64 linkFlapBackOffMs
}

struct DumpLinksReply {
 1: string thisNodeName
 3: bool isOverloaded
 6: map<string, InterfaceDetails>
        (cpp.template = "std::unordered_map") interfaceDetails
}

struct AdjKey {
  1: string nodeName;
  2: string ifName;
}

//
// Struct to store internal override states for links (e.g. metric, overloaded
// state) etc.
// NOTE: This is not currently exposed via any API
//
struct LinkMonitorState {
  // Overload bit for Open-R. If set then this node is not available for
  // transit traffic at all.
  1: bool isOverloaded = 0;

  // Overloaded links. If set then no transit traffic will pass through the
  // link and will be unreachable.
  2: set<string> overloadedLinks;

  // Custom metric override for links. Can be leveraged to soft-drain interfaces
  // with higher metric value.
  3: map<string, i32> linkMetricOverrides;

  // Label allocated to node (via RangeAllocator).
  // `0` indicates null value (no value allocated)
  4: i32 nodeLabel = 0;

  // Custom metric override for adjacency
  5: map<AdjKey, i32> adjMetricOverrides;
}

//
// Struct representing build information. Attributes are described in detail
// in `openr/common/BuildInfo.h`
//
struct BuildInfo {
  1: string buildUser;
  2: string buildTime;
  3: i64 buildTimeUnix;
  4: string buildHost;
  5: string buildPath;
  6: string buildRevision;
  7: i64 buildRevisionCommitTimeUnix;
  8: string buildUpstreamRevision;
  9: i64 buildUpstreamRevisionCommitTimeUnix;
  10: string buildPackageName;
  11: string buildPackageVersion;
  12: string buildPackageRelease;
  13: string buildPlatform;
  14: string buildRule;
  15: string buildType;
  16: string buildTool;
  17: string buildMode;
}
