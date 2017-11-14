`PrefixManager`
---------------

This module is responsible for keeping track of the prefixes originating
from the node and advertising them into the network via KvStore.

### APIs
---

For more information about message formats, checkout
- [if/PrefixManager.thrift](https://github.com/facebook/openr/blob/master/openr/if/PrefixManager.thrift)
- [if/Lsdb.thrift](https://github.com/facebook/openr/blob/master/openr/if/Lsdb.thrift)

#### KvStoreClient
The kvStoreClient running with this module is responsible for making the
`prefix:<node_name>` key persistent in the network.

#### PersistentStoreClient
Responsible for writing the current PrefixDatabase to disk, and picking it up
after restarts.

#### Cmd Socket
Supports following commands
- `ADD_PREFIXES` => Adds list of prefixes provided as argument
- `WITHDRAW_PREFIXES` => Withdraws list of prefixes provided as argument
- `WITHDRAW_PREFIXES_BY_TYPE` => Withdraws prefixes of type provided as argument
- `SYNC_PREFIXES_BY_TYPE` => Withdraws all current prefixes of type and adds
                             list of prefixes provided
- `GET_ALL_PREFIXES` => Returns all prefixes currently being advertised
- `GET_PREFIXES_BY_TYPE` => Returns all prefixes of type currently being
                            advertised

### Implementation
---

`PrefixManager` module is quite simple. It stores the list of prefixes to be
advertised by the node, listens on a ROUTER socket for commands that modify this
list, and updates the PrefixDatabase advertised in `kvStore` and persisted on
disk when the list changes.

### Interacting with PrefixManager
---

For c++, we provide a simple `PrefixManagerClient` which implements a simple api
for modifying the prefix list advertised from the node.

Additionally, you can add and remove prefixes from the `breeze` cli. See
[docs/Breeze.md](https://github.com/facebook/openr/blob/master/openr/docs/Breeze.md) for more details
