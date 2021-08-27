# KvStore - Store and Sync

## Introduction

---

`KvStore` is the in-memory `key-value` datastore module of Open/R. It provides a
self-contained storage which aims for eventual consistency across the whole
network. Underlying implementation is based on **conflict-free replicated data
type (CRDT)**. The stores are inter-connected in a mesh, and synchronize their
contents in an eventually consistent fashion. The store is used to disseminate a
set of key-value pairs to all nodes in the network/cluster. For example, a node
may post information to its local store about its adjacent neighbors under a key
`adj:<node-name>` and this information will propagate to all other stores in the
network, under the same key name.

To limit the boundary of flooding domain, `KvStore` has multiple `KvStoreDb`
instances spawned based on the number of `AREA` configured when initialized.
Each `KvStoreDb` will do sync/update individually with its counterpart of peer
node. For `AREA` concept, see `Area.md` for more details.

## Inter Module Communication

---

![KvStore flow diagram](https://user-images.githubusercontent.com/10733132/130928965-f0f94f67-5d08-4e20-9b02-e491debfe89d.png)

- `[Producer] ReplicateQueue<Publication>`: propagate `thrift::Publication` and
  kvStoreSynced signal to local subscribers( i.e. `Decision`) for any delta
  update it notices. Those updates can come from either locally(e.g. prefix
  change from `PrefixManager` or adjacency change from `LinkMonitor`) or
  remotely.
- `[Producer] ReplicateQueue<KvStoreSyncEvent>`: publish `KvStoreSyncEvent` to
  `LinkMonitor` to indicate progress of initial full-sync between node and its
  peers.
- `[Consumer] RQueue<thrift::PeerUpdateRequest>`: receive **PEER SPEC**
  information from `LinkMonitor` to know how to establish TCP connection with
  peers over Thrift channel.
- `[Consumer] RQueue<KeyValueRequest>`: receive requests from `LinkMonitor` and
  `PrefixManager` to set and clear key-values in`KvStore`. Used for key-values
  originated by local node.

## Operations

---

Typical workflow at a high level will be:

- `KvStore` spawns one or multiple `KvStoreDb` based on number of AREAs
  configured in the system upon initialization. Then it waits for peer update.
- Peer **UP**:
  - `KvStore` receives **PEER SPEC** info(i.e. TCP port, link-local address
    etc.) and tries to establish TCP connection to fulfill initial sync of
    database. See later section for detail of state transition.
  - Regarding to **parallel adjacency** case, `LinkMonitor` will help manage all
    of the complexity and make it transparent to `KvStore`.
- Peer **DOWN**:
  - `KvStore` receives **DOWN** signal and close established TCP session. Clean
    up all data-structures for this peer and stop periodic syncing.

## Deep Dive

---

### KvStore Public APIs

`KvStore` supports multiple types of messages exchanged between peers over TCP.
Message exchange is fulfilled via public API and invoked between thrift client
and server. Some examples are:

```
/*
 * @params: area => single areaId to get K-V pairs from
 *          thrift::KeyGetParams => parameters to get specific K-V pairs
 * @return: thrift::Publication
 */
folly::SemiFuture<std::unique_ptr<thrift::Publication>>
semifuture_getKvStoreKeyVals(std::string area, thrift::KeyGetParams keyGetParams)

/*
 * @params: area => single areaId to set K-V pairs
 *          thrift::KeySetParams => parameters to set specific K-V pairs
 * @return: folly::Unit
 */
folly::SemiFuture<folly::Unit>
semifuture_setKvStoreKeyVals(std::string area, thrift::KeySetParams keySetParams)

/*
 * @params: area => single areaId to set K-V pairs
 *          thrift::KeyDumpParams => parameters to dump ALL K-V pairs
 * @return: thrift::Publication WITHOUT `value`(serialized binary data)
 */
folly::SemiFuture<std::unique_ptr<std::vector<thrift::Publication>>>
semifuture_dumpKvStoreHashes(std::string area, thrift::KeyDumpParams keyDumpParams);

/*
 * @params: selectAreas => set of areas to dump keys from
 *          thrift::KeySetParams => parameters to set K-V pairs
 * @return: thrift::Publication
 */
folly::SemiFuture<std::unique_ptr<std::vector<thrift::Publication>>>
semifuture_dumpKvStoreKeys(thrift::KeyDumpParams keyDumpParams,
       std::set<std::string> selectAreas = {});

/*
 * @params: area => area to dump self-originated K-V pairs from
 * @return: SelfOriginatedKeyVals => map of K-V pairs
 */
folly::SemiFuture<std::unique_ptr<SelfOriginatedKeyVals>>
  semifuture_dumpKvStoreSelfOriginatedKeys(std::string area);

/*
 * @params: area => area to dump KvStore peers from
 * @return: thrift::PeersMap => map of peers KvStore is subscribed to
 */
folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
  semifuture_getKvStorePeers(std::string area);

/*
 * @params: area => area to add/update KvStore peers to
 * @return: folly::Unit
 */
folly::SemiFuture<folly::Unit> semifuture_addUpdateKvStorePeers(
  std::string area, thrift::PeersMap peersToAdd);

/*
 * @params: area => area to delete KvStore peers from
 * @return: folly::Unit
 */
folly::SemiFuture<folly::Unit> semifuture_deleteKvStorePeers(
  std::string area, std::vector<std::string> peersToDel);

/*
 * @params: area => area to dump flood topology
 * @return: thrift::SptInfos => Spanning Tree Infos for given area
 */
folly::SemiFuture<std::unique_ptr<thrift::SptInfos>>
  semifuture_getSpanningTreeInfos(std::string area);

/*
 * @params: area => area to update flood topology process
 *          thrift::FloodTopoSetParams => params to set/unset a child for a
 *                                        given root
 * @return: folly::Unit
 */
folly::SemiFuture<folly::Unit> semifuture_updateFloodTopologyChild(
  std::string area, thrift::FloodTopoSetParams floodTopoSetParams);

/*
 * @params: area => area to send dual messages to
 *          thrift::DualMessages => Dual messages for DualNode (KvStoreDb) to
 *                                  process
 * @return: folly::Unit
 */
folly::SemiFuture<folly::Unit> semifuture_processKvStoreDualMessage(
  std::string area, thrift::DualMessages dualMessages);

/*
 * @params: selectAreas => set of areas to retrieve area summary
 * @return: thrift::KvStoreAreaSummary => counters, key-vals, peers, etc.
 */
folly::SemiFuture<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>>
  semifuture_getKvStoreAreaSummaryInternal(
      std::set<std::string> selectAreas = {});

/*
 * @return: std::map<std::string, int64_t> => num_keys, num_peers, etc.
 */
folly::SemiFuture<std::map<std::string, int64_t>> semifuture_getCounters();

/*
 * @return: messaging::RQueue<Publication> => read-only interface for KvStore
 *                                            updates queue
 */
messaging::RQueue<Publication> getKvStoreUpdatesReader();

/*
 * @params: area => area to retrieve current KvStore peer state from
 *          peerName => name of peer for which to fetch current peer state
 * @return: thrift::KvStorePeerState
 */
folly::SemiFuture<std::optional<thrift::KvStorePeerState>>
  semifuture_getKvStorePeerState(
      std::string const& area, std::string const& peerName);
```

For more information about `KvStore` parameters, check out

- [if/Types.thrift](https://github.com/facebook/openr/blob/master/openr/if/Types.thrift)

### KvStore Sync via Thrift

There are three types of sychronization message exchanged inside `KvStore`:

- Initial Full Sync
- Incremental Updates, aka, flooding
- Finalized Full Sync

#### Initial Full Sync - Finite State Machine (FSM)

Intitial full sync with a peer is performed when it is added to the local store.

![State Machine Transition](https://user-images.githubusercontent.com/51382140/102559618-1f9ad180-4085-11eb-91c9-0605765da97b.png)

`KvStore` leverages FSM to track and update initial db full-sync with individual
peers. FSM ensures the clean state representation of peer and makes this
event-driven syncing easy to track and control.

```
KvStorePeerState
- IDLE        => fresh and not yet connected(default)
- SYNCING     => synchronizing with peer
- INITIALIZED => initial 3-way sync done

KvStorePeerEvent
- PEER_ADD          => new peer_spec received
- PEER_DEL          => peer is removed
- SYNC_RESP_RCVD    => initial db full-sync response received
- THRIFT_API_ERROR  => error/timeout/failure for initial sync
```

#### Incremental Updates - Flooding Update

All incremental changes in local KvStore are published as `thrift::Publication`
messages containing changes. All received incremental changes are processed and
applied locally and conditionally forwarded. The forwarding is done via thrift
client-server channel. Whenevenr an update is received, it is applied locally
and then forwarded to all neighbors. An update is ignored when it is echoed back
to avoid infinite loop of control packet. This is done via `originatorId`.

![flooding via thrift](https://user-images.githubusercontent.com/51382140/102559861-b4053400-4085-11eb-9dbc-0890ae0b4f75.png)

#### Finalized Full Sync - Part of 3 way sync

No matter a syncing request comes from either side of two peers, `KvStore` will
make sure both sides reach eventual consistency. So it implements **finalized
full-sync** mechanism to notify missing keys from peer's perspective.

For example, let's say we have K-V with format of **(key, value, version)**:

```
NodeA has: (k0, a, 1), (k1, a, 1), (k2, a, 2), (k3, a, 1)
NodeB has:             (k1, a, 1), (k2, b, 1), (k3, b, 2), (k4, b, 1)
```

Two cases:

- A initiates a full-sync with B:
  - A requests to get full dump K-V database of B by providing hashes of local
    store;
  - B replies all of its K-V pairs to A with: **thriftPub.keyVals**: k3(B has
    higher version), k4(A doesn't have) **thriftPub.toBeUpdatedKeys**: k0(B
    doesn't have), k2(A has higher version);
  - A noticed B needs to be updated with **k0** and **k2**. Send back with
    finalized full-sync;
- B initiates a full-sync with A:
  - Similar logic to follow 3-way sync;

### Implementation Details

#### Loop detection

To avoid blind flooding, the KV store implements loop detection logic similar to
BGP. We assign every KV store an "originatorId" which is unique in the system.
When a store receives a message, it checks the originatorId, and if it matches
then flooding stops. This allows one to design efficient flooding topologies and
avoid excessive message duplication in the mesh.

#### Data Encoding

One prominent feature is that all values are opaquely encoded as Thrift objects
using client's choice of protocol (though this is not strictly required). The
data-store itself does not care about the value contents, it only needs to be
told if two values are different, when it propagates them. At the same time, on
the client side, this approach removes the burden of protocol encoding/decoding
by using our standard Thrift libs.

#### Versioning of K-V pairs

We implement very simple versions for merge conflict resolution. Every key has a
**64-bit** version value, which is compared to the incoming update message. Only
if the incoming version is greater will we update the local store and flood the
original update message to our subscribers (peers). Notice that we'll preserve
the original version in the flooded message so that other folks can compare
their versions with the original submission.

#### Delete Operation

KvStore only supports `Add` or `Update` operations. Implementing `Delete`
operation in CRDT is non-trivial and not well defined in eventual consistent
fashion. For all practical purposes, `Delete` operation can be acheived via
optional `time-to-live` aka `ttl` field, which indicates the lifetime of
key-value.

#### Time-To-Live(ttl)

When advertising a key-value, set the `ttl` to a specified amount of time and
submit to the local store. On update, the local store will flood it to all other
stores in the network. Periodically every store will scan locally stored keys
and decrement the ttl with the elapsed time. If the `ttl` drops below `0`, the
key is removed from the local store. Since every node does the same operation,
the key is deleted from all stores. When key-dump is requested, then an updated
ttl is sent (received - elapsed time) reflecting the remaining lifetime of a
key-value since it's origination.

#### ttl updates

In order to keep key-values with limited lifetime persisted for long duration,
originators emit `ttl updates` with new ttl values. On receipt of a ttl update
(with higher version), the ttl of a key in local store is updated and the ttl
update is flooded to its neighbors.

`KvStore` provides two different ways for `ttl updates`:

1. Self-originated keys with link-state information. These `ttl updates` are
   completely managed by KvStore.
2. Non-self-originated keys (e.g. key-vals injected from user for global view
   and eventual consistency). User is responsible for refreshing `ttl updates`
   periodically and updating ttlVersion on their own.

#### Key Expiry Notifications

Whenever keys are expired in a given KvStore, the notification is generated and
published via thrift channel. All subscribers can take appropriate action to
handle an expired key (for e.g. Decision removes adj/prefix DB of nodes). These
notifications are ignored by other KvStores as they will be generating the very
same notifications by themselves.

#### Self-originated key-values

All link-state protocol related key-values originated by the local node are sent
to `KvStore` via key-value requests. There are 3 key-value request types:

- `Set` stores the key-value and adds it to a cache used to generate
  `ttl updates` to maintain the key-value in `KvStore` for longer durations.
- `Persist` does the same as `Set` while also ensuring that the key-value
  submitted doesn't get overriden by another node.
- `Clear` removes from the cache, which ends `ttl updates` and allows the key to
  expire.

`KvStore` stores key-values with newer versions to ensure new or updated
self-originated key-values make it to the network and are NOT **overriden** by
other nodes.

#### KvStoreClientInternal

`KvStore` is core and a heavily used module in Open/R. Interacting with
`KvStore` involves sending and receiving proper thrift objects on sockets. This
was leading to a lot of complexity in the code. `KvStoreClientInternal` is added
to address this concern. It provides APIs to subscribe/unsubscribe to key-value
changes within `KvStore`. A client uses these APIs to call a callback function
when the value associated with a key changes.

### More Reading

- [Conflict Free Replicated Data Type (CRDT)](https://www.wikiwand.com/en/Conflict-free_replicated_data_type)
