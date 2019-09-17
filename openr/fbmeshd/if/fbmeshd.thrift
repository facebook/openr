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

typedef i64 (cpp2.type = "uint64_t") MacAddress // network byte order
typedef byte (cpp2.type = "uint8_t") u8
typedef i32 (cpp2.type = "uint32_t") u32
typedef i64 (cpp2.type = "uint64_t") u64

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
  3: u64 sn
  4: u32 metric
  5: u64 expTime
  6: u32 nextHopMetric
  7: u8 hopCount
  8: bool isRoot
  9: bool isGate
}

struct StatCounter {
  1: string key
  2: i64 value
}

service MeshService {
  list<string> getPeers(1: string ifName)
    throws (1: MeshServiceError error)

  PeerMetrics getMetrics(1: string ifName)
    throws (1: MeshServiceError error)

  Mesh getMesh(1: string ifName)
    throws (1: MeshServiceError error)

  list<StatCounter> dumpStats()

  list<MpathEntry> dumpMpath();
}

struct MeshPathFramePANN {
  1: MacAddress origAddr
  2: u64 origSn
  3: u8 hopCount
  4: u8 ttl
  6: MacAddress targetAddr
  7: u32 metric
  8: bool isGate
  9: bool replyRequested
}

/*
* rnl thrift objects
*/

enum MplsActionCode {
  PUSH = 0
  SWAP = 1
  PHP = 2      # Pen-ultimate hop popping => POP and FORWARD
  POP_AND_LOOKUP = 3
}

const map<i16, i16> protocolIdtoPriority = {99:10, 253:20, 64:11}
const i16 kUnknownProtAdminDistance = 255
