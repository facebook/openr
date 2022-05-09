# Open/R Initialization Process

This document provides formal specifications, event flow chart, and formal
verification of Open/R’s initialization process.

## Overview

---

The initialization process is an ordered flow of **Events** in the system that
adheres to subsequently defined **Expectations** and **Formal Specifications**.

### Highlights

- Support all cases - Cold Start & Graceful Restart
- Deterministic initialization process that adheres to formal specifications.
- Signal based approach as opposed to timer based. No timers to configure for
  the initialization process.
- Visibility into Open/R's initialization process and its performance. This can
  be recorded and monitor via standard monitoring paradigms - Counters,
  Structured Logs, and Thrift APIs.
- Improved understanding of initialization process & its implementation. Thus
  easier to maintain and evolve code.

### Expectations

Support all cases - Cold Start or Graceful Restart (aka GR) Should be non
disruptive (no packet drop) throughout the startup process Respect operational
state (e.g. drain) throughout the initialization process

## Events

---

Here we describe all the events that take part in Open/R's initialization
process. Order of events is described in Event Flow Chart below.

### AGENT_CONFIGURED

Underlying SwitchAgent is configured. After this event SwitchAgent would report
all available interfaces and enable control plane IO. This means Open/R can
communicate with neighboring nodes via inband interfaces.

Implementation - `getSwitchState` Thrift API result from SwitchAgent

### LINK_DISCOVERED

Open/R has discovered all the links, aka interfaces, and their associated
addresses of the system for the first time after starting up.

Implementation - First successful `GET_LINKS` API call via netlink

### NEIGHBOR_DISCOVERED

Open/R has discovered all the neighbors that it could on the discovered links.
These neighbors are also **added** as **KvStore peers** as and when they are
discovered.

Implementation - Neighbor Discovery is initiated on all the discovered Links in
fast mode with solicited response requirements. Based on current implementation,
Open/R, can discover neighbors within the fast neighbor discovery window. A
timer based signal to be generated once a fast neighbor discovery window
completes on all the discovered interfaces to trigger this event.

NOTE - Discovered neighbors are not announced as Adjacency in KvStore

### KVSTORE_SYNCED

All the KvStore peers are synchronized. This means the **KvStore** instance of
the local node is **consistent** with all other instances in **the network**.

NOTE - Any subsequent update to KvStore on any node would be flooded to all
instances of area KvStore, and an eventual consistency would be achieved.

### SR_SID_ALLOCATED

All Segment Routing (SR) Segment Identifiers (SIDs) (e.g. NodeSID) are allocated
for the starting node. Certain labels e.g. NodeSID can be allocated via
distributed computation using RangeAllocator.

NOTE - The SID allocation must not affect existing SID allocation of other
participating nodes in the network.

### PREFIX_DB_SYNCED

A node has synchronized all of the route advertisements, aka prefix entries, to
KvStore. Along with addition or update of entries, it also cleans up any stale
prefix entries from any previous incarnation.

### RIB_COMPUTED

First route computation event of the local node. This computation is intended to
produce the Routing Information Base (RIB) of the node.

### FIB_SYNCED

Computed RIB is programmed to the underlying Forwarding Information Base (FIB).

### INITIALIZED

This would **unblock** the special flag of **AdjOnlyUsedByOtherNode** on
neighboring nodes to complete bi-directional adjacency establishment for path
computation and start pouring traffic to node which undergoes INITIALIZATION
sequence.

## Formal Specification (FS)

---

Formal specification defines the constraints in the system that are important to
the correct functioning of the system.

1. Control Plane IO readiness is a must for inter-node communication aka
   Neighbor Discovery & KvStore communications.
2. All neighbors must be discovered and added as KvStore peers to ensure
   KvStore’s eventual consistency with all other instances in the network.
3. KvStore Consistency is a must for any distributed computation to function
   correctly. E.g. loop-free routing, uniqueness of Node SID allocation
4. All active nodes (part of KvStore topology) must perform computation
   immediately upon receipt of KvStore update. KvStore consistency guarantees
   correctness of Distributed Computation (e.g. Routes).
5. Any computed route must be programmed (add/update) as soon as after it's
   computation. This ensures that distributed computation that is loop-free also
   ensures that forwarding is loop-free, and also guarantees reachability in
   intermediate nodes of one route path.
6. Distributed computation in an area may result in micro-loops as all the Nodes
   in Area converge to a new state. We term this as the **Convergence Cycle**.
   This should be fine as long as it is less than the defined threshold.
7. For the restarting node, the FIB_SYNCED event must not result in any update.
   This ensures that restart is graceful. This assumes there is no network event
   throughout the restart and restart completes within GR window.
8. RIB Computation and Programming must happen to facilitate area route
   redistribution
9. The restarting node should advertise the same Adjacencies and Prefixes as
   before to respect GR. Any intended change in Adjacencies or Prefixes through
   configuration, will be realized in KvStore only after initialized state.
10. Any node (including restarting) shouldn’t cause any RIB update to respect
    GR. On any RIB update, a node should drop the adjacencies with the
    restarting node. For practical purposes, we only drop adjacencies with the
    restarting node on "Topology Update".

## Event Flow Chart

---

The diagram below defines the order of events throughout the Initialization
process. Red boxes represents external signal. **Initialized** state represents
that node is part of network and routing packets actively.

![Open/R Initialization Process Event Flow Chart](https://user-images.githubusercontent.com/1482609/123338843-57844500-d4fe-11eb-9b9a-c34a5bda9e27.png)

**Note - Prefix & Adjacency update in KvStore**

Any advertisement in KvStore by the initializing node that can affect forwarding
state shouldn’t happen before the local node starts route computation and
programming. By making Prefix and Adjacency advertisements in KvStore to depend
on `FIB_SYNCED` would ensure that FS(4) is respected.

**Note - Prefix database sync** Redistribution of routes across the area is
based on RIB routes. For a restarting node it must wait for `FIB_SYNCED` to
receive a full copy of RIB routes before syncing routes in KvStore. If a node
doesn’t await then it may end up withdrawing the RIB prefixes from KvStore.
They’ll be added back once first RIB computation/programming completes.

**Note - Adjacency & Prefix database sync** We considered both approaches where
an initializing node synchronized its adjacency database in parallel to or after
the prefix database synchronization in KvStore.

Synchronizing Adjacency database in parallel to Prefix database may end up using
stale prefixes in KvStore from previous instantiation. This would get fixed as
soon as the Prefix database synchronizes. However, we can avoid this altogether
by performing Adjacency synchronization after Prefix synchronization.

## Implementation

---

**Important Note** - This work is in progress. So everything described here may
not be fully implemented.

The Event Flow Chart above describes the order of initialization, but it doesn’t
express how to implement it. Below we describe incremental steps for
implementation such that code can be deployed at any stage to production. Every
step would benefit us and bring us closer to the final solution.

1. Introduce events - `NEIGHBOR_DISCOVERED`, `KVSTORE_SYNCED`
2. Decision to await for `KVSTORE_SYNCED` event to begin it's route computation.
   This removes the `decision_eor_time_s` config parameter and makes route
   computation signal based. This step marks RIB_COMOUTED event.
3. Fib begins it's first route programming on receipt of update from Decision.
   This removes dependency on `fib_hold_time_s` config parameter and makes route
   programming signal based. First successful sync would publish a fib update
   and marks `FIB_SYNCED` event.
4. PrefixManager would perform `PREFIX_DB_SYNC` on receipt of first FIB update
   and mark `PREFIX_DB_SYNC` event.
5. LinkMonitor would need to learn `PREFIX_DB_SYNC` and perform
   `ADJACENCY_DB_SYNC` in KvStore
6. Spark would implement a new state for Neighbor, that signifies the neighbor
   is available for KvStore peering but corresponding adjacency can't be used
   for routing, aka `Hold State`. Spark would need to learn `ADJACENCY_DB_SYNC`
   from LinkMonitor and declare adjacency usable for routing as well. This would
   be an interesting and non-trivial engineering work.
7. OpenrCtrlHandler - Expose events and their timestamps via thrift APIs. This
   would provide complete visibility into the Initialization Process. Measure
   duration of overall initialization time and track it as a performance metric.
8. Integrate details of initialization in `breeze openr status` CLI
