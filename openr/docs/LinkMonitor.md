`LinkMonitor`
-------------

This module is responsible for learning link information from the underlying
system and managing neighbor sessions. At a high level
- Discover the links on system and enable/disable neighbor discovery on them
- Maintain local node's link-state in KvStore
- Manage peering sessions of KvStore (one per neighbor)

### APIs
---

For more information about message formats, checkout
- [if/LinkMonitor.thrift](https://github.com/facebook/openr/blob/master/openr/if/LinkMonitor.thrift)
- [if/Lsdb.thrift](https://github.com/facebook/openr/blob/master/openr/if/Lsdb.thrift)

### Link/Address Discovery
---

LinkMonitor relies on external `Platform` service to provide interface and
address information. By default OpenR comes with `NetlinkPlatform` which
learns about interface and addresses via `netlink` library and it can be used
on most platforms.

### PUB Channel
---

Any link activity (address or status) learned via `netlink` is published to PUB
channel which can be consumed in realtime by other applications. For now FIB
and Spark listens to these messages and take appropriate actions (e.g.
shrinking ECMP group or start/stop neighbor discovery on link).

### ROUTER Command Socket
---

`LinkMonitor` accepts various commands for operation of link-state protocol.
Important one being overloading link/node commands to alter link-state in a
network for network operations. Commands being
- `SET/UNSET OVERLOAD` => Toggles transit traffic through node
- `SET/UNSET LINK_OVERLOAD` => Toggles transit traffic through a specific link
- `SET/UNSET LINK_METRIC` => Customize metric value on link for soft drains
- `DUMP_LINKS` => Retrieves the link information of a node

### LinkState Management
---

LinkMonitor listens to `Spark` events (described below) (e.g. `NEIGHBOR_UP`,
`NEIGHBOR_DOWN`) and maintains the `link-state` of the local node. Then along
with custom overrides list `metric`, `overload` bits for node/link it prepares
the `AdjacencyDatabse` object for the node and keeps it up to date in the
`KvStore` (so that everyone else in the network can see this LinkDatabase).

On link down, neighbor discovery is immediately stopped on a link, link state is
updated and `KvStore` peering sessions are tore down.

> NOTE that Link-Up has a back off but Link-Down doesn't. This is because, we
want to be as fast as possible to react to down events to avoid potential packet
drops.

### Link Events Dampening
---

Sometime link status can flap badly because of various reasons for e.g. bad
optic, or poor wireless reach. In such cases we would like to avoid control
plane churn across whole of a network, as nodes will try to route through/across
when link is up/down. To avoid such scenario we have added dampening support
in LinkMonitor. If link flaps then backoff is applied which gets doubled if
link flaps again within backoff period and so on. If link shows itself stable
within period then it is enabled for neighbor discovery.

You can configure backoffs for link event dampening with following flags
- `--link_flap_initial_backoff_ms=1000` (default=1s)
- `--link_flap_max_backoff_ms=60000` (default=60s)

### Link Metric
---

LinkMonitor is responsible for computing metric value for each Adjacency
to neighbors which is used to compute cost of a path in Decision's SPF
computation. For now we support two kind of metrics (configured via flag)
- `hop_count` => Use `1` (constant) metric value for each Adjacency
- `rtt_metric` => `rtt_us / 10` where `rtt_us` is measured rtt in microseconds

OpenR is pretty flexible and using other parameters like `loss`, `jitter`,
`signal strength` is potentially doable (via Platform abstraction)
