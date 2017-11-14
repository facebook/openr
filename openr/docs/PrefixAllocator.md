`PrefixAllocator`
-----------------

The class assigns each node with a unique prefix of certain length from a given seed prefix in a distributed manner.
It is built atop RangeAllocator. The seed prefix of length N is evenly divided into sub-prefix of length M. 
The underlying RangeAllocator allocates a unique integer I within [0, 2^(N - M) - 1] and PrefixAllocator allocates I-th prefix of length M.
