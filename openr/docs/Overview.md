`OpenR`
-------

Open Routing, OpenR, is Facebook's internally designed and developed routing
protocol/platform. Primarily built for performing routing on `Terragraph`
network but it's awesome design and flexibility has lead to it's adoption onto
Facebook's WAN Network, `Express Backbone`, as a sole IGP routing protocol.

### Goals
---

It started with it's first goal of building a simple and extensible routing
protocol for the Terragraph project and beyond.

**Simplicity** is understood in two dimensions:
* Maximum reuse of existing, well tested components (boost graph lib, thrift2,
  0MQ) for rapid development and code quality.
* Building on basic link-state routing principles and avoiding complex designs.

**Extensibility**
* quickly iterate on basic model and incrementally add functionality such as
  segment-routing label distribution, or loop-free alternatives, etc.

Our primary protocol features are:
* Shared data-bus - to allow adding distributed apps on top of it
* Fast convergence - handling local failures in under 100ms timeframe
* Secure bootstrap - avoiding unwanted participants in network routing
* Address delegation - plug-and-play address allocation.
* HW/SW Segregation - easily integrate with different hardware

We plan to add more functionality in the future, and this is where the
extensibility helps a lot. We are driven primarily by idea of autonomic
networking and ability to innovate quickly.

### Why another protocol ?
---

Our intent is not building a new protocol, but rather provide a modular solution
to build distributed applications in the network of any kind (wireless mesh,
data-center, WAN). One of this applications happen to be the routing system. If
you look at OSPF or ISIS, they have the distributed message flooding store as
part of their design. However, this store is not being leveraged for any other
purposes, rather than optimized for distributing routing/link-state information
only. We decouple the store and synchronize component of traditional IGPs into a
module of its own, and use this to add functionality in separate new modules -
e.g. a module for ECMP routing, loop-free alternate failover, bandwidth
allocation and so on. Effectively, our goal is to provide underlying substrate
to quickly iterate in the field of distributed network applications, whereas
traditional routing protocols are rigid systems that are slow to extend. This
`protocol` could be then universally used across different types of networks,
and tuned appropriately. In addition, we are looking to build upon the concepts
of autonomic networking and make our networks easier to configure and manage.

### Routing Protocol Design
---

The protocol implementation consists of the components depicted below:
* `KvStore` - Key Value Store
* `LinkMonitor` - Module to discover and monitor links on a given system
* `Spark` - Neighbor discovery module
* `Decision` - Routes computation unit
* `Fib` - Route programming unit, client interface

![openr-module-interaction-flow](https://user-images.githubusercontent.com/1482609/31962601-d95542ee-b8b2-11e7-8e6b-9ac38882e0b7.png)

There are five modules interacting together: all five will run on every node in
the network. We use ZMQ as the message transport bus, and Thrift for message
encoding. Two of the modules (Store & Sync aka KvStore + Link Monitor's Spark)
will be talking to the neighbors. Notice that unlike the traditional network
protocols we do not focus on the wire format or interaction logic - rather, this
is relegated to Thrift and ZMQ to deal with. Here is the high level of overview
for each module and you can read more about other modules in it's own section.


### Code Organization
---

Code is very modularized and each module is self contained. For more information
about each module please refer to documentation in header files of that module.
Code is very well documented. This README gives you an overview of how different
modules/libraries comes together to build OpenR.

### Documentation
---

High level overview of the documentation organization (alphabetically ordered)

#### [`Breeze.md`](Breeze.md)

Python click based CLI tool to interact with OpenR

#### [`Decision.md`](Decision.md)

Detailed overview about `openr/decision` module

#### [`DeveloperGuide.md`](DeveloperGuide.md)

Instructions on how to contribute to `OpenR` project and follow best
testing/coding practices.

#### [`Emulator.md`](Emulator.md)

Tool for generating/managing virtual mesh-network topologies for testing OpenR
(at scale ... ~1000+ nodes topologies). Network topology can span across
multiple servers.

#### [`Fib.md`](Fib.md)

Detailed overview about `openr/fib` module

#### [`KvStore.md`](KvStore.md)

Detailed overview about eventually replicated datastore, `openr/kvstore` and
it's operations as well as features.

#### [`LinkMonitor.md`](LinkMonitor.md)

Detailed overview about `openr/link-monitor` module

#### [`Miscellaneous.md`](Miscellaneous.md)

Miscellaneous notes and references e.g. various `OpenR` features like v4,
parallel links, drain and graceful restart, security concerns, marking control
plane packets and potential extensions.

#### [`Monitoring.md`](Monitoring.md)

Detailed overview about monitoring operations of OpenR. e.g. syslog, special
event logs (structured) and counters.

#### [`Platform.md`](Platform.md)

Guide for integrating OpenR on new platform and overview about `openr/platform`
codebase.

#### [`PrefixManager.md`](PrefixManager.md)

Managing prefix information in KvStore.

#### [`Spark.md`](Spark.md)

Neighbor discovery protocol of OpenR .... More simpler and extensible version
of well documented `bfd` protocol.

### Extra Readings
---
- [0MQ Internal Architecture](http://zeromq.org/whitepapers:architecture)
- [Terragraph](https://code.facebook.com/posts/1072680049445290/introducing-facebook-s-new-terrestrial-connectivity-systems-terragraph-and-project-aries/)
- [Express Backbone](https://code.facebook.com/posts/1782709872057497/building-express-backbone-facebook-s-new-long-haul-network/)
