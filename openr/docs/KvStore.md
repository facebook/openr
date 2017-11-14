`KvStore - Store and Sync`
--------------------------

`KvStore` provides self-contained, in-memory `key-value` datastore which is
eventually consistent. Underlying implementation is based on  **conflict-free
replicated data type (CRDT)**. The stores are interconnected in a mesh, and
synchronize their contents in eventually consistent fashion. This store is used
to disseminate set of key-value pairs to all nodes in the network/cluster. For
example, a node may post information to its local store about its adjacent
neighbors under a key `adj:myRouteName` and this information will propagate to
all other stores in the network, under the same key name.

### APIs
---

For more information about message formats, checkout
- [if/KvStore.thrift](https://github.com/facebook/openr/blob/master/openr/if/KvStore.thrift)

#### ROUTER Command Socket

This socket accepts various commands which allows to modify peer list as well
as key-value content dynamically via KvStore. Some commands are listed below
- `KEY_SET` => Set/Update key-value in a KvStore
- `KEY_GET` => Get existing key-value in a KvStore
- `KEY_DUMP` => Get content of local KvStore. Optionally takes a filter argument
- `PEER_ADD` => Add a new peer to a KvStore
- `PEER_DEL` => Del existing peer
- `PEER_DUMP` => Get list of all current peers KvStore is connected to

#### PUB/SUB Channel
All incremental changes in local KvStore are published as `thrift::Publication`
message containing changes. All received incremental changes are processed and
applied locally and conditionally forwarded.

### Implementation
---

#### Incremental Updates - Flooding
Whenever an update is received (either via Cmd socket) or (pub socket) it is
applied locally. If update causes any change in local KvStore then it is
forwarded to all neighbors. An update is ignored when it is echoed back which
limits the flooding.

Here we have potential optimization opportunity to limit flooding only in tree
hierarchy to reduce data flow.

#### Full Sync
Full sync with a neighbor is performed when it is added to the local store.
There is also periodic sync with random neighbor (anti-entropy sync), in case if
any published message from neighbor is missed out.


### Data Encoding
---

One prominent feature is that all values are opaquely encoded as Thrift objects
using client's choice of the protocol (though this is not strictly required).
The data-store itself does not care about the value contents, it only needs to
be told if two values are different, when it propagates them. At the same time,
on the client side, this approach removes the burden of protocol
encoding/decoding by using our standard Thrift libs.

### Versioning of Key-Values
---

We implement very simple versions for merge conflict resolution. Every key has
a 64-bit version value, which is compared to the incoming update message. Only
if the incoming version is greater we will update the local store and flood the
original update message to our subscribers (peers). Notice that we'll preserve
the original version in the flooded message, so that other folks can compare
their versions with the original submission.

### Loop detection
---

To avoid blind flooding, the KV store implements loop detection logic similar
to BGP. We assign every KV store an "originatorId" which is unique in the
system. When a store receives a message, it checks the originatorId, and if it
matches then flooding stops. This allows one to design efficient flooding
topologies and avoid excessive message duplication in the mesh.

### Delete Operation
---
KvStore only supports `Add` or `Update` operations. Implementing `Delete`
operation in CRDT is non-trivial and not well defined in eventual consistent
fashion. For all practical purposes we have supported `Delete` operation via
optional `time to live` aka `ttl` field, indicates lifetime of key-value in
number of seconds.

#### ttl
While advertising key-value set `ttl` to specified seconds and submit to local
store. On update, local store will flood it to all other stores in the network.
Periodically every store, scan locally stored keys and decrement ttl with
elapsed time, if `ttl` drops below `0` key is removed from local store. Since
every node does the same operation, key is deleted from all stores. When
key-dump is requested, then updated ttl is sent (received - elapsed time)
reflecting remaining lifetime of key-value since it's origination.

#### ttl updates
In order to keep key-values with limited lifetime persisted for long duration,
originators are expected to emit `ttl updates` with new ttl values. On receipt
of ttl update (if with higher version), ttl of a key in local store is updated
and ttl updated is flooded to neighbors.

#### Key Expiry Notifications
Whenever keys are expired in a given KvStore, the notification is generated
and published on SUB socket. All subscribers can take appropriate action to
handle expired key (for e.g. Decision removes adj/prefix DB of nodes). These
notifications are ignored by other KvStores as they will be generating very same
notifications by themselves.

### KvStoreClient
---

KvStore is core and heavily used module in OpenR. Interacting with KvStore
involves creating proper thrift objects and send/recv commands on sockets. This
was leading to lot of complexity in code. `KvStoreClient` is added to address
this concern. It provides APIs to interact with KvStore and supports all the
above APIs in really nice semantics so that writing code becomes easy and fun.

While submitting `Key-Vals`, special care needs to be taken for the versions.
Effectively it's up to you to ensure that your change makes it to the network,
so you need to submit with a newer version, and then wait for your KV
publication to come back to you. You would need to watch out for publications
and see if your key gets modified, and take actions accordingly. There is a
special `persist-key` operation which can ensure that key-value submitted
doesn't get override by anyone else.

### More Readings
---

- [Conflict Free Replicated Data Type (CRDT)](https://www.wikiwand.com/en/Conflict-free_replicated_data_type)
