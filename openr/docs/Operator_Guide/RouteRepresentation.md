# Route Representation

Open/R provides flexible ways of expressing reachability information. It allows
a network operator to express `Preference`, `Performance`, and `Policy Metadata`
with every route announcement. This document describes the route representation
in detail.

### Design Rationale

Simplicity and flexibility are the two main criteria that we considered into the
design of Open/R route. Traditional protocols like BGP, OSPF, and ISIS influenced
the design. The key takeaways

- Numeric metrics to indicate route preference
- Customizable forwarding behaviors within Area
- Meta-data for influencing route policing at Area Border
- Expressing performance requirements
- Path tracing

### Definitions

#### `transitive`
Attributes that are preserved during route re-distribution at Area Border. Note
that not all `transitive` attributes are `mutable` by the policy. `non-transitive`
is the opposite of `transitive`.

#### `mutable`
The route attribute that can be altered by the policy during route re-distribution
at Area Border. `immutable` is the opposite of `mutable`.

### Attributes

#### > `prefix`
`transitive`, `immutable`
Express Network Layer Reachability Information (NLRI) for IPv4 or IPv6. This is
a key attribute of the advertised route.

#### > `metrics`
Expresses the relative preference of route among all received advertisements.
The metrics can be tuned to prefer one network path over another or prefer
certain applications over another in-case of anycast addresses. Open/R supports
3 dimensions of metrics to express a preference. The design allows us to add more
metrics in the future based on the use-case. All metrics are numeric by definition,
as it greatly simplifies the understanding and implementation.

<img width="250" align=right alt="Screen Shot 2020-05-29 at 10 18 29 AM" src="https://user-images.githubusercontent.com/1482609/91365580-55563a80-e7b6-11ea-95f4-9e15986e3bd8.png">

**`metrics.path_preference`**
`numeric`, `transitive`, `mutable`, `prefer-higher`, `1st criterion`
Network path preference for this route. This is set at the origination point. It is
either retained (transitive) or updated as the route traverses the network.
e.g. LIVE-PATH=1000, DRAINED-PATH=500

**`metrics.source_preference`**
`numeric`, `transitive`, `immutable`, `prefer-higher`, `2nd criterion`
User or Application preference of the route. Set at the origination point and is
never modified as route propagates in the network. e.g. HIGH-PRI=200, LOW-PRI=100

**`metrics.distance`**
`numeric`, `transitive`, `mutable`, `prefer-lower`, `3rd criterion`
Cost to reach the originating node from the current area border. By default
incremented by `igp_cost` of the route within the area during route re-distribution
across area. Should never decrease if manipulated via policy to ensure loop-free
routing. This resembles AS-Path length of BGP

> Take a look at [Decision Route Computation](../Protocol_Guide/Decision.md) for understanding best
route selection process

#### > `tags`
`set[string]`, `transitive`, `mutable`
Meta-data associated with the route to facilitate route policing as the route
propagates through the network. Encoded as `set<string>` and hence there is no
ordering and duplicates. This can be used to express various things like
origination node name, propagation scopes, route type, etc. This analogous to
BGP Communities, except this, is much more flexible as there is no byte limit.

#### > `area_stack`
`list[string]`, `transitive`, `immutable`
An ordered list of areas the route has traversed so far. The entry at the front
indicates the `originating area`, while entry at the back indicates the
`peer area` re-distributing the route. This helps in preventing route looping
and debugging.

You can draw similar to BGP AS-Path. However, note that it is not used in best
route selection and can't be modified by the policy.

#### > `forwarding-algorithm`
`non-transitive`, `mutable`
Link-state algorithm for route computaion. Open/R supports two forwarding
algorithm, `SP_ECMP` (Shortest Path ECMP) and `KSP2_ED_ECMP` (K-Shortest Path).
The algorithm with the lowest value is chosen in-case of conflicting advertisements.

#### > `forwarding-type`
`non-transitive`, `mutable`
Data plane forwarding mechanism to use. Open/R supports two forwarding types,
`IP` (Usual IP routing) and `SR_MPLS` (Source Routing with MPLS data plane). The
type with the lowest value is chosen in-case of conflicting advertisements.

> NOTE: `KSP2_ED_ECMP` is only compatible with `SR_MPLS` forwarding type.

#### > `min-nexthops`
`optional[numeric]`, `non-transitive`, `mutable`
Expresses the minimum number of next-hops on a computed route. The route is programmed
and hence re-distributed if min-next-hop criteria is met. Kind of a performance
requirement. This feature helps to avoid funneling in the CLOS-Fabric networks
when the capacity of one of the planes is reduced greatly compared to others
because of maintenance or device failure.

#### > `label-prepend`
`optional[numeric]`, `non-transitive`, `mutable`
The `MPLS` label to prepend for IP->MPLS routes when forwarding towards the
node advertising this route. This is only compatible with forwarding type
`SR_MPLS`


### Thrift Struct

Refer to [struct PrefixEntry in openr/if/Lsdb.thrift](https://github.com/facebook/openr/blob/master/openr/if/Lsdb.thrift)
