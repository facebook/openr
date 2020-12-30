# PrefixAllocator - Auto address allocation

## Introduction

---

PrefixAllocator built on the top of [Range Allocator](RangeAllocator.md). As the
name suggests, this module assigns each node with a unique prefix of a certain
length from a given seed prefix in a distributed fashion. The seed prefix of
length `N` is evenly divided into sub-prefixes of length `M`. The underlying
RangeAllocator allocates a unique integer `I` within `[0, 2^(N - M) - 1]` and
PrefixAllocator allocates the `I-th` prefix of length `M`.

NOTE: This functionality is deprecated and will soon be removed from Open/R

## Inter-Module Communication

---

See
[Range Allocator Inter-Module Communication](RangeAllocator.md) section

## Configuration

---

There are three ways you can configure PrefixAllocator to achieve prefix
allocation, assignment as well advertisement.

Checkout `PrefixAllocationConfig` in
[if/OpenrConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/PrefixManager.thrift)
for more details on configuration.

Common configuration options for enabling prefix allocator is as below

```python
openr_config = OpenrConfig()

# Enable prefix allocator and initialize prefix allocation config
openr_config.enable_prefix_allocation = True
prefix_allocation_config = PrefixAllocationConfig()
openr_config.prefix_allocation_config = prefix_allocation_config

# Loopback interface on which to assign address
prefix_allocation_config.loopback_interface = "lo"

# Should elected address be assigned or not
prefix_allocation_config.set_loopback_addr = True

# Remove any global address on the interface before assigning new addresses
prefix_allocation_config.override_loopback_addr = True
```

### Seeded Allocation via `FLAGS`

`seed_prefix` and `alloc_prefix_len` using command line parameters. All nodes
must be initialized with same paramters for allocation to be effective.

Example configuration options are as below

```python
// Set allocation mode
prefix_allocation_config.prefix_allocation_mode = PrefixAllocationMode.DYNAMIC_ROOT_NODE

// NOTE: Seed prefix must be set
prefix_allocation_config.seed_prefix = "face:b00c::/48"
prefix_allocation_config.allocate_prefix_len = 64
```

### Seeded Allocation via `KvStore`

More flexible way is to initialize PrefixAllocator via KvStore. You can set a
special key `e2e-network-prefix` in KvStore with appropriate value which will be
learned by PrefixAllocator on all nodes, almost immediately and they will
trigger allocation process.

Example configuration options are as below

```python
// Set allocation mode
prefix_allocation_config.prefix_allocation_mode = PrefixAllocationMode.DYNAMIC_LEAF_NODE

// NOTE: Seed prefix must be empty
prefix_allocation_config.seed_prefix = None
prefix_allocation_config.allocate_prefix_len = None
```

An example for setting seed prefix is as below

```console
// Value consists of two parts separated by comma
// seed-prefix: v6 seed prefix
// alloc-prefix-length: length of prefix to be allocated out of seed prefix
breeze kvstore set-key e2e-network-prefix face:b00c:cafe::/56,64
```

If you ever need to change seed prefix, you can just change it via KvStore and
all nodes will elect new prefix based on new seed prefix almost immediately.

```console
// Extending range of seed prefix
breeze kvstore set-key e2e-network-prefix face:b00c:cafe::/48,64

// Changing seed prefix entirely to new one
breeze kvstore set-key e2e-network-prefix face:baba:c00c::/56,64
```

### Static Allocation via `KvStore`

In static allocation mode, you can control which address should be assigned to
which nodes via `thrift::StaticAllocation` object set in KvStore as special key,
`e2e-network-allocations`. From centralized controller, you can update value of
this key. Each node will lookup their address in StaticAllocation map and assign
their address if found, or withdraw previously assigned address if not found.

Example configuration options are as below

```python
// Set allocation mode
prefix_allocation_config.prefix_allocation_mode = PrefixAllocationMode.STATIC

// NOTE: Seed prefix must be empty
prefix_allocation_config.seed_prefix = None
prefix_allocation_config.allocate_prefix_len = None
```

Example for setting static allocation via breeze as below, ideally it should be
done via `KvStoreClient::persistKey` from centralized Controller.

```console
// Set static allocation for node1 and node2 (they will learn and assign new
// addresses immediately
breeze kvstore alloc-set node1 10.126.3.0/24
breeze kvstore alloc-set node2 10.126.4.0/24

// View all static allocations
breeze kvstore alloc-list

// Breeze advertised prefixes from node1 and node2
breeze kvstore prefixes --nodes=node1,node2

// Unset allocations
breeze kvstore alloc-unset node1
breeze kvstore alloc-unset node2
```
