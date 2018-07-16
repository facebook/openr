/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace php Openr
namespace py openr.LinkMonitor

include "Lsdb.thrift"
include "Spark.thrift"

//
// LinkMonitor provides simple API to drain/undrain the node
// and to dump all known interfaces of the node
//

enum LinkMonitorCommand {
  /**
   * Commands to set/unset overload bit. If overload bit is set then the node
   * will not do any transit traffic. However node will still be reachable in
   * the network from other nodes.
   */
  SET_OVERLOAD    = 1,    // No response will be sent
  UNSET_OVERLOAD  = 2,    // No response will be sent

  /**
   * Get the current link status information
   */
  DUMP_LINKS      = 3,    // DumpLinksReply will be sent back

  /**
   * Command to set/unset overload bit for link. If overload bit is set then
   * no transit traffic will pass through the link which is equivalent to
   * hard drain on the link.
   */
  SET_LINK_OVERLOAD   = 4,  // No response will be sent
  UNSET_LINK_OVERLOAD = 5,  // No response will be sent

  /**
   * Command to override metric for adjacencies over specific interfaces. This
   * can be used to emulate soft-drain of links by using higher metric value
   * for link.
   *
   * Request must have valid `interfaceName` and `overrideMetric` values for
   * SET command. UNSET command only expects `interfaceName`.
   */
  SET_LINK_METRIC     = 6,  // No response will be sent
  UNSET_LINK_METRIC   = 7,  // No response will be sent

  /**
   * Command to override metric for specific adjacencies.
   *
   * Request must have valid 'adjacency' node name
   */
  SET_ADJ_METRIC     = 8,  // No response will be sent
  UNSET_ADJ_METRIC   = 9,  // No response will be sent

  /**
   * Command to request OpenR version
   */
   GET_VERSION = 10, // replies with OpenrVersions

  /**
   * Command to request build information
   */
  GET_BUILD_INFO = 11,  // replies with OpenrBuildInfo
}

struct LinkMonitorRequest {
 1: LinkMonitorCommand cmd
 2: string interfaceName
 3: i32 overrideMetric = 1  # Default value (can't be less than 1)
 4: optional string adjNodeName
}

struct OpenrVersions {
 1: Spark.OpenrVersion version
 2: Spark.OpenrVersion lowestSupportedVersion
}

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
struct LinkMonitorConfig {
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

/**
 * Struct representing build information. Attributes are described in detail
 * in `openr/common/BuildInfo.h`
 */
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
