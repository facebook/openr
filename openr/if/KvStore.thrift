/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace php Openr
namespace py openr.KvStore

typedef map<string, Value>
  (cpp.type = "std::unordered_map<std::string, Value>") KeyVals

typedef map<string, PeerSpec>
  (cpp.type = "std::unordered_map<std::string, PeerSpec>") PeersMap


// a value as reported in get replies/publications
struct Value {
  // current version of this value
  1: i64 version;
  // the node that set the value
  // NOTE: the numbering is on purpose to maintain
  // backward compat
  3: string originatorId
  // the actual value: nonexistent for TTL update
  2: optional binary value;
  // TTL in milliseconds
  4: i64 ttl;
  // current version of this TTL, used to update TTL
  // TTL can be updated to prevent a key from expiring and being deleted
  // Newer TTL is represented by higher ttlVersion
  // Reset to 0 when "value version" increments
  5: i64 ttlVersion = 0;
  // Hash associated with `tuple<version, originatorId, value>`. Clients
  // should leave it empty and as will be computed by KvStore on `KEY_SET`
  // operation.
  6: optional i64 hash;
}

enum Command {
  // operations on keys in the store
  KEY_SET   = 1,
  KEY_GET   = 2,
  KEY_DUMP  = 3,      // dump keys only if TTL balance > kTtlThreshold
  KEY_DUMP_ALL  = 10, // dump keys regardless of TTL balance
  HASH_DUMP = 7,
  COUNTERS_GET = 9,   // Return object will be Monitor::CounterMap

  // operations on the store peers
  PEER_ADD  = 4,
  PEER_DEL  = 5,
  PEER_DUMP = 6,
}

//
// Cmd params
//

// parameters for the KEY_SET command
struct KeySetParams {
  // NOTE: the struct is denormalized on purpose,
  // it may happen so we repeat originatorId
  // too many times...

  // NOTE: we retain the original number in thrift struct
  2: KeyVals keyVals;

  // Solicit for an ack. This can be set to false to make requests one-way
  3: bool solicitResponse = 1;

  // Name of node who is originating set key request
  4: optional string originatorId;
}

// parameters for the KEY_GET command
struct KeyGetParams {
  1: list<string> keys
}

// parameters for the KEY_DUMP command
// if request includes keyValHashes information from peer, only respsond with
// keyVals on which hash differs
// if keyValHashes is not specified, respond with flooding element to signal of
// DB change
struct KeyDumpParams {
  1: string prefix
  3: set<string> originatorIds
  2: optional KeyVals keyValHashes
}

// Peer's publication and command socket URLs
// This is used in peer add requests and in
// the dump results
struct PeerSpec {
  1: string pubUrl
  2: string cmdUrl
}

// parameters for peer addition
struct PeerAddParams {
  // map from nodeName to peer spec; we expect to
  // learn nodeName from HELLO packets, as it MUST
  // match the name supplied with Publication message
  1: PeersMap peers
}

// peers to delete
struct PeerDelParams {
  1: list<string> peerNames
}

//
// Request specification
//

// a request to the server (tagged union)
struct Request {
  1: Command cmd
  2: KeySetParams keySetParams
  3: KeyGetParams keyGetParams
  6: KeyDumpParams keyDumpParams
  4: PeerAddParams peerAddParams
  5: PeerDelParams peerDelParams
}

//
// Responses
//
// this is also used to respond to GET requests
struct Publication {
  // NOTE: the numbering is on purpose, maintaining
  // backward compatibility
  2: KeyVals keyVals;
  // expired keys
  3: list<string> expiredKeys;
}

// Dump of the current peers: sent in
// response to any PEER_ command, so the
// caller can see the result of the request
struct PeerCmdReply {
  1: PeersMap peers
}
