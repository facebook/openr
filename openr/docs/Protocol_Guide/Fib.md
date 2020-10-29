`Fib`
-----

This module is responsible for programming the actual forwarding tables (e.g. in
hardware) on the local node.

### APIs
---

For more information about message formats, check out
- [if/Fib.thrift](https://github.com/facebook/openr/blob/master/openr/if/Fib.thrift)
- [if/Lsdb.thrift](https://github.com/facebook/openr/blob/master/openr/if/Lsdb.thrift)
- [if/Platform.thrift](https://github.com/facebook/openr/blob/master/openr/if/Platform.thrift)

#### SUB Socket
Receives RouteDatabase updates in real-time from Decision and re-programs routes.

#### Cmd Socket
Supports following commands
- `ROUTE_DB_GET` => Get routing database for specified node (as an argument)

### Implementation
---

Fib chooses the best next-hops for each prefix by excluding LFAs and then
program routes via external FibAgent (HW specific, implements `FibService`)
over thrift. Internally we have different adaptation layers for different
hardware platforms - Arista, FBOSS, Juniper, Marvell and Linux etc implementing
the route programming interface, `FibService`, defined in `Platform.thrift`. By
default `Openr\R` comes with `NetlinkFibHandler` which can program routes into
any Linux server for software routing (we use this in Emulation)

### Fast Reaction
---

Fib also listens for link events from LinkMonitor. If a link goes down, it will
immediately remove it from any ECMP groups and will look for an LFA path for
any now unroutable prefixes without waiting for new routes from Decision.
