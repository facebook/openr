`Miscellaneous`
---------------

### OpenR and V4
---

OpenR was initially designed to work on networks which support IPv6 only.
However, in effort to bring OpenR onto Backbone and DataCenter networks, we
needed OpenR to support v4 as well. You can set the flag **--enable_v4** to
exchange v4-prefixes and transport addresses. Both v4 and v6 will run
simultaneously.

Note that when v4 is enabled the underlying v4 and v6 network topology must be
congruent (all devices should support v4 and have a v4 address assigned to
interfaces). We use any v4 addresses assigned to the interface as a transport
address for v4 traffic between nodes. For IPv6 we always use Link-Local
address as it is always present.

Further, you can't run OpenR on a v4 only network. All communication between
OpenR processes running on different machines and link discovery is done using
IPv6 transport layer.

### Traffic Class for Control Packets
---

All IP packets being exchanged between two OpenR intsnaces running on different
nodes are marked with the traffic class of `FLAGS_ip_tos`. This is so the
underlying network can differentiate control plane traffic from normal traffic
packets and give them higher priority.

### OpenR Graceful Restart
---

All the pieces of OpenR make it a routing protocol which is able to do
dynamic topology discovery, exchange reachability info and program the **FIB**. It
easily detects nodes going up and down and responds quickly by programming routes
to hardware via its route programming agent.

However, on software upgrade or occasional process restart, we do not want to
disrupt traffic forwarding. Many traditional protocols like BGP and OSPF
have extensions (RFCs) which define the operations of Graceful Restart. In
OpenR, the concept of Graceful Restart is baked into its basic functionality.

The aim of Graceful Restart is to preserve the forwarding state of restarting
node as well as routes pointing to the restarting node from its neighbor so that
none of the traffic is disrupted. This is very important as it prevents traffic
disruptions during software upgrades or restarts.

The operation of graceful restart can be described in two domains

#### Restarting Node
Restart of a node can happen anytime (admin, crash or just software upgrade). GR
is implemented to handle not-anticipated restart of neighbor. When a node
restarts, it does the following:

1. Establish the adjacencies with all of its neighbor within **adjHoldTime**
2. Hold the currently programmed routes for **fibHoldTime**
3. Any physical link-events must propagate to FIB and take immediate action on
   already programmed routes
4. Advertise new adjacency after **adjHoldTime** and program the latest computed
   routes after **fibHoldTime**

It is assumed that network is not changing much when a node is restarting.
However local link down events are taken into immediate effect.

#### Node whose neighbor is restarting
Spark-hello packets are exchanged between neighbors as a keep-alive mechanism.
Each hello packet has a sequence number which is monotonically increasing on
every hello packet sent from a neighbor. Every node keeps track of the latest hello
packet received from each neighbor. If we see a wrap-up in hello packet sequence
number then it is an indication that a node has restarted.

On detecting node-restart (receipt of hello packet with wrapped sequence number)
Spark generates a NEIGHBOR_RESTART event and passes it to LinkMonitor with
latest neighbor info (socket urls). LinkMonitor updates KvStore to update
neighbors adjacency (will be updated only if needed). However, spark doesn't
reset the **holdTimer** for the neighbor which has been detected as restarted
and it doesn't do unless it sees itself in the subsequent neighbor's hello
packets.

#### Notes on Timers
Restarting node has to form adjacencies with all of its neighbors within it's
holdTime from its last hello message sent before restarting.

**adjHoldTime** = 2 * keepAliveTime

Reason: At worst case 2 hello message needs to be exchanged between nodes to
form adjacency

**fibHoldTime** = 3 * keepAliveTime

Reason: Giving out extra buffer of **keepAliveTime** to let our latest adjacency
accepted by KvStore and then proceed with route programming.

**holdTime** = >> keepAliveTime

We should keep much higher holdTime than keepAliveTime for graceful restart to
be complete. On my testing having 30s of holdTime and 2s keepAliveTime works
great and we see no traffic disruptions even when two adjacent neighbors are
restarting together.

### Parallel link Support
---

Two nodes can have multiple Adjacencies between them if they are connected
over distinct interfaces (Wireless or Wired Parallel links). LinkMonitor
advertises all adjacencies (can be multiple for one neighbor) into KvStore.
Decision creates one edge per Adjacency between nodes with appropriate metric
value. When a node is selected as a nexthop then all interfaces over which the node
has an adjacency will be used as the nexthop.

### Persistent Store
---

This module helps in persisting various state information of OpenR so that
across restart/reboots OpenR comes back with the same state as before.

- Advertised prefixes
- Link/Node overload bits of custom link metrics
- Previously elected prefix for a node

### Drain Support
---

Drain/Undrain of links or nodes is often exercised for planned network
maintenances. LinkMonitor modules provides a few ways and simpler APIs to perform
these operations and the state information is reflected in the AdjacencyDatabase

#### Set/Unset Node Overload

If set, stops transit traffic going through the node. Though traffic originating
and terminating at the node will continue to flow like before.

#### Set/Unset Link Overload

If set, stops transit traffic through the link. This is kind of hard draining the
link. If all the links of a node have `overload` bit set then the node will become
disconnected from the network (OpenR continues to work unaffected).

#### Set/Unset Link Metric Overrides

You can set custom metric values on a link (usually to a high number) in order to
perform soft drain. Most traffic will be drained except traffic which doesn't
have alternate paths.

### Security Concerns
---

Currently, openr assumes it is running on a trusted network and makes no effort
to authenticate incoming connections. We hope to address this in the short term
by enabling [CurveZmq](http://curvezmq.org/) on sockets reachable from off box.
CurveZmq by itself makes no attempt at solving the hard problem of
authentication and accepts connections from a configurable list of public keys.
To address this, we may add to our peering protocol the ability to present and
verify certificates.

We understand that this is a difficult problem with subtle pitfalls. Going
forward we hope to move towards an approach built on top of widely deployed and
well verified solutions such as OpenSSL.


### Potential Extensions
---

Some potential extensions we have been thinking of and are on our roadmaps

- Segment routing
- KvStore flooding optimizations
- Weighted ECMP routing
