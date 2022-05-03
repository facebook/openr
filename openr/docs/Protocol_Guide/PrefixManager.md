# PrefixManager - Route Advertisement

## Introduction

---

`PrefixManager` is the module which keeps track of the prefixes originated from
local node. It advertises/withdraws prefixes to/from the network via `KvStore`.
Main functions of this module are:

- Prefix operations, including advertising and withdrawing;
- Route Origination;
- Prefix redistribution(cross-AREA);

## Inter Module Communication

---

![PrefixManager Intermodule Communication](https://user-images.githubusercontent.com/51382140/110161227-ec253480-7da1-11eb-8584-844de9568e1d.png)

There are three channels of information for managing route advertisements

- `[Producer] ReplicateQueue<DecisionRouteUpdate>`: publish static routes to be
  programmed by local nodes. This queue is currently ONLY used for route
  origination purpose. Each static route is with a special type `CONFIG`.

- `[Producer] ReplicateQueue<KeyValueRequest>`: send requests to set/clear
  key-values in `KvStore` representing advertised/withdrawn routes.

- `[Consumer] RQueue<PrefixEvent>`: receive route advertising & withdrawing
  commands. Multiple sources within Open/R (`LinkMonitor`, `PrefixAllocator`,
  `BgpRib`) uses this channel to manage their advertisements. Each source is
  assigned and expected to use a unique prefix type.

- `[Consumer] RQueue<DecisionRouteUpdate>`: receive the computed (hence
  programmed) routes. The programmed routes are candidates for advertisements to
  other areas of which they're not part of. This is termed as route
  re-distribution across the areas. Each RibRoute is converted into a route
  advertisement or withdraw with a special type `RIB`.

In addition, `PrefixManager` will read routes to be originated from
`OpenrConfig.originated_prefixes` and stored them inside `originatedPrefixDb_`.
These routes supports route-aggregation logic with `minimum_supporting_routes`
knob. Each originated route maintains a count of its supporting routes.

## Operations

---

To fulfill operations defined in previous section, `PrefixManager` updates its
local prefix database and interacts with `KvStore`:

- [Advertise]: Send `Persist` key-value request to `kvRequestQueue`;
- [Withdraw]: Send `Clear` key-value request to `kvRequestQueue`;

See [KvStore.md](KvStore.md#self-originated-key-values) for how `KvStore`
handles these key-value requests.

`PrefixManager` supports the following operations:

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
