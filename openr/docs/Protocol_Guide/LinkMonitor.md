# LinkMonitor - Links Discovery

## Introduction

---

`LinkMonitor` is the module interacts with the system to monitor link, aka,
interface status and addresses. It learns and monitor the link information from
Linux kernel through `Netlink Protocol`. Main functions of this module are:

- Monitor system interface status & address;
- Initiate neighbor discovery for newly added links;
- Maintain KvStore peering with discovered neighbors;
- Maintain `AdjacencyDatabase` of current node in `KvStore` by injecting
  `adj:<node-name>`;

## Inter Module Communication

---

![LinkMonitor Intermodule Communication](https://user-images.githubusercontent.com/10733132/130930966-4c2557ce-bc88-4781-a37a-dff29da52363.png)

- `[Producer] ReplicateQueue<thrift::InterfaceDatabase>`: react to `Netlink`
  event update and asynchronously update interface database to inform `Spark` to
  start/stop neighbor discovery on the updated interfaces.

- `[Producer] ReplicateQueue<PrefixEvent>`: populate redistributed interface
  information from `OpenrConfig` and inject interface address information to
  `PrefixManager`, which is responsible for injecting prefixes into `KvStore`
  for propagation.

- `[Producer] ReplicateQueue<thrift::PeerUpdateRequest>`: populates **PEER
  SPEC** information to `KvStore` for peer session establishment over TCP
  connection.

- `[Producer] ReplicateQueue<KeyValueRequest>`: send requests to set key-value
  storing node's adjacency database in `KvStore`.

- `[Consumer] RQueue<NeighborEvents>`: receive neighbor update sent from `Spark`
  for adjacency updates. Events include neighbor UP/DOWN/RESTART/RTT-CHANGE.
  This info will finally lead to **PEER SPEC** propagation towards `KvStore`.

- `[Consumer] RQueue<fbnl::NetlinkEvent>`: receive `Netlink` event from
  underneath platform to add/delete/update interface information, which further
  populates update to `Spark` and `PrefixManager`.

- `[Consumer] RQueue<KvStoreSyncEvent>`: receive notification from `KvStore` to
  indicate state of initial full-sync. This helps with the graceful-restart(GR)
  case. See the later section for detail.

## Operations

---

Typical workflow at a high level will be:

- Link **UP**:

  - `LinkMonitor` receives interface updates and expands the interface database.
    It will update `Spark` for neighbor discovery work.
  - `Spark` sends back UP adjacency if any, which leads to adjacency key-value
    population towards `KvStore`.

- Link **DOWN**:

  - `LinkMonitor` receives interface update and shrinks interface database.
    `Spark` will be updated and all established adjacency will be dropped.
    Finally, the neighbor DOWN event will be reported back;

> NOTE: `LinkMonitor` manages peering sessions of `KvStore` per peer. This means
> there will be ONLY one TCP session towards one unique node even there are
> parallel adjacencies between them.

## Deep Dive

---

### Link/Address Discovery

`LinkMonitor` relies on `Netlink` to directly fetch LINK/ADDRESS information
from Linux Kernel. See `Netlink.md` for detailed understanding. It leverages
fiber task to monitor update via reader queue for event notification.

### LinkState Management

`LinkMonitor` provides public API to accept various commands for operation of
the link-state protocol. For example, to overload link/node to alter link-state
in the network. Sample APIs are:

- `SET/UNSET OVERLOAD` => Toggles transit traffic through node
- `SET/UNSET LINK_OVERLOAD` => Toggles transit traffic through a specific link
- `SET/UNSET LINK_METRIC` => Customize metric value on a link for soft drains
- `DUMP_LINKS` => Retrieves the link information of a node
- `DUMP_ADJS` => Retrieves the adjacency information of a node

### Adjacency Event Throttling

`LinkMonitor` listens to `Spark` events (described below) (e.g. `NEIGHBOR_UP`,
`NEIGHBOR_DOWN`) and maintains the link-state of the local node. From there, it
gathers any customized link metrics and `OVERLOAD` bits for the node or any
link. Then it prepares the `AdjacencyDatabase` object for the node and keeps it
up to date in `KvStore` to let everyone else in the network be aware of any
link-state change.

> NOTE: **NEIGHBOR UP** event goes through throttled fashion since we don't want
> `KvStore` suffers from tremendous updates when a node is just started.
> However, **NEIGHBOR DOWN** event doesn't do the same thing due to fast
> convergence requirement to avoid potential packet loss.

### Link Events Dampening

Interfaces on systems are usually expected to be stable either UP or DOWN.
However for number of reasons the interface may go crazy and starts to flap e.g.
bad-optics, wireless medium hindrance. In such cases, we would like to avoid
control plane churn across the whole network, as nodes will try to route
through/around when the link is up/down. To avoid such a scenario, `LinkMonitor`
supports link-event dampening, which means exponential backoff is applied to the
link if it flaps. As the name suggests, back-off time will be 2x the previous
backoff period until it reaches `max_backoff_ms`. If the link shows stability
within a period, it is enabled for neighbor discovery.

You can configure backoffs for link event dampening with `LinkMonitorConfig`.

```
struct LinkMonitorConfig {
  1: i32 linkflap_initial_backoff_ms = 1000 # 1s
  2: i32 linkflap_max_backoff_ms = 8192 # 8.192s
  ...
}
```

See
[if/OpenrConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/OpenrConfig.thrift)

### Link Metric

`LinkMonitor` is responsible for computing the metric value for each adjacency
to neighbors which are then used to compute the cost of a path in `Decision`'s
SPF computation. For now, we support two kinds of metrics:

- [by default] `hop_count` => Use `1` (constant) metric value for each Adjacency
- [by config knob] `rtt_metric` => `rtt_us / 100` where `rtt_us` is measured rtt
  in microseconds.

> NOTE: `rtt` is measured dynamically by `Spark` as part of neighbor discovery
> and keep-alive mechanisms. RTT changes are observed handled dynamically.

### Segment Routing Support

To Support `Segment Routing`, `LinkMonitor` injects:

- **Node Label** by leveraging `RangeAllocator` to assign a globally unique
  label across the network(via `KvStore` to detect collision);
- **Adjacency Label** by leveraging `Spark` to assign a unique label derived
  from **ifIndex**;

> NOTE: **Adjacency Label** is locally unique per node.
