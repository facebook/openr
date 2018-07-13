`Runbook - Configuration Guide for OpenR`
-----------------------------------------

Good work on building and installing Open/R. Here are some ways you can run
Open/R. You should have the `openr` C++ binary, `run_openr.sh` script, and
python tool `breeze` all installed under the appropriate bin directory on your
system.

There are two main external services required to make Open/R functional, both of
which comes pre-compiled with the `openr` binary for Linux only and can be
enabled or disabled via command line flag or configuration options. These are
- `FibService` => route programming interface
- `SystemService` => provides interface notifications to OpenR

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
IFACE_PREFIXES=eth
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

#### ADVERTISE_INTERFACE_DB

Boolean argument to enable/disable interface database advertisement into the
network. Disabled by default. Other applications can learn explicit interface
status of whole network via KvStore bus.

```
ADVERTISE_INTERFACE_DB=false
```

#### PREFIXES

Static list of comma separate prefixes to announce from the current node. Can't
be changed while running. Default value is empty

```
PREFIXES="face:cafe::1/128,face:b00c::/64"
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

#### ENABLE_SUBNET_VALIDATION

OpenR supports subnet validation to avoid mis-cabling of v4 addresses on
different subnets on each end of the link. Need to enable v4 and this flag at
the same time to turn on validation.

```
ENABLE_SUBNET_VALIDATION=true
```

#### ENABLE_HEALTH_CHECKER

OpenR can measure network health internally by pinging other nodes in the
network and exports this information as counters or via breeze APIs. By default
health checker is disabled. The expectation is that each node must have at least
one v6 loopback addressed announced into the network for the reachability check.

```
ENABLE_HEALTH_CHECKER=true
```

#### ENABLE_LFA

With this option, additional Loop-Free Alternate (LFA) routes can be computed,
per RFC 5286, for fast failure recovery. Under the failure of all primary
nexthops for a prefix, because of link failure, next best precomputed LFA will
be used without need of an SPF run.

```
ENABLE_LFA=false
```

#### HEALTH_CHECKER_PING_INTERVAL_S

Configure ping interval of the health checker. The below option configures it to
ping all other nodes every 3 seconds.

```
HEALTH_CHECKER_PING_INTERVAL_S=3
```

#### IFACE_PREFIXES

Interface prefixes to perform neighbor discovery on. All interfaces whose
names start with these are used for neighbor discovery.

```
IFACE_PREFIXES=eth,nic,po
```

#### VERBOSITY

Set logging verbosity of OpenR logs

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

#### SPARK_HOLD_TIME_S

Hold time indicating time in seconds from it's last hello after which neighbor
will be declared as down. Default value is `30 seconds`.

```
SPARK_HOLD_TIME_S=30
```

#### SPARK_KEEPALIVE_TIME_S

How often to send spark hello messages to neighbors. Default value is 3 seconds.

```
SPARK_KEEPALIVE_TIME_S=3
```

#### SPARK_FASTINIT_KEEPALIVE_TIME_MS

When interface is detected UP, OpenR can perform fast initial neighbor discovery
as opposed to slower keep alive packets. Default value is 100 which means
neighbor will be discovered within 200ms on a link.

```
SPARK_FASTINIT_KEEPALIVE_TIME_MS=100
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

#### ENABLE_NETLINK_SYSTEM_HANDLER

Knob to enable/disable default implementation of `SystemService` and
`PlatformPublisher` that comes along with OpenR for Linux platform. If you want
to run your own SystemService then disable this option.

```
ENABLE_NETLINK_SYSTEM_HANDLER=true
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

Experinmental and partially implemented segment routing feature. As of now it
only elects node/adjacency labels. In future we will extend it to compute and
program FIB routes.

```
ENABLE_SEGMENT_ROUTING=false
```

#### IP_TOS

Set type of service (TOS) value with which every control plane packet from
Open/R will be marked with. This marking can be used to prioritize control plane
traffic (as compared to data plane) so that congention in network doesn't affect
operations of Open/R

```
IP_TOS=192
```
