# Range Allocator

### Introduction

---

As the name suggests, this module is responsible for allocating a unique integer
from a numeric range. The `uniqueness` is ensured across all the participating
allocators in the given `Area`. Uniqueness is achieved through distributed
computation using KvStore as the underlying data bus.

This is an abstract module that supports two use-cases

1. Election a unique label, aka node segment, similar to Label Distribution
   Protocol
2. Auto assignment of a unique prefix in a given network for each node.

This is an abstract module that supports two specific application in Open/R -
Node Segment Label Allocation and Prefix Allocation.

### Inter Module Communication

---

To ensure the uniqueness across all participating allocators, all RangeAllocator
instances would need to talk to each other. `KvStore` provides the communication
bus for data exchange.

RangeAllocator uses `KvStoreClientInternal` for communicating with KvStore.
Below we describe the diagram depicting the inter-module and inter-node
communication of RangeAllocator through KvStore.

![Range Allocator Communication through KvStore](https://user-images.githubusercontent.com/1482609/102537914-7641e480-4060-11eb-8bbb-55f63c83987d.png)

> NOTE: Only RangeAllocator instances of a particular `Area` interact with each
> other. The uniqueness of the label is not guaranteed across areas.

### Operations

---

`Numeric range` aka [low, high] is provided through the configuration on each
node. Below we describe the pseudocode of the RangeAllocator module

1. `Generate` a `random value` from range to be claimed
2. `Check` value `in KvStore`, if
   - `registered` - goto Step-1
   - `not registered` - goto Step-3
3. Register value in KvStore with current nodeId. Aka claim the value
4. Monitor value in KvStore. If it gets claimed by some other allocator
   instance, then goto Step-1

Some practical considerations

- Generation of a random value from the range can be controlled through
  different seed value on each node. This avoids conflicts in the initial
  election
- On restart of a node, it is beneficial to use value elected in the previous
  instance as the seed value
- Exponential backoff is used for retry of new allocation to avoid chocking
  KvStore data bus.
