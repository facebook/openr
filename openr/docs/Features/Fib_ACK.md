# Open/R FIB-ACK feature

## Introduction

---

FIB-ACK feature guarantees the forwarding information, aka, routes to be added,
to be programmed in HW table before the reachability is announced to other nodes
within/arcross KvStore areas. Reverse order applies for routes to be deleted.

Open/R performs as link-state routing algorithm in one single area, and
distance-vector routing algorithm when redistributing routes across areas.
FIB-ACK feature applies in both scenarios.

- Link-state algorithm in single area: For routes originated by source node,
  FIB-ACK feature will make sure the route is programmed locally before
  advertising to peers through KvStores.
- Distance-vector algorithm across areas: For routes to be redistributed from
  one area to another, FIB-ACK feature will make sure the route is programmed
  locally first.

## FIB-ACK for routes in single area

One device could originate and advertise routes/prefixes to attract traffic from
peers in the network. In order to avoid traffic loss, the device itself should
be able to forward the traffic ahead of announcing the routes/prefixes to peers.

There are several types of originated routes in Open/R,

- Originated IP prefixes from Open/R config,
- Routes received from services (e.g., VIP routes),
- Originated segment routing MPLS routes.

Fib module is responsible for programming routes received from Decision module,
and then forward the programmed ones to PrefixManager. For above types of
originated routes, PrefixManager will not advertise them into KvStores until
receiving associated programmed unicast/MPLS routes from Fib module.

## FIB-ACK for routes redistribution across areas

Open/R creates multiple KvStoreDbs, one instance per area configured on this
device. Decision takes in prefix and adjacency infromation from KvStores of all
areas for RIB computation. For route redistribution, PrefixManager advertises
learned routes from one area to another, aka, from one KvStoreDb instance to
another. Similarlly, the device should be able to forward traffic before
redistributing routes/prefixes across areas.

In Open/R, PrefixManager receives programmed routes from Fib module and
redistributes the routes across areas. In this way, routes must have been
programmed locally before readvertisement happens.

## FIB-ACK support for routes to be deleted

To avoid traffic loss in the process of deleting routes from the network, peers
should stop sending traffic before the programmed routes are removed from the
originated device. In Open/R, this is achieved by withdrawing routes from
KvStore immediately, followed by delaying the removal of routes at originated
device. In this way, peers receive route withdrawal and stop sending traffic to
originators. The delay timer is set as 1 second today, given this should be
enough for route withdrawl announcement among peers. It could be adjusted for
larger network if necessary.
