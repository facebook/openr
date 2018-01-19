`PrefixAllocator`
-----------------

The class assigns each node with a unique prefix of a certain length from a
given seed prefix in a distributed manner. It is built atop RangeAllocator.
The seed prefix of length N is evenly divided into sub-prefixes of length M. The
underlying RangeAllocator allocates a unique integer I within [0, 2^(N - M) - 1]
and PrefixAllocator allocates the I-th prefix of length M.

There are two ways you can feed input paramters to PrefixAllocator

### Via flags
`--seed_prefix` and `--alloc_prefix_len` using command line parameters. All
nodes must be initialized with same paramters for allocation to be effective.

### Via KvStore
More flexible way is to initialize PrefixAllocator via KvStore. You can set a
special key `e2e-network-prefix` in KvStore with appropriate value which will
be learned by PrefixAllocator on all nodes, almost immediately and they will
trigger allocation process. An example is as follows

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
