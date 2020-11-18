/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.KvStore
namespace php Openr
namespace py openr.KvStore
namespace py3 openr.thrift
namespace lua openr.KvStore
namespace wiki Open_Routing.Thrift_APIs

include "Dual.thrift"

const string kDefaultArea = "0"

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

typedef map<string, Value>
  (cpp.type = "std::unordered_map<std::string, openr::thrift::Value>") KeyVals


enum Command {
  // operations on keys in the store
  KEY_SET   = 1,
  KEY_DUMP  = 3,
  DUAL = 10, // DUAL message
  FLOOD_TOPO_SET = 11, // set or unset flooding-topo child
}

enum FilterOperator {
  // Logical operation between originator ids and keys
  OR = 1,
  AND = 2,
}


struct KeySetParams {
  // NOTE: the struct is denormalized on purpose,
  // it may happen so we repeat originatorId
  // too many times...

  // NOTE: we retain the original number in thrift struct
  2: KeyVals keyVals;

  // Solicit for an ack. This can be set to false to make requests one-way
  3: bool solicitResponse = 1;

  // Optional attributes. List of nodes through which this publication has
  // traversed. Client shouldn't worry about this attribute.
  5: optional list<string> nodeIds;

  // optional flood root-id, indicating which SPT this publication should be
  // flooded on; if none, flood to all peers
  6: optional string floodRootId;

  // optional attribute to indicate timestamp when request is sent. This is
  // system timestamp in milliseconds since epoch
  7: optional i64 timestamp_ms
}

struct KeyGetParams {
  1: list<string> keys
}

// KeyDumpParams specifies which
struct KeyDumpParams {
  1: string prefix    (deprecated)
  3: set<string> originatorIds
  // if set to true (default), ignore TTL updates
  // This field is more for subscriptions.
  6: bool ignoreTtl = true

  // if set to true, Value.value binary will not be published in Value
  7: bool doNotPublishValue = false

  // optional attribute to include keyValHashes information from peer.
  //  1) If NOT empty, ONLY respond with keyVals on which hash differs;
  //  2) Otherwise, respond with flooding element to signal DB change;
  2: optional KeyVals keyValHashes

  // The default is OR for dumping KV store entries for backward compatibility.
  // The default will be changed to AND later. We can also make `oper`
  // mandatory later. The default for subscription is AND now.
  4: optional FilterOperator oper
  // Keys to subscribe to in KV store so that consumers receive only certain
  // kinds of updates. For example, a consumer might be interesred in
  // getting "adj:.*" keys from open/r domain.
  5: optional list<string> keys;
}

// Peer's publication and command socket URLs
// This is used in peer add requests and in
// the dump results
struct PeerSpec {
  // peer address over thrift for KvStore external sync
  1: string peerAddr

  // cmd url for KvStore external sync over ZMQ
  2: string cmdUrl

  // thrift port
  4: i32 ctrlPort = 0
}

typedef map<string, PeerSpec>
  (cpp.type = "std::unordered_map<std::string, openr::thrift::PeerSpec>")
  PeersMap

// parameters for peer addition
struct PeerAddParams {
  // map from nodeName to peer spec; we expect to
  // learn nodeName from HELLO packets, as it MUST
  // match the name supplied with Publication message
  1: PeersMap peers
}

// parameters for peers deletion
struct PeerDelParams {
  1: list<string> peerNames
}

// peer updateRequest
struct PeerUpdateRequest {
  1: string area
  2: optional PeerAddParams peerAddParams
  3: optional PeerDelParams peerDelParams
}

// set/unset flood-topo child
struct FloodTopoSetParams {
  // spanning tree root-id
  1: string rootId
  // from node-id
  2: string srcId
  // set/unset a spanning tree child
  3: bool setChild
  // action apply to all-roots or not
  // if true, rootId will be ignored and action will be applied to all roots
  4: optional bool allRoots
}

typedef set<string>
  (cpp.type = "std::unordered_set<std::string>") PeerNames

// single spanning tree information
struct SptInfo {
  // passive state or not
  1: bool passive
  // metric cost towards root
  2: i64 cost
  // optional parent if any (aka nexthop)
  3: optional string parent
  // a set of spt children
  4: PeerNames children
}

// map<root-id: SPT-info>
typedef map<string, SptInfo>
  (cpp.type = "std::unordered_map<std::string, openr::thrift::SptInfo>")
  SptInfoMap

// all spanning tree(s) information
struct SptInfos {
  // map<root-id: SptInfo>
  1: SptInfoMap infos
  // all DUAL related counters
  2: Dual.DualCounters counters
  // current flood-root-id if any
  3: optional string floodRootId
  // current flooding peers
  4: PeerNames floodPeers
}

//
// KvStoreRequest specification
//

// a request to the server (tagged union)
struct KvStoreRequest {
  // Command type. Set one of the optional parameter based on command
  1: Command cmd

  // area identifier to identify the KvStoreDb instance (mandatory)
  11: string area

  2: optional KeySetParams keySetParams
  3: optional KeyGetParams keyGetParams
  6: optional KeyDumpParams keyDumpParams
  9: optional Dual.DualMessages dualMessages
  10: optional FloodTopoSetParams floodTopoSetParams
}

//
// Responses
//
// this is also used to respond to GET requests
struct Publication {
  // NOTE: the numbering is on purpose, to maintain backward compatibility
  2: KeyVals keyVals;
  // expired keys
  3: list<string> expiredKeys;

  // Optional attributes. List of nodes through which this publication has
  // traversed. Client shouldn't worry about this attribute.
  4: optional list<string> nodeIds;

  // a list of keys that needs to be updated
  // this is only used for full-sync respone to tell full-sync initiator to
  // send back keyVals that need to be updated
  5: optional list<string> tobeUpdatedKeys;

  // optional flood root-id, indicating which SPT this publication should be
  // flooded on; if none, flood to all peers
  6: optional string floodRootId;

  // area to which this publication belongs
  7: string area;
}
