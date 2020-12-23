# PrefixManager - Route Advertisement

## Introduction

---

This module is responsible for keeping track of the prefixes originating from
the node and advertising them into the network via KvStore.

For more information about message formats, checkout

- [if/PrefixManager.thrift](https://github.com/facebook/openr/blob/master/openr/if/PrefixManager.thrift)
- [if/Lsdb.thrift](https://github.com/facebook/openr/blob/master/openr/if/Lsdb.thrift)

## Inter Module Communication

---

![PrefixManager Intermodule Communication](https://user-images.githubusercontent.com/5740745/102555840-d5195500-4084-11eb-83a9-e55681139a4b.png)

### Prefix Advertisements

There are three channels of information for managing route advertisements

- `[Producer] ReplicateQueue<thrift::RouteDatabaseDelta>`: receive route
  advertise & withdraw commands. Multiple sources within Open/R (`LinkMonitor`,
  `PrefixAllocator`, `BgpRib`) uses this channel to manage their advertisements.
  Each source is assigned and expected to use a unique type.

- `[Consumer] RQueue<DecisionRouteUpdate>`: receive the computed (hence
  programmed) routes. The programmed routes are candidates for advertisements to
  other areas of which they're not part of. This is termed as route
  re-distribution across the areas. Each RibRoute is converted into a route
  advertisement or withdraw with a special type `RIB`.

- `originatedPrefixDb_`: routes read from `config.originated_prefixes`. These
  routes supports route aggregation logic with `minimum_supporting_routes` knob.
  Each originated route maintain a count of its supporting routes.

In all cases, `PrefixManager` updates the PrefixDatabase advertised in `KvStore`
and persisted on disk when the list changes.

### KvStoreClient

The kvStoreClient running with this module is responsible for making the
`prefix:<node_name>` key persistent in the network.

## Operations

---

Prefix Manager supports the following operations:

- `ADD_PREFIXES` => Adds the list of prefixes provided as an argument
- `WITHDRAW_PREFIXES` => Withdraws the list of prefixes provided as an argument
- `WITHDRAW_PREFIXES_BY_TYPE` => Withdraws prefixes of the type provided as an
  argument
- `SYNC_PREFIXES_BY_TYPE` => Withdraws all current prefixes of the type provided
  and adds the list of prefixes provided
- `GET_ALL_PREFIXES` => Returns all prefixes currently being advertised
- `GET_PREFIXES_BY_TYPE` => Returns all prefixes of the type provided currently
  being advertised

Above requests come from two places:

- `prefixUpdateRequestQueue` -> request from internal modules
- public thrift APIs defined in `openr/if/OpenrCtrl.thrift` -> request from
  external user. e.g. one can add and remove prefixes from the `breeze` cli. See
  [CLI.md](../Operator_Guide/CLI.md) for more details

## Deep Dive

---

### Redistribute Prefix Workflow

![RouteRedistributeLogic](https://user-images.githubusercontent.com/5740745/90441634-250fed00-e08e-11ea-90b5-d29c7e94e558.png)

For a route update (pfx1, node1, area1) from `decisionRouteUpdatesQueue`,
workflow is as follows:

- run area1 egress policy
- append area1 to area_stack, this is considered as a route cross area boundary
- run area2 ingress policy, if accepted => inject to area2.

### Selecting Unique Prefix Advertisement

![RouteRedistributeLogicWithBgp](https://user-images.githubusercontent.com/5740745/90441674-3953ea00-e08e-11ea-99dc-5c0cc731dda8.png)

A prefix can be requested to be advertised by multiple sources e.g. from
configuration (originating route) or RIB (re-distributing route). However, only
a single prefix information can be advertised to other nodes. We do so by
following criteria:

- Tie-breaking on Metrics
- If still not unique, then tie-break on type (aka Source). The tie-breaking on
  source is statically defined in code

Let's take the same (pfx1, node1, area1) example:

- pfx1 is redistributed into area2, (pfx1(`RIB`), me, area2)
- pfx1 is originated with type `BGP` (pfx1(`BGP`), me, all)

To decide which prefix entry openr should originate. We'll compare their
attributes first, if they are the same, `BGP`(3) win over `RIB`(6).

To conclude, attributes tie break happens in two places in openr:

- in prefix Manager: same prefix of different typrs originated by me.
- in decision: same prefix originated by different nodes.
