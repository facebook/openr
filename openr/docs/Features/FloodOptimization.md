# Flood Optimization

## Introduction

---

Flooding Optimization allows Open/R to greatly reduce control plane traffic. We
use DUAL (diffusing update algorithm) to let Open/R to form a SPT (spanning
tree, not necessarily MST) used as a flooding topology. Open/R will only flood
KvStore Updates on flooding topology which is a subset of physical topology.
This reduces flooding-updates complexity from O(E) down to O(V).

## DUAL

---

code path: `openr/dual/`

This part is the core DUAL algorithm library. It's a combination of
distance-vector and Diffusing algorithm. To address looping problem in
distance-vector, there is a few solutions, one of them being specifying the
whole path to make it path-vector (like BGP does), DUAL instead leverage
diffusing algorithm to make sure Parent Node won't take any action until all its
dependent nodes have successfully calculated their paths. Here's few important
specs DUAL library carries:

- Make sure DUAL nodes can form SPT correctly, every other nodes will find
  shortest path towards the root (similarly to finding shortest path towards
  prefix as routing protocol does). (NOTE: each DUAL run can end up different
  SPTs if there is multiple shortest path towards the root, but SPT formation is
  guaranteed)
- Make sure DUAL nodes can handle any network failures (e.g any single/multiple
  node/link failures under any topology can be handled by DUAL properly ->
  reform new SPT)
- **Convergence time** is small (benchmark in simulation 1024 grid topology,
  takes < 1 sec to converge)
- Can form **multiple SPT** if multiple roots are specified.

## KvStore Integration

---

code path: `openr/kvstore/`

This part integrate DUAL library with KvStore, here's how it works:

- KvStore is inheriting from DUAL, so each KvStore node can talk to its neighbor
  via DUAL protocol
- KvStore overrides I/O part via zmq currently (can be replaced by others like
  thrift)
- Whenever a node's nexthop towards root got changed (due to network event e.g
  node/link flap etc), it will synchronize with its old and new parent to make
  sure SPT information is consistent on both ends.
- Multiple KvStore nodes can be picked as roots, in that case, multiple SPT
  trees would be formed but only one of them will be USED as flooding topology
  based on node's priority (currently smallest node's name would win). If
  current root got restarted, SPT formed by 2nd priority root will become
  current flooding topology.
- When a node needs to flood its update, it will only flood to its dual-parent
  and dual-children instead of all neighbors as before.
- In case an update needs to be flooded while DUAL node is not ready (e.g it's
  in the process of forming SPT, we call it ACTIVE mode), in this mode, KvStore
  will fall back to legacy flooding mode by flooding everywhere. Once DUAL is
  done, it goes back to PASSIVE mode, flooding optimization will take effect
  again.
- Handle parallel link cases as well. From DUAL perspective, parallel link will
  be treated as a single neighbor.
- Backward compatible. You can have a single open/r domain, where 30% is
  flooding disabled, 70% enabled. 70% nodes would leverage DUAL to perform
  flooding optimization while 30% will perform legacy flooding.
