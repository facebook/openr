# Configuration Guide

Good work on building and installing Open/R. Here are some ways you can run
Open/R. You should have the `openr` C++ binary, a JSON configuration and a
python CLI tool `breeze` all installed under the appropriate bin directory on
your system.

- `FibService` => route programming interface FibService is main external
  service required to make Open/R functional, which comes pre-compiled with the
  `openr` binary for Linux only and can be enabled or disabled via command line
  flag or configuration options.

Checkout
[Platform.thrift](https://github.com/facebook/openr/blob/master/openr/if/Platform.thrift)
for more detail about these services.

## Quick Start
---

You can run the openr binary directly with some command line parameters and
query links from it

```console
// On shell-1
$ openr --ifname_regex_include=eth.*

// On shell-2
$ breeze lm links
Interface    Status    Overloaded    Metric Override    ifIndex    Addresses
-----------  --------  ------------  -----------------  ---------  ------------------------
eth0         Up                                         2          169.254.0.13
                                                                   fe80::20a:f7ff:fe9a:3616
```

Run OpenR

```console
/path/to/openr -v 2 --config /path/to/openr.confg
```

You can also pass in a custom configuration file and override/add openr flags:

### Running as a Daemon

`openr` should be run as a daemon so that if it crashes or node gets restarted,
the service comes up automatically. For newer Linux versions like CentOS we run
it as a `systemd` service. The following describes `openr.service` for systems
supporting systemd.

```ini
Description=Facebook Open Routing Platform
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/openr -v 2 --config /etc/openr.conf
Restart=always
RestartSec=3
TimeoutSec=10
TimeoutStartSec=10
TimeoutStopSec=10
LimitNOFILE=10000000
LimitCORE=32G

[Install]
WantedBy=multi-user.target
```

## Thrift-based JSON Configuration File
---

The configuration file is expected to be in JSON format of the Open/R
configuration thrift model,
[OpenrConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/OpenrConfig.thrift).
This model contains various knobs which were provided as command line
configuration options before. Please check it out for configuration
specification.

## Command Line Configuration Options
---

### MIN_LOG_LEVEL

Log messages at or above this level. Again, the numbers of severity levels INFO,
WARNING, ERROR, and FATAL are 0, 1, 2, and 3, respectively. Defaults to 0

```shell
MIN_LOG_LEVEL=0
```

### VERBOSITY

Show all verbose `VLOG(m)` messages for m less or equal the value of this flag.
Use higher value for more verbose logging. Defaults to 1

- Control using the `-v` CLI argument to openr

### DECISION_DEBOUNCE_MIN_MS / DECISION_DEBOUNCE_MAX_MS

Knobs to control how often to run Decision. On receipt of first even debounce is
created with MIN time which grows exponentially up to max if there are more
events before debounce is executed. This helps us to react to single network
failures quickly enough (with min duration) while avoid high CPU utilization
under heavy network churn.

```shell
DECISION_DEBOUNCE_MIN_MS=10
DECISION_DEBOUNCE_MAX_MS=250
```

### ENABLE_PERF_MEASUREMENT

Experimental feature to measure convergence performance. Performance information
can be viewed via breeze API `breeze perf fib`

### IP_TOS

Set type of service (TOS) value with which every control plane packet from
Open/R will be marked with. This marking can be used to prioritize control plane
traffic (as compared to data plane) so that congestion in network doesn't affect
operations of Open/R

```shell
IP_TOS=192
```

### TLS Related Flags

We are in the process of adding TLS for all openr traffic. This will be
implemented with secure [Thrift](https://github.com/facebook/fbthrift).

The following flags allow you to enable TLS for your openr network. Note: for a
time while we transition, the plaintext thrift endpoint will remain reachable.

To enable security, please pass the flags detailed below. For authentication,
specify acceptable common names via TLS_ACCEPTABLE_PEERS

### ENABLE_SECURE_THRIFT_SERVER

Flag to enable TLS for our thrift server. Disable this for plaintext thrift.

### X509_CERT_PATH

If we are running an SSL thrift server, this option specifies the certificate
path for the associated wangle::SSLContextConfig

### X509_KEY_PATH

If we are running an SSL thrift server, this option specifies the key path for
the associated wangle::SSLContextConfig

### X509_CA_PATH

If we are running an SSL thrift server, this option specifies the certificate
authority path for verifying peers

### TLS_TICKET_SEED_PATH

If we are running an SSL thrift server, this option specifies the TLS ticket
seed file path to use for client session resumption

### ECC_CURVE_NAME

If we are running an SSL thrift server, this option specifies the eccCurveName
for the associated wangle::SSLContextConfig

### TLS_ACCEPTABLE_PEERS

A comma separated list of strings. Strings are x509 common names to accept SSL
connections from.
