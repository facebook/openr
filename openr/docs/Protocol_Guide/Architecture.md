# Architecture

Everything is designed but only a few things are designed well. Open/R is one of
the few. It is primarily designed to facilitate distributed computation on a
network of nodes. Routing is a core functionality implemented on the top, unlike
other protocols where design is greatly influenced by routing functions.

## Design Principles

---

**CPU & Memory**

Premature optimization is the root of all evil. Unlike traditional routing
protocol designs, we're intentionally ignoring the optimization considerations
for `memory` and `compute`. The design should solely focus on core
functionalities. Further, the processing power and memory size of core
networking devices have improved manyfold over the past few decades.

**Modularity**

Make a program do one thing and do it better
([p.s. Unix Philosphy](https://en.wikipedia.org/wiki/Unix_philosophy)).
Borrowing ideas of `simplicity`, `modularity`, and `extensibility` in the design
of Open/R. Read more about `Modules` below.

**Build Abstractions**

Avoid complex design patterns. With the modular design, build new things on the
top rather than extending existing modules.

**Leverage Existing**

Don't re-invent the wheel. Maximize the reuse of existing and well-tested
libraries/components (e.g. thrift, folly). This helps us with rapid development
and a lower code footprint to maintain. Hence we strive to focus on solving the
right problem.

**Asynchronous**

Use asynchronous communication patterns to bake efficiency in the design itself.

## Modules

Open/R process consists of multiple modules stitched together with a messaging
interface. Below we describe module invariants

- Module is a C++ class instantiation
- Implements `Runnable` interface. starts/stop/join
- Is independent of other modules. i.e. You can instantiate the module without
  creating an instance of other modules
- It does one thing and does it well
- Provides thread-safe public APIs for accessing or modifying internal state for
  RPC APIs
- Uses [Messaging](Messaging.md) interface for talking to other modules

The picture below describes Open/R modules and their interactions

![Open/R Architecture](https://user-images.githubusercontent.com/1482609/102832805-4ec28300-43a4-11eb-9752-7f0e617b067b.png)

The diagram above doesn't intend to capture every single detail of the message
flow. For more details refer to the module's documentation.

> NOTE: BGP Speaker functionality is not open-sourced, but it is possible for a
> developer to create integrate with BGP with their own implementation.

## Messages

---

Every arrow in the picture above depicts a flow of messages from one module to
another. Such flow of messages is powered by [Messaging](Messaging.md) library.
Multiple arrows may be supported by just one queue e.g. RIB Updates from the
Decision to multiple modules (Fib, Decision, PrefixManager) is supported by
single queue instance.

For more details on all the queue types and their message type refer to
`openr/Main.cpp`. All queue initialization happens in that code.
