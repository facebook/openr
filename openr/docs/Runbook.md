`Runbook - Configuration Guide for OpenR`
-----------------------------------------

Good work on building and installing Open/R. Here are some ways you can run
Open/R. You should have `openr` C++ binary, `run_openr.sh` script and python
tool `breeze` and installed under appropriate bin directory on the system.

There are two main external services required to make Open/R functional, both of
which comes pre-compiled into `openr` binary for Linux only and can be enabled
or disabled via command line flag or configuration options. These are
- `FibService` => route programming interface
- `SystemService` => provides interface notifications to OpenR

Checkout
[Platform.thrift](https://github.com/facebook/openr/blob/master/openr/if/Platform.thrift)
for more detail about these services.

### Quick Start
---

You can run openr binary directly with some command line parameter and query
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

Preferred way of running OpenR is via [run_openr.sh](https://github.com/facebook/openr/blob/master/openr/scripts/run_openr.sh) script. Benefits
of it instead of directly passing parameters is that, it provides configuration
options which are easier to work with and are less fragile compared to command
line options.

To configure Open/R, you can simply drop file consisting of configuration
option at `/etc/sysconfig/openr` and trigger OpenR restart. An example
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

### Running as a Daemon
---

`openr` should be run as a daemon so that if crashed or node get restarted, the
service comes up automatically. For newer Linux versions like CentOS we run it
as `systemd` service. Following describes `openr.service` for systems supporting
systemd. You can write one based on your platform very easily (as all it does is
to execute `run_openr.sh`).

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
OpenR within it's own domain. This option becomes very useful when we you want
to run OpenR on two nodes adjacent to each other but belongs to different domain
e.g. Data Center and Wide Area Network. Usually it should depict the Network.

```
DOMAIN=cluster10.dc3
```

#### PREFIXES

Static list of comma separate prefixes to announce from current node. Can't be
changed while running it. Default value is empty

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
count. With this option you can ask OpenR to compute and use RTT of link as a
metric value. You should only use this for network where links have significant
delay, in order of couple of milliseconds. Using this for point-to-point link
will cause lot of churn in metric updates as measure RTT will fluctuate a lot
because of packet processing overhead. RTT is measured at application level
and hence the fluctuation for point-to-point links.

```
ENABLE_RTT_METRIC=false
```

#### ENABLE_V4

OpenR supports v4 as well but it needed to be turned on explicitly. It is
expected that each interface will have v4 address configured for link local
transport and v4/v6 topologies are congruent.

```
ENABLE_V4=false
```

#### ENABLE_HEALTH_CHECKER

OpenR can measure network health internally by pinging other nodes in network
and export this information as counters or via breeze APIs. By default
health checker is disabled. Expectation is that each node must have at least one
v6 loopback addressed announced into the network for reachability check.

```
ENABLE_HEALTH_CHECKER=true
```

#### HEALTH_CHECKER_PING_INTERVAL_S

Configure ping interval of health checker. Below option configures it to ping
all other nodes every 3 seconds.

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

Enable prefix allocator to elect and assign a unique prefix for node. You will
need to specify other configuration parameters below.

```
ENABLE_PREFIX_ALLOC=true
```

#### SEED_PREFIX

In order to elect a prefix for node a super prefix to elect from is required.
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

Knowb to enable/disable default implementation of `SystemService` and
`PlatformPublisher` that comes along with OpenR for Linux platform. If you want
to run your own SystemService then disable this option.

```
ENABLE_NETLINK_SYSTEM_HANDLER=true
```

#### DECISION_DEBOUNCE_MIN_MS / DECISION_DEBOUNCE_MAX_MS

Knobs to control how often to run Decision. On receipt of first even debounce
is created with MIN time which grows exponentially upto max if there are more
events before debounce is executed. This helps us to react to single network
failures quickly enough (with min duration) while avoid high CPU utilization
under heavy network churn.

```
DECISION_DEBOUNCE_MIN_MS=10
DECISION_DEBOUNCE_MAX_MS=250
```

#### ENABLE_PERF_MEASUREMENT

Experimental feature to measure convergence performance. Performance information
can be viewed via breeze API `breeze perf fib`
