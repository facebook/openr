`Platform`
----------

This module is responsible for running a platform process for the OpenR main
process to handle:
- Fib thrift service for route programming on Linux platform;
- System thrift service to publish Linux system updating events

### APIs
---

For more information about message formats, checkout
- [if/Platform.thrift](https://github.com/facebook/openr/blob/master/openr/if/Platform.thrift)

### Netlink System Handler
---

Inherits from `NetlinkSubscriber`, a simple wrapper over Netlink Socket for
subscribing link/address/neighbor events.
Publishes system dependent link activity (address or status) learned via
`netlink` in real-time to clients (i.e.LinkMonitor) of Platform.
Provides thrift service interface for client modules (i.e. LinkMonitor) to
query for all link/neighbor entries by dispatching thrift service call get*,
and Netlink System Handler responses with a full dump, which is periodically
synced between platform and cache.


### Netlink Fib Handler
---
Provides thrift service interface for on-box client modules (i.e. Fib) to
program platform routing. Client dispatches thrift service call to update routes
or get full route table from Platform.
Client can periodically synchronize with service by keep alive check call,
a re-sync request is supported by the handler to re-send routing information
upon client restart.


### Platform Support
---
To support platform other than Linux, developers should implement the thrift
service APIs in `if/Platform.thrift`. For system service handler, ZMQ PUB Socket
 is required for publishing platform events to OpenR.
