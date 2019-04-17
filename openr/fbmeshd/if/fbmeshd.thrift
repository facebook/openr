/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.fbmeshd.thrift
namespace py3 fbmeshd

exception MeshServiceError {
  1: string message
} (message = "message")

//typedef list<Mesh> (cpp.type = "std::vector<Mesh>") MeshList

typedef map<string, i32>
(cpp.type = "std::unordered_map<std::string, int32_t>") PeerMetrics

typedef i32 (cpp2.type = "int32_t") UInt32
typedef i64 (cpp2.type = "uint64_t") MacAddress // network byte order

struct SeparaPayload {
  1: MacAddress domain
  2: i32 metricToGate
  3: MacAddress desiredDomain
  4: optional bool enabled = true
}

struct Mesh {
  4: i32 frequency
  6: i32 channelWidth
  7: i32 centerFreq1
  12: optional i32 centerFreq2
  14: optional i32 txPower
}

struct MpathEntry {
  1: MacAddress dest
  2: MacAddress nextHop
  3: i64 sn
  4: i32 metric
  5: i64 expTime
  6: byte flags
  7: byte hopCount
  8: bool isRoot
  9: bool isGate
}

service MeshService {
  list<string> getPeers(1: string ifName)
    throws (1: MeshServiceError error)

  PeerMetrics getMetrics(1: string ifName)
    throws (1: MeshServiceError error)

  Mesh getMesh(1: string ifName)
    throws (1: MeshServiceError error)

  void setMetricOverride(1: string macAddress, 2: UInt32 metric)
  UInt32 getMetricOverride(1: string macAddress)
  i32 clearMetricOverride(1: string macAddress)

  list<MpathEntry> dumpMpath();
}

struct MeshPathFramePREQ {
  1: byte flags
  2: byte hopCount
  3: byte ttl
  4: i32 preqId
  5: i64 origAddr
  6: i64 origSn
  7: i32 lifetime
  8: i32 metric
  9: byte targetCount
  10: byte targetFlags
  11: i64 targetAddr
  12: i64 targetSn
}

struct MeshPathFramePREP {
  1: byte flags
  2: byte hopCount
  3: byte ttl
  4: i64 targetAddr
  5: i64 targetSn
  6: i32 lifetime
  7: i32 metric
  8: i64 origAddr
  9: i64 origSn
}

struct MeshPathFrameRANN {
  1: byte flags
  2: byte hopCount
  3: byte ttl
  4: i64 rootAddr
  5: i64 rootSn
  6: i32 interval
  7: i32 metric
}

struct MeshPathFramePANN {
  1: i64 origAddr
  2: i64 origSn
  3: byte hopCount
  4: byte ttl
  6: i64 targetAddr
  7: i32 metric
  8: bool isGate
  9: bool replyRequested
}
