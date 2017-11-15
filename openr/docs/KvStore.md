`KvStore - Store and Sync`
--------------------------

`KvStore` provides a self-contained, in-memory `key-value` datastore which is
eventually consistent. Underlying implementation is based on  **conflict-free
replicated data type (CRDT)**. The stores are interconnected in a mesh, and
synchronize their contents in an eventually consistent fashion. This store is
used to disseminate a set of key-value pairs to all nodes in the network/cluster.
For example, a node may post information to its local store about its adjacent
neighbors under a key `adj:myRouteName` and this information will propagate to
all other stores in the network, under the same key name.

### APIs
---

For more information about message formats, check out
- [if/KvStore.thrift](https://github.com/facebook/openr/blob/master/openr/if/KvStore.thrift)

#### ROUTER Command Socket

This socket accepts various commands which allow for modifying the node's peer
list as well as key-value content dynamically via KvStore. Some commands are
listed below
- `KEY_SET` => Set/Update key-value in a KvStore
- `KEY_GET` => Get existing key-value in a KvStore
- `KEY_DUMP` => Get content of local KvStore. Optionally takes a filter argument
- `PEER_ADD` => Add a new peer to a KvStore
- `PEER_DEL` => Del existing peer
- `PEER_DUMP` => Get list of all current peers KvStore is connected to

#### PUB/SUB Channel
All incremental changes in local KvStore are published as `thrift::Publication`
messages containing changes. All received incremental changes are processed and
applied locally and conditionally forwarded.

### Implementation
---

#### Incremental Updates - Flooding
Whenever an update is received (either via Cmd socket) or (pub socket) it is
applied locally. If the update causes any change in local KvStore then it is
forwarded to all neighbors. An update is ignored when it is echoed back which
limits the flooding.

Here we have a potential optimization opportunity to limit flooding only to a
minimum spanning tree.

#### Full Sync
Full sync with a neighbor is performed when it is added to the local store.
There is also periodic sync with a random neighbor (anti-entropy sync), in case
any published message from a neighbor was missed.


### Data Encoding
---

One prominent feature is that all values are opaquely encoded as Thrift objects
using client's choice of protocol (though this is not strictly required).
The data-store itself does not care about the value contents, it only needs to
be told if two values are different, when it propagates them. At the same time,
on the client side, this approach removes the burden of protocol
encoding/decoding by using our standard Thrift libs.

### Versioning of Key-Values
---

We implement very simple versions for merge conflict resolution. Every key has
a 64-bit version value, which is compared to the incoming update message. Only
if the incoming version is greater will we update the local store and flood the
original update message to our subscribers (peers). Notice that we'll preserve
the original version in the flooded message so that other folks can compare
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
fashion. For all practical purposes, `Delete` operation can be acheived via
optional `time to live` aka `ttl` field, which indicates the lifetime of
key-value.

#### ttl
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
originators are expected to emit `ttl updates` with new ttl values. On receipt
of a ttl update (with higher version), the ttl of a key in local store is
updated and the ttl update is flooded to neighbors.

#### Key Expiry Notifications
Whenever keys are expired in a given KvStore, the notification is generated
and published on SUB socket. All subscribers can take appropriate action to
handle an expired key (for e.g. Decision removes adj/prefix DB of nodes). These
notifications are ignored by other KvStores as they will be generating the very
same notifications by themselves.

### KvStoreClient
---

KvStore is core and a heavily used module in OpenR. Interacting with KvStore
involves sending and receiving proper thrift objects on sockets. This
was leading to a lot of complexity in the code. `KvStoreClient` is added to
address this concern. It provides APIs to interact with KvStore and supports all
the above APIs in really nice semantics so that writing code becomes easy and
fun.

While submitting `Key-Vals`, special care needs to be taken for the versions.
Effectively it's up to you to ensure that your change makes it to the network,
so you need to submit with a newer version, and then wait for your KV
publication to come back to you. You should check publications
to see if your key gets modified, and take actions accordingly. There is a
special `persist-key` operation which can ensure that key-value submitted
doesn't get overridden by anyone else.

### More Reading
---

- [Conflict Free Replicated Data Type (CRDT)](https://www.wikiwand.com/en/Conflict-free_replicated_data_type)
