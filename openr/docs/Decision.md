`Decision`
----------

This module is responsible for computing the local routing table from the
Adjacency and Prefix databases advertised by every node in network (read from
KvStore)

### APIs
---

For more information about message formats, check out
- [if/Decision.thrift](https://github.com/facebook/openr/blob/master/openr/if/Decision.thrift)
- [if/Lsdb.thrift](https://github.com/facebook/openr/blob/master/openr/if/Lsdb.thrift)

#### SUB Socket
Receives realtime Adjacency and Prefix DB updates about all nodes in the network
from the local KvStore.

#### Cmd Socket
Supports the following commands
- `ROUTE_DB_GET` => Get routing database for specified node (as an argument)
- `ADJ_DB_GET` => Get adjacency DBs known to Decision
- `PREFIX_DB_GET` => Get prefix DBs known to Decision

### Implementation
---

`Decision` module uses the links information to build a full graph of the
network, and associates prefixes as `leaf` nodes to their nodes of origin. All
it needs to do after this is run SPF and compute the shortest paths to all other
nodes. This information is then published to the FIB module over a PUB/SUB
socket pair. The FIB module is responsible for obtaining a full dump of the
routing state from KvStore when it restarts.

### Loop Free Alternates
---

In addition to computing the routes reachable for the local system, the Decision
module additionally runs SPF from the perspective of its direct neighbors. This
way we can find the cost to reach the same prefix both from `this` node's
perspective and the `neighbor` perspective. Given this information, the local
node can determine the neighbors that guarantee loop-free alternate paths (LFA)
to the same prefix other than the one the current node is using. These backup
paths could be supplied along with the primary path, or they all could be used
for load-sharing toward the prefix.


### Event Dampening
---

We implemented simple event dampening - i.e. hold SPF runs on first received
link state update to see if we can collect more before doing the full
computation run. This timer could use exponential back-off to allow for
catching more events under heavy network churn. In practice, this helps save a
lot of CPU under heavy network churn.

> NOTE: we assume all links are point-to-point, no multi-access networks are
being considered. This simplifies many things, e.g. there is no need to consider
pseudo-nodes to develop special flooding schemes for shared segments.

### More Reading
---

- [Basic Specification for IP Fast Reroute: Loop-Free Alternates](https://tools.ietf.org/html/rfc5286)
