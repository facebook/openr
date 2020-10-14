`Runbook - Configuration Guide for OpenR`
-----------------------------------------

Good work on building and installing Open/R. Here are some ways you can run
Open/R. You should have the `openr` C++ binary, `run_openr.sh` script, and
python tool `breeze` all installed under the appropriate bin directory on your
system.

- `FibService` => route programming interface
FibService is main external service required to make Open/R functional,
which comes pre-compiled with the `openr` binary for Linux only and can be
enabled or disabled via command line flag or configuration options.

Checkout
[Platform.thrift](https://github.com/facebook/openr/blob/master/openr/if/Platform.thrift)
for more detail about these services.

### Quick Start
---

You can run the openr binary directly with some command line parameters and query
links from it

```
// On shell-1
$ openr --ifname_regex_include=eth.*

// On shell-2
$ breeze lm links
Interface    Status    Overloaded    Metric Override    ifIndex    Addresses
-----------  --------  ------------  -----------------  ---------  ------------------------
eth0         Up                                         2          169.254.0.13
                                                                   fe80::20a:f7ff:fe9a:3616
```

### run_openr.sh and Configuration file
---

The preferred way of running OpenR is via the  [run_openr.sh](https://github.com/facebook/openr/blob/master/openr/scripts/run_openr.sh)
script. The benefit of it instead of directly passing parameters is that, it
provides configuration options which are easier to work with and are less
fragile than command line options.

To configure Open/R, you can simply drop files consisting of configuration
options at `/etc/sysconfig/openr` and trigger an OpenR restart. An example
configuration file looks like following

```
DRYRUN=false
REDISTRIBUTE_IFACES=lo
IFACE_REGEX_INCLUDE=eth.*
IFACE_REGEX_EXCLUDE=
VERBOSITY=1
```

Run OpenR
```
$ run_openr.sh
```

You can also pass in a custom configuration file and override/add openr flags:
```
$ run_openr.sh --help
USAGE: run_openr.sh [config_file_path] [openr_flags]
If config_file_path is not provided, we will source the one at /etc/sysconfig/openr
If openr_flags are provided, they will be passed along to openr and override any passed by this script
```

### Running as a Daemon
---

`openr` should be run as a daemon so that if it crashes or node gets restarted,
the service comes up automatically. For newer Linux versions like CentOS we run
it as a `systemd` service. The following describes `openr.service` for systems
supporting systemd. You can write one based on your platform very easily (as all
it does is execute `run_openr.sh`).

```
Description=Facebook Open Routing Platform
After=network.target

[Service]
Type=simple
ExecStart=/usr/sbin/run_openr.sh
Restart=always
RestartSec=3
TimeoutSec=10
TimeoutStartSec=10
TimeoutStopSec=10
LimitNOFILE=10000000
LimitCORE=32G
SyslogIdentifier=openr
StandardOutput=syslog

[Install]
WantedBy=multi-user.target
```

### Configuration Options
---

#### OPENR

Path of `openr` binary on system. If binary is installed under searchable bin
paths then you don't need any change here.

```
OPENR=/usr/local/bin/openr
```

#### DOMAIN

Name of domain this node is part of. OpenR will `only` form adjacencies to
OpenR instances within it's own domain. This option becomes very useful if you
want to run OpenR on two nodes adjacent to each other but belonging to different
domains, e.g. Data Center and Wide Area Network. Usually it should depict the
Network.

```
DOMAIN=cluster10.dc3
```

#### LOOPBACK_IFACE

Indicates loopback address to which auto elected prefix will be assigned if
enabled.

```
LOOPBACK_IFACE=lo
```

#### REDISTRIBUTE_IFACES

Comma separated list of interface names whose `/32` (for v4) and `/128` (for v6)
should be announced. OpenR will monitor address add/remove activity on this
interface and announce it to rest of the network.

```
REDISTRIBUTE_IFACES=lo1
```

#### DECISION_GRACEFUL_RESTART_WINDOW_S

Set time interval to wait for convergence before OpenR calculates routes and
publishes them to the system. Set negative to disable this feature.

```
DECISION_GRACEFUL_RESTART_WINDOW_S=60
```

#### DRYRUN

OpenR will not try to program routes in it's default configuration. You should
explicitly set this option to false to proceed with route programming.

```
DRYRUN=true
```

#### ENABLE_RTT_METRIC

Default mechanism for cost of a link is `1` and hence cost of path is hop
count. With this option you can ask OpenR to compute and use RTT of a link as a
metric value. You should only use this for networks where links have significant
delay, on the order of a couple of milliseconds. Using this for point-to-point
links will cause lot of churn in metric updates as measured RTT will fluctuate a
lot because of packet processing overhead. RTT is measured at application level
and hence the fluctuation for point-to-point links.

```
ENABLE_RTT_METRIC=false
```

#### ENABLE_V4

OpenR supports v4 as well but it needs to be turned on explicitly. It is
expected that each interface will have v4 address configured for link local
transport and v4/v6 topologies are congruent.

```
ENABLE_V4=false
```

#### DEPRECATED_ENABLE_SUBNET_VALIDATION

OpenR supports subnet validation to avoid mis-cabling of v4 addresses on
different subnets on each end of the link. This is now by default enabled if v4 is enabled.

```
ENABLE_SUBNET_VALIDATION=true
```

#### ENABLE_LFA

With this option, additional Loop-Free Alternate (LFA) routes can be computed,
per RFC 5286, for fast failure recovery. Under the failure of all primary
nexthops for a prefix, because of link failure, next best precomputed LFA will
be used without need of an SPF run.

```
ENABLE_LFA=false
```

#### IFACE_REGEX_INCLUDE

Interface prefixes to perform neighbor discovery on. All interfaces whose
names start with these are used for neighbor discovery.

```
IFACE_REGEX_INCLUDE=eth.*,nic.*,po.*
```

#### IFACE_REGEX_EXCLUDE

Regex to exclude interface to perform neighbor discovery on. All interfaces whose
names start with these are excluded for neighbor discovery.

```
IFACE_REGEX_EXCLUDE=po[0-3]{3}
```
#### MIN_LOG_LEVEL

Log messages at or above this level. Again, the numbers of severity levels INFO,
WARNING, ERROR, and FATAL are 0, 1, 2, and 3, respectively. Defaults to 0

```
MIN_LOG_LEVEL=0
```


#### VERBOSITY

Show all verbose `VLOG(m)` messages for m less or equal the value of this flag.
Use higher value for more verbose logging. Defaults to 1

```
VERBOSITY=1
```


#### ENABLE_PREFIX_ALLOC

Enable prefix allocator to elect and assign a unique prefix for the node. You
will need to specify other configuration parameters below.

```
ENABLE_PREFIX_ALLOC=true
```

#### SEED_PREFIX

In order to elect a prefix for the node a super prefix to elect from is required.
This is only applicable when `ENABLE_PREFIX_ALLOC` is set to true.

```
SEED_PREFIX="face:b00c::/64"
```

#### ALLOC_PREFIX_LEN

Block size of allocated prefix in terms of it's prefix length. In this case
`/80` prefix will be elected for a node. e.g. `face:b00c:0:0:1234::/80`

```
ALLOC_PREFIX_LEN=80
```

#### SET_LOOPBACK_ADDR

If set to true along with `ENABLE_PREFIX_ALLOC` then second valid IP address of
the block will be assigned onto `LOOPBACK_IFACE` interface. e.g. in this case
`face:b00c:0:0:1234::1/80` will be assigned on `lo` interface.

```
SET_LOOPBACK_ADDR=true
```

#### OVERRIDE_LOOPBACK_ADDR

Whenever new address is elected for a node, before assigning it to interface
all previously allocated prefixes or other global prefixes will be overridden
with the new one. Use it with care!

```
OVERRIDE_LOOPBACK_ADDR=false
```

#### PREFIX_FWD_TYPE_MPLS

Prefix type can be IP or SR_MPLS. Based on the type either IP next hop or MPLS
label next hop is used for routing to the prefix. The type applies to both
prefix address and redistributed interface address.

#### PREFIX_FWD_ALGO_KSP2_ED_ECMP

Boolean variable to change prefix forwarding algorithm from standard
(Shortest path ECMP) to 2-Shortest-Path Edge Disjoint ECMP. Algorithm computes
edge disjoint second shortest ECMP paths for each destination prefix. MPLS
SR based tunneling is used for forwarding traffic over non-shortest paths.

This can be computationally expensive for networks exchanging large number of
routes. As per current implementation it will incur one extra SPF run per
destination prefix.

`PREFIX_FWD_TYPE_MPLS` must be set if `PREFIX_FWD_ALGO_KSP2_ED_ECMP` is set.

#### SPARK_HOLD_TIME_S

Hold time indicating time in seconds from its last hello after which neighbor
will be declared as down. Default value is `30 seconds`.

```
SPARK_HOLD_TIME_S=30
```

#### SPARK2_HELLO_TIME_S

How often to send helloMsg to neighbors. Default value is 20 seconds.

```
SPARK2_HELLO_TIME_S=20
```

#### SPARK2_HELLO_FASTINIT_MS

When interface is detected UP, OpenR can perform fast initial neighbor discovery.
Default value is 500 which means neighbor will be discovered within 1s on a link.

```
SPARK2_HELLO_FASTINIT_MS=500
```

#### SPARK2_HEARTBEAT_TIME_S

heartbeatMsg are used to detect if neighbor is up running when adjacency is established
Default value is 2s.

```
SPARK2_HEARTBEAT_TIME_S=2
```

#### SPARK2_HEARTBEAT_HOLD_TIME_S

Expiration time if node does NOT receive 'keepAlive' info from neighbor node.
Default value is 10s.

```
SPARK2_HEARTBEAT_HOLD_TIME_S=10
```

#### ENABLE_NETLINK_FIB_HANDLER

Knob to enable/disable default implementation of `FibService` that comes along
with OpenR for Linux platform. If you want to run your own FIB service then
disable this option

```
ENABLE_NETLINK_FIB_HANDLER=true
```

#### FIB_HANDLER_PORT

TCP port on which `FibService` will be listening.

```
FIB_HANDLER_PORT=60100
```

#### DECISION_DEBOUNCE_MIN_MS / DECISION_DEBOUNCE_MAX_MS

Knobs to control how often to run Decision. On receipt of first even debounce
is created with MIN time which grows exponentially up to max if there are more
events before debounce is executed. This helps us to react to single network
failures quickly enough (with min duration) while avoid high CPU utilization
under heavy network churn.

```
DECISION_DEBOUNCE_MIN_MS=10
DECISION_DEBOUNCE_MAX_MS=250
```

#### SET_LEAF_NODE

Sometimes a node maybe a leaf node and have only one path in to network. This
node does not require to keep track of the entire topology. In this case, it may
be useful to optimize memory by reducing the amount of key/vals tracked by the
node.

Setting this flag enables key prefix filters defined by KEY_PREFIX_FILTERS.
A node only tracks keys in kvstore that matches one of the prefixes in
KEY_PREFIX_FILTERS.

```
SET_LEAF_NODE=false
```

#### KEY_PREFIX_FILTERS

This comma separated string is used to set the key prefixes when key prefix
filter is enabled (See SET_LEAF_NODE).
It is also set when requesting KEY_DUMP from peer to request keys that match
one of these prefixes.

```
KEY_PREFIX_FILTERS="foo,bar"
```

#### ENABLE_PERF_MEASUREMENT

Experimental feature to measure convergence performance. Performance information
can be viewed via breeze API `breeze perf fib`

#### ENABLE_SEGMENT_ROUTING

Experimental and partially implemented segment routing feature. As of now it
only elects node/adjacency labels. In future we will extend it to compute and
program FIB routes.

```
ENABLE_SEGMENT_ROUTING=false
```

#### IP_TOS

Set type of service (TOS) value with which every control plane packet from
Open/R will be marked with. This marking can be used to prioritize control plane
traffic (as compared to data plane) so that congestion in network doesn't affect
operations of Open/R

```
IP_TOS=192
```

#### MEMORY_LIMIT_MB

Enforce upper limit on amount of memory in mega-bytes that open/r process can
use. Above this limit watchdog thread will trigger crash. Service can be
auto-restarted via system or some kind of service manager. This is very useful
to guarantee protocol doesn't cause trouble to other services on device where
it runs and takes care of slow memory leak kind of issues.

```
MEMORY_LIMIT_MB=300
```
#### KVSTORE_KEY_TTL_MS

Set the TTL (in ms) of a key in the KvStore. For larger networks where burst of
updates can be high having high value makes sense. For smaller networks where
burst of updates are low, having low value makes more sense.
Defaults to 300000 (5 min).

```
KVSTORE_KEY_TTL_MS=300000
```

#### KVSTORE_ZMQ_HWM

Set buffering size for KvStore socket communication. Updates to neighbor
node during flooding can be buffered upto this number. For larger networks where
burst of updates can be high having high value makes sense. For smaller networks
where burst of updates are low, having low value makes more sense.
Defaults to 65536.

```
KVSTORE_ZMQ_HWM=65536
```

#### ENABLE_FLOOD_OPTIMIZATION

Set this true to enable flooding-optimization, Open/R will start forming
spanning tree and flood updates on formed SPT instead of physical
topology. This will greatly reduce kvstore updates traffic, however, based on
which node is picked as flood-root, control-plane propagation might increase.
Before, propagation is determined by shortest path between two nodes. Now, it
will be the path between two nodes in the formed SPT, which is not necessary to
be the shortest path. (worst case: 2 x SPT-depth between two leaf nodes).
data-plane traffic stays the same.

```
ENABLE_FLOOD_OPTIMIZATION=false
```

#### IS_FLOOD_ROOT

Set this true to let this node declare itself as a flood-root. You can set
multiple nodes as flood-roots in a network, in steady state, open/r will pick
optimal (smallest node-name) one as the SPT for flooding. If optimal root went
away, open/r will pick 2nd optimal one as SPT-root and so on so forth. If all
root nodes went away, open/r will fall back to naive flooding.

```
IS_FLOOD_ROOT=false
```

### TLS Related Flags

We are in the process of adding TLS for all openr traffic. This will be
implemented with secure [Thrift](https://github.com/facebook/fbthrift).

The following flags allow you to enable TLS for your openr network.
Note: for a time while we transition, the plaintext zmq endpoints will remain
reachable.

To enable security, please pass the flags detailed below. For authentication,
specify acceptable common names via TLS_ACCEPTABLE_PEERS

#### ENABLE_SECURE_THRIFT_SERVER

Flag to enable TLS for our thrift server. Disable this for plaintext thrift.

#### X509_CERT_PATH

If we are running an SSL thrift server, this option specifies the certificate
path for the associated wangle::SSLContextConfig

#### X509_KEY_PATH

If we are running an SSL thrift server, this option specifies the key path for
the associated wangle::SSLContextConfig

#### X509_CA_PATH

If we are running an SSL thrift server, this option specifies the certificate
authority path for verifying peers

#### TLS_TICKET_SEED_PATH

If we are running an SSL thrift server, this option specifies the TLS ticket
seed file path to use for client session resumption

#### ECC_CURVE_NAME

If we are running an SSL thrift server, this option specifies the eccCurveName
for the associated wangle::SSLContextConfig

#### TLS_ACCEPTABLE_PEERS

A comma separated list of strings. Strings are x509 common names to accept SSL
connections from.

### Link Backoff - Improving Stabilility of Link State

Network can have some bad links that can keep flapping because of bad hardware
or environment (in case of wireless links). A single link flap event on one node
will cause SPF runs on all the devices. If such link continue to flap
continuously with some interval then it creates instability and unnecessary
churn in control plane.

Enabling link backoff can alleviate this problem. Main idea is to let link prove
itself stable for expected amount of time before using it. The duration to prove
stability increases with every flap within max-backoff.

#### LINK_FLAP_INITIAL_BACKOFF_MS

When link goes down after being stable/up for long time, then the backoff is
applied.

- If link goes up and down in backoff state then it's backoff gets doubled
  (Exponential backoff). Backoff clear timer will start from latest down
  event
- When backoff is cleared then actual status of link is used. If UP link then
  neighbor discovery is performed else Open/R will wait for link to come up.

#### LINK_FLAP_MAX_BACKOFF_MS

Serves two purposes
- This is the maximum backoff a link gets penalized with by consecutive flaps
- When the first backoff time has passed and the link is in UP state, the next
  time the link goes down within `LINK_FLAP_MAX_BACKOFF_MS`, the new backoff
  double of the previous backoff. If the link remains stable for
  `LINK_FLAP_MAX_BACKOFF_MS` then all of its history is erased and link gets
  `LINK_FLAP_INITIAL_BACKOFF_MS` next time when it goes down.

