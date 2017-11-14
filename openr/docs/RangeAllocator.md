`RangeAllocator`
-------------

RangeAllocator is an abstract logic built on the top of `KvStore` communication
channel to elect a unique integer value from a given range in a distributed application. 
Each node can claim a value and submit it to KvStore. 
If multiple nodes claim the same value, the highest priority one (i.e., highest originator ID) wins. 
Others backoff and try another value. This process repeats till every node gets a unique ID or the whole range is exhausted.

We use this to elect a unique label (similar to Label Distribution Protocol) as well as auto assignment of an unique prefix in a given network for each node.
