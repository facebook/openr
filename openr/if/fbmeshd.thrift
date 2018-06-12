/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 facebook.fbmeshd
namespace py openr.fbmeshd

exception MeshServiceError {
  1: string message
} (message = "message")

//typedef list<Mesh> (cpp.type = "std::vector<Mesh>") MeshList

typedef map<string, i32>
(cpp.type = "std::unordered_map<std::string, int32_t>") PeerMetrics

struct Mesh {
  1: string meshId
  2: string ifName
  3: string channelType
  4: i32 frequency
  5: i32 channelTypeInt
  6: i32 channelWidth
  7: i32 centerFreq1
  12: bool encryptionEnabled
  8: optional i32 rssiThreshold
  9: optional string macAddress
  10: optional i32 ttl
  11: optional i32 elementTtl
}

struct MeshConfig {
  1: list<Mesh> meshList;
}

service MeshService {
  list<string> getPeers(1: string ifName)
    throws (1: MeshServiceError error)

  PeerMetrics getMetrics(1: string ifName)
    throws (1: MeshServiceError error)

  Mesh getMesh(1: string ifName)
    throws (1: MeshServiceError error)
}
