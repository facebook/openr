`PrefixAllocator`
-----------------

The class assigns each node with a unique prefix of a certain length from a
given seed prefix in a distributed manner. It is built atop RangeAllocator.
The seed prefix of length N is evenly divided into sub-prefixes of length M. The
underlying RangeAllocator allocates a unique integer I within [0, 2^(N - M) - 1]
and PrefixAllocator allocates the I-th prefix of length M.

There are three ways you can configure PrefixAllocator to achieve prefix
allocation, assignment as well advertisement.

Common configuration options for enabling prefix allocator is as below
```
// Enable prefix allocator
ENABLE_PREFIX_ALLOC=true

// Loopback interface on which to assign address
LOOPBACK_IFACE=lo

// Should elected address be assigned or not
SET_LOOPBACK_ADDR=true

// Remove any global address on the interface before assigning new addresses
OVERRIDE_LOOPBACK_ADDR=true
```

### Seeded Allocation via flags
`--seed_prefix` and `--alloc_prefix_len` using command line parameters. All
nodes must be initialized with same paramters for allocation to be effective.

Example configuration options are as below
```
// NOTE: Seed prefix must be set and static allocation set to false
SEED_PREFIX=face:b00c::/48
ALLOC_PREFIX_LEN=64
STATIC_PREFIX_ALLOC=false
```

### Seeded Allocation via KvStore
More flexible way is to initialize PrefixAllocator via KvStore. You can set a
special key `e2e-network-prefix` in KvStore with appropriate value which will
be learned by PrefixAllocator on all nodes, almost immediately and they will
trigger allocation process.

Example configuration options are as below
```
// NOTE: Seed prefix must be empty and static allocation set to false
SEED_PREFIX=
STATIC_PREFIX_ALLOC=false
```

An example for setting seed prefix is as below

```
// Value consists of two parts separated by comma
// seed-prefix: v6 seed prefix
// alloc-prefix-length: length of prefix to be allocated out of seed prefix
breeze kvstore set-key e2e-network-prefix face:b00c:cafe::/56,64
```

If you ever need to change seed prefix, you can just change it via KvStore
and all nodes will elect new prefix based on new seed prefix almost immediately.

```
// Extending range of seed prefix
breeze kvstore set-key e2e-network-prefix face:b00c:cafe::/48,64

// Changing seed prefix entirely to new one
breeze kvstore set-key e2e-network-prefix face:baba:c00c::/56,64
```

### Static Allocation via KvStore
In static allocation mode, you can control which address should be assigned to
which nodes via `thrift::StaticAllocation` object set in KvStore as special key,
`e2e-network-allocations`. From centralized controller, you can update value of
this key. Each node will lookup their address in StaticAllocation map and
assign their address if found, or withdraw previously assigned address if not
found.

Example configuration options are as below
```
// NOTE: Seed prefix must be empty and static allocation set to true
SEED_PREFIX=
STATIC_PREFIX_ALLOC=true
```

Example for setting static allocation via breeze as below, ideally it should
be done via `KvStoreClient::persistKey` from centralized Controller.

```
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
