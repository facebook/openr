`Breeze`
--------

`Breeze` is a `python-click` based CLI tool to peek into OpenR's state.
It allows you to inspect:

- Link state database
- Advertised prefix database
- Links discovered by OpenR
- Neighbors
- Computed routes along with Loop-Free Alternates
- Programmed routes in FIB
- Set/Unset overload bit for node and links or custom metrics on links
- Add/Del/Modify keys in KvStore


### How it Works
---

All OpenR modules expose various ZMQ APIs to access their internal state over
TCP transport. OpenR has [python based
clients](https://github.com/facebook/openr/tree/master/openr/py/openr/clients)
for each module which  Breeze leverages to talk to OpenR, retrieve
information, and display it

### How to use it
---

Breeze is very intuitive to use. Just do `--help` at any stage to see options,
arguments, and subcommands. Later sections cover about each
command in detail.

To get auto-completion in your bash shell, simply copy `eval
"$(_BREEZE_COMPLETE=source breeze)"` to your `~/.bashrc`.

```
$ breeze
Usage: breeze [OPTIONS] COMMAND [ARGS]...

  Command line tools for OpenR.

Options:
  -H, --host TEXT               Host to connect to (default = localhost)
  -t, --timeout INTEGER         Timeout for socket communication in ms
  -f, --ports-config-file TEXT  JSON file for ports config
  --color / --no-color          Enable coloring display
  --help                        Show this message and exit.

Commands:
  config         CLI tool to peek into Config Store module.
  decision       CLI tool to peek into Decision module.
  fib            CLI tool to peek into Fib module.
  healthchecker  CLI tool to peek into Health Checker module.
  kvstore        CLI tool to peek into KvStore module.
  lm             CLI tool to peek into Link Monitor module.
  monitor        CLI tool to peek into Monitor module.
  perf           CLI tool to view latest perf log of each...
  prefixmgr      CLI tool to peek into Prefix Manager module.```
```

### KvStore Commands
---

#### Nodes
You can identify all nodes in a network based on the content of KvStore.

```
$ breeze kvstore nodes

Node     V6-Loopback            V4-Loopback
-------  ---------------------  --------------
* node1  fc00:cafe:babe::1/128  192.168.0.1/32
> node2  fc00:cafe:babe::2/128  192.168.0.2/32
> node3  fc00:cafe:babe::3/128  192.168.0.3/32
> node4  fc00:cafe:babe::4/128  192.168.0.4/32
```

#### Prefix/Adjacency Database
Each node announces to the KvStore an Adjacency database, modeled as a list of
neighbors, and a Prefix database, the list of prefixes announced from that node.
Since every node has the same content in its KvStore (you can think of it as an
eventually consistent distributed database), each node has global knowledge of
the network.

By default `prefixes` and `adj` KvStore commands will show information for local
node only unless specified with `--nodes=all` or `--nodes=<node1>,<node2>`.

```
//
// See prefixes of a current node (node1)
//

$ breeze kvstore prefixes

> node1's prefixes
da00:cafe:babe:f6:f70b::/80
fc00:cafe:babe::1/128
192.168.0.1/32

//
// See adjacencies of node3
//

$ breeze kvstore adj --nodes=node3

> node3's adjacencies, version: 4, Node Label: 43049, Overloaded?: False
Neighbor    Local Interface    Remote Interface      Metric    Weight    Adj Label  NextHop-v4    NextHop-v6                 Uptime
node4       if_3_4_0           if_4_3_0                   1         1        50007  172.16.0.7    fe80::f498:41ff:fe0c:6c23  2m57s
node1       if_3_1_0           if_1_3_0                   1         1        50006  172.16.0.2    fe80::90be:bbff:fe2b:d4f7  2m57s

//
// See adjacencies of all nodes in a network
//

$ breeze kvstore adj --nodes=all
...
```

#### Inspect KvStore Content

More fun with KvStore. Read-write operations on replicated datastore via breeze
CLI.

- List keys in KvStore. Print all keys along with their TTL information.
```
$ breeze kvstore keys --ttl

Key                   OriginatorId      Version  TTL (HH:MM:SS)      TTL Version
--------------------  --------------  ---------  ----------------  -------------
adj:node1             node1                   4  0:05:00                       5
adj:node2             node2                   4  0:05:00                       5
adj:node3             node3                   4  0:05:00                       5
adj:node4             node4                   2  0:05:00                       5
allocprefix:16185099  node1                   1  0:05:00                       5
allocprefix:2710045   node2                   1  0:05:00                       5
allocprefix:401583    node3                   1  0:05:00                       5
allocprefix:6678890   node4                   1  0:05:00                       5
intf:node1            node1                   1  0:05:00                       5
intf:node2            node2                   1  0:05:00                       5
intf:node3            node3                   1  0:05:00                       5
intf:node4            node4                   1  0:05:00                       5
nodeLabel:1           node4                   1  0:05:00                       5
nodeLabel:19385       node2                   1  0:05:00                       5
nodeLabel:41983       node1                   1  0:05:00                       5
nodeLabel:43049       node3                   1  0:05:00                       5
prefix:node1          node1                   4  0:05:00                       5
prefix:node2          node2                   4  0:05:00                       5
prefix:node3          node3                   4  0:05:00                       5
prefix:node4          node4                   4  0:05:00                       5
```

- List specific key-values
```
$ breeze kvstore keyvals nodeLabel:1 intf:node4

> intf:node4 ---
00000000: 18 05 6E 6F 64 65 34 1B  02 8C 08 69 66 5F 34 5F  ..node4....if_4_
00000010: 32 5F 30 11 16 0E 19 1C  18 04 AC 10 00 05 00 19  2_0.............
00000020: 1C 18 10 FE 80 00 00 00  00 00 00 9C DB 34 FF FE  .............4..
00000030: 41 97 90 00 00 08 69 66  5F 34 5F 33 5F 30 11 16  A.....if_4_3_0..
00000040: 0C 19 1C 18 04 AC 10 00  07 00 19 1C 18 10 FE 80  ................
00000050: 00 00 00 00 00 00 F4 98  41 FF FE 0C 6C 23 00 00  ........A...l#..
00000060: 1C 19 1C 18 05 6E 6F 64  65 34 18 0F 49 4E 54 46  .....node4..INTF
00000070: 5F 44 42 5F 55 50 44 41  54 45 44 16 9A A5 D7 F0  _DB_UPDATED.....
00000080: F6 57 00 00 00                                    .W...

> nodeLabel:1 ---
00000000: 01 00 00 00                                       ....
```

- Add some custom key
```
// Set key with 60 seconds validity
$ breeze kvstore set-key test-key test-value --version=1 --ttl=60
Success: Set key test-key with version 1 and ttl 60000 successfully in KvStore.
This does not guarantee that value is updated in KvStore as old value can be
persisted back
```

- Get the key we just set
```
$ breeze kvstore keys --prefix=test-key --ttl

Key       OriginatorId      Version  TTL (HH:MM:SS)      TTL Version
--------  --------------  ---------  ----------------  -------------
test-key  breeze                  1  0:01:00                       1

$ breeze kvstore keyvals test-key

> test-key --- test-value
```

- Override the old value with a new one with a higher version
```
$ breeze kvstore set-key test-key test-value --version=2 --ttl=60
Success: Set key test-key with version 1 and ttl 60000 successfully in KvStore.
This does not guarantee that value is updated in KvStore as old value can be
persisted back

$ breeze kvstore keyvals test-key

> test-key --- new-test-value
```

- Erase key from KvStore. It is performed by setting TTL of the key to zero.

> NOTE: You shouldn't try to erase keys originated by OpenR

> NOTE: Erase only happens on all nodes reachable from the current node. Any
node momentarily disconnected from the network might bring back the key you just
erased.

```
$ breeze kvstore erase-key test-key
Success: key test-key will be erased soon from all KvStores.
```

#### KvStore Compare
Get the delta between contents of two KvStores. By default, the command will
compare each pair of nodes in the network. It will output nothing if the
kvstores are synced between nodes. There should be no difference if nodes are
part of the same network.

```
$ breeze kvstore kv-compare --nodes='fc00:cafe:babe::3'
dumped kv from fc00:cafe:babe::3
```

#### KvStore Signature
Returns a signature of the contents of the KV store for comparison with other
nodes.  In case of a mismatch, use `kv-compare` to analyze differences.

```
$ breeze kvstore kv-signature
sha256: d0bd6722917451be2f56a9b1e720ecffeb7e45f9c0e842d62dc90c1d047d870f
```

#### Peers
The KvStore of a node is connected to the KvStores of all immediate neighbors.
All updates are flooded via PUB/SUB channels between these stores and they
guarantee eventual consistency of data on all nodes. This command shows
information about a node's KvStore peers and details of the connections.

```
$ breeze kvstore peers

> node2
cmd via tcp://[fe80::e0fd:b7ff:fe23:e73f%if_1_2_0]:60002
pub via tcp://[fe80::e0fd:b7ff:fe23:e73f%if_1_2_0]:60001

> node3
cmd via tcp://[fe80::ecf4:d3ff:fef1:4cb6%if_1_3_0]:60002
pub via tcp://[fe80::ecf4:d3ff:fef1:4cb6%if_1_3_0]:60001
```

#### Snoop
Live snooping on KV-store updates in the network.


```
$ breeze -H 2401:db00:2120:20ed:feed::1 kvstore snoop

> Key: test-key update
version:         -->  1
originatorId:    -->  breeze


> Key: test-key update
version:  1  -->  2


> Key: test-key got expired

...
```

#### Visualize topology
Generates an image file with a visualization of the topology. Use `scp` to copy
it to your local machine to open the image file.

```
$ breeze kvstore topology
Saving topology to file => /tmp/openr-topology.png
```

### Decision Commands
---

#### Adjacency/Prefixes Database
Dump the link-state and prefixes databases from Decision module.

```
$ breeze decision adj

> node1's adjacencies, version: N/A, Node Label: 41983, Overloaded?: False
Neighbor    Local Interface    Remote Interface      Metric    Weight    Adj Label  NextHop-v4    NextHop-v6                 Uptime
node2       if_1_2_0           if_2_1_0                   1         1        50006  172.16.0.1    fe80::e0fd:b7ff:fe23:e73f  1h37m
node3       if_1_3_0           if_3_1_0                   1         1        50007  172.16.0.3    fe80::ecf4:d3ff:fef1:4cb6  1h37m

$ breeze decision prefixes

> node1's prefixes
fc00:cafe:babe::1/128
da00:cafe:babe:f6:f70b::/80
192.168.0.1/32
```


#### Path
List all the paths from a source node to a destination node based on the
link-state information in Decision module. Flag the paths that are actually
programmed into Fib module with `*`.

```
$ breeze decision path --src=node1 --dst=node4
2 paths are found.

  Hop  NextHop Node    Interface      Metric  NextHop-v6
    1  node2           if_1_2_0            2  fe80::e0fd:b7ff:fe23:e73f
    2  node4           if_2_4_0            1  fe80::9cdb:34ff:fe41:9790


  Hop  NextHop Node    Interface      Metric  NextHop-v6
    1  node3           if_1_3_0            2  fe80::ecf4:d3ff:fef1:4cb6
    2  node4           if_3_4_0            1  fe80::f498:41ff:fe0c:6c23
```

#### Routes
Request the computed routing table

```
$ breeze decision routes

> 192.168.0.2/32
via 172.16.0.1@if_1_2_0 metric 1

> 192.168.0.3/32
via 172.16.0.3@if_1_3_0 metric 1

> 192.168.0.4/32
via 172.16.0.1@if_1_2_0 metric 2
via 172.16.0.3@if_1_3_0 metric 2

> da00:cafe:babe:29:5a1d::/80
via fe80::e0fd:b7ff:fe23:e73f@if_1_2_0 metric 1

> da00:cafe:babe:65:e96a::/80
via fe80::e0fd:b7ff:fe23:e73f@if_1_2_0 metric 2
via fe80::ecf4:d3ff:fef1:4cb6@if_1_3_0 metric 2

> da00:cafe:babe:6:20af::/80
via fe80::ecf4:d3ff:fef1:4cb6@if_1_3_0 metric 1

> fc00:cafe:babe::2/128
via fe80::e0fd:b7ff:fe23:e73f@if_1_2_0 metric 1

> fc00:cafe:babe::3/128
via fe80::ecf4:d3ff:fef1:4cb6@if_1_3_0 metric 1

> fc00:cafe:babe::4/128
via fe80::e0fd:b7ff:fe23:e73f@if_1_2_0 metric 2
via fe80::ecf4:d3ff:fef1:4cb6@if_1_3_0 metric 2
```

#### Validate
Check all prefix & adj dbs in Decision against that in KvStore.

```
$ breeze decision validate
Decision is in sync with KvStore if nothing shows up
```

### LinkMonitor Commands
---

#### links
OpenR discovers local interfaces of a node (based on specified regular
expressions) and monitors their status via asynchronous Netlink APIs. It also
performs neighbor discovery on learned links.

```
$ breeze lm links

Interface    Status    Overloaded    Metric Override    ifIndex    Addresses
-----------  --------  ------------  -----------------  ---------  -------------------------
if_1_2_0     Up                                         6          172.16.0.0
                                                                   fe80::c2f:5bff:fe9a:1cdc
if_1_3_0     Up                                         7          172.16.0.2
                                                                   fe80::90be:bbff:fe2b:d4f7
```

#### set/unset link metric

Set/unset custom metric value for a link.

- Set custom link metric on one link and drain another link
> NOTE: Setting link overload will make link completely unusable for routing
while setting a high value as the link metric will make the link less
preferable, mimicking hard-drain and soft-drain behavior respectively.

```
$ breeze lm set-link-metric if_1_2_0 5

Are you sure to set override metric for interface if_1_2_0 ? [yn] y
Successfully set override metric for the interface.

$ breeze lm set-link-overload aq_1_3_0

Are you sure to set overload bit for interface aq_1_3_0 ? [yn] y
```

- List links
```
$ breeze lm links

Interface    Status    Overloaded    Metric Override    ifIndex    Addresses
-----------  --------  ------------  -----------------  ---------  -------------------------
if_1_2_0     Up                      5                  6          172.16.0.0
                                                                   fe80::c2f:5bff:fe9a:1cdc
if_1_3_0     Up        True                             7          172.16.0.2
                                                                   fe80::90be:bbff:fe2b:d4f7
```

- Undo changes with unset operation

```
$ breeze lm unset-link-metric if_1_2_0

Are you sure to unset override metric for interface if_1_2_0 ? [yn] y
Successfully unset override metric for the interface.

$ breeze lm unset-link-overload if_1_3_0

Successfully unset overload status of the interface.
```

#### set/unset node overload

Setting node overload will maintain reachability to/from this node but it will
not use the node for transit traffic.

- Set node overload
```
$ breeze lm set-node-overload

Are you sure to set overload bit for node node1 ? [yn] y
Successfully set overload bit..
```

- See that it is reflected in its adjacency DB
```
$ breeze kvstore adj

> node1's adjacencies, version: 10, Node Label: 41983, Overloaded?: True
Neighbor    Local Interface    Remote Interface      Metric    Weight    Adj Label  NextHop-v4    NextHop-v6                 Uptime
node2       if_1_2_0           if_2_1_0                   1         1        50006  172.16.0.1    fe80::e0fd:b7ff:fe23:e73f  1h51m
node3       if_1_3_0           if_3_1_0                   1         1        50007  172.16.0.3    fe80::ecf4:d3ff:fef1:4cb6  1h51m
```

- Unset node overload
```
$ breeze lm unset-node-overload

Are you sure to unset overload bit for node node1 ? [yn] y
Successfully unset overload bit..
```

### PrefixManager Commands

PrefixManager exposes APIs to list, advertise and withdraw prefixes into the
network for the current node.

#### view
List this node's currently advertised prefixes

```
$ breeze prefixmgr view

Type              Prefix
----------------  ---------------------------
LOOPBACK          fc00:cafe:babe::1/128
LOOPBACK          192.168.0.1/32
PREFIX_ALLOCATOR  da00:cafe:babe:f6:f70b::/80
```

#### advertise/withdraw

- Advertise a prefix from this node.

```
$ breeze prefixmgr advertise face:b00c::/64
Advertised face:b00c::/64
```

- Sync routes with new list of routes of a given type (BREEZE here)
```
$ breeze prefixmgr sync face:b00c::/80
Synced 1 prefixes with type BREEZE
```

- List current node's prefix database
```
$ breeze prefixmgr view

Type              Prefix
----------------  ---------------------------
LOOPBACK          fc00:cafe:babe::1/128
LOOPBACK          192.168.0.1/32
BREEZE            face:b00c::/80
PREFIX_ALLOCATOR  da00:cafe:babe:f6:f70b::/80
```

- Withdraw advertised routes
```
$ breeze prefixmgr withdraw face:b00c::/80
Withdrew face:b00c::/80
```

> NOTE: You can use different a prefix-type while advertising/syncing routes
with the `--prefix-type` option

### Config Commands
---

Config is stored as opaque key-values on the disk. Here are APIs to read and
update it.

#### List configs of various modules

- Dump link monitor config.
```
$ breeze config link-monitor
== Link monitor parameters stored ==

> isOverloaded: Yes

> nodeLabel: 5

> overloadedLinks: aq_2, aq_1

> linkMetricOverrides:

```

- Dump prefix allocation config.
```
$ breeze config prefix-allocator

== Prefix Allocator parameters stored  ==

> Seed prefix: 49.50.51.52/128

> Allocated prefix length: 80

> Allocated prefix index: 80
```

- Dump prefix manager config.
```
$ breeze config prefix-manager

== Prefix Manager parameters stored  ==

fc00:cafe:babe::3/128
da00:cafe:babe:94:4634::/80

```

#### Store/Erase configs manually (Not so useful commands)

- Erase a config key.
```
$ breeze config erase prefix-manager-config
Key erased

```


- Store a config key.
```
$ breeze config store prefix-allocator-config /tmp/config
Key stored
```

### Monitor Commands
---

#### counters
Fetch and display OpenR counters. You can programmatically fetch these counters
periodically via Monitor client and export them for monitoring purposes.

```
$ breeze monitor counters --prefix decision

decision.adj_db_update.count.0 : 16.0
decision.adj_db_update.count.3600 : 7.0
decision.adj_db_update.count.60 : 0.0
decision.adj_db_update.count.600 : 0.0
decision.paths_build_requests.count.0 : 30.0
decision.paths_build_requests.count.3600 : 22.0
decision.paths_build_requests.count.60 : 0.0
decision.paths_build_requests.count.600 : 0.0
decision.prefix_db_update.count.0 : 10.0
decision.prefix_db_update.count.3600 : 3.0
decision.prefix_db_update.count.60 : 0.0
decision.prefix_db_update.count.600 : 0.0
decision.spf.multipath_ms.avg.0 : 0.0
decision.spf.multipath_ms.avg.3600 : 0.0
decision.spf.multipath_ms.avg.60 : 0.0
decision.spf.multipath_ms.avg.600 : 0.0
decision.spf_runs.count.0 : 78.0
decision.spf_runs.count.3600 : 66.0
decision.spf_runs.count.60 : 0.0
decision.spf_runs.count.600 : 0.0
```

### FIB Commands
---

FIB is the only platform-specific part of OpenR. FibAgent runs as a separate
service on the node which is primarily responsible for managing the routing
tables on the node. Breeze also exposes commands to interact with FibAgent.

#### List/Add/Delete route via breeze!
> NOTE: You don't need to do this but they are just for fun!

This command only shows how to `add/del` routes. You can instead use the `sync`
command to replace all existing routing entries with newly specified routes.

- List routes
```
$ breeze -H leb01.labdca1 fib list

== leb01.labdca1's FIB routes ==

> 10.127.253.2/32
via 10.254.112.129@Port-Channel1201

> 2620:0:1cff:dead:bef1:ffff:ffff:2/128
via fe80::21c:73ff:fe3c:e50c@Port-Channel1201
```

- Add new route
```
// add route
$ breeze -H leb01.labdca1 fib add '192.168.0.0/32' '1.3.4.5@Port-Channel1201'
192.168.0.0 32
Added 1 routes.

// list routes
$ breeze -H leb01.labdca1 fib list

== leb01.labdca1's FIB routes ==

> 10.127.253.2/32
via 10.254.112.129@Port-Channel1201

> 192.168.0.0/32
via 1.3.4.5@Port-Channel1201

> 2620:0:1cff:dead:bef1:ffff:ffff:2/128
via fe80::21c:73ff:fe3c:e50c@Port-Channel1201
```

- Delete existing routes
```
// delete routes
$ breeze -H leb01.labdca1 fib del '192.168.0.0/32'
192.168.0.0 32
Deleted 1 routes.

// list routes
$ breeze -H leb01.labdca1 fib list

== leb01.labdca1's FIB routes ==

> 10.127.253.2/32
via 10.254.112.129@Port-Channel1201

> 2620:0:1cff:dead:bef1:ffff:ffff:2/128
via fe80::21c:73ff:fe3c:e50c@Port-Channel1201
```

#### Syncing routes
You can use sync API to synchronize routing table with fresh routing table
entries.

```
$ breeze -H leb01.labdca1 fib list

== leb01.labdca1's FIB routes ==

> 10.127.253.2/32
via 10.254.112.129@Port-Channel1201

> 2620:0:1cff:dead:bef1:ffff:ffff:2/128
via fe80::21c:73ff:fe3c:e50c@Port-Channel1201


$ breeze -H leb01.labdca1 fib sync '' ''
Reprogrammed FIB with 0 routes.
$ breeze -H leb01.labdca1 fib list

== leb01.labdca1's FIB routes ==

```

#### `linux` for platforms where we use Linux routing

One can then use `linux`  specific commands, the most useful being "validate"
which validates if the system (kernel routing table) has all the routes that
OpenR intended to program.  In case of a mismatch, the discrepancies will be
reported to aid debugging.

```
$ breeze fib validate-linux
PASS
Fib and Kernel routing table match
```

An example of reporting discrepancy
```
$ breeze fib validate-linux
FAIL
Fib and Kernel routing table do not match
Routes in kernel but not in Fib

> fc00:cafe:babe::999/128
via fe80::1@terra_241_3
```
