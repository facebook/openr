#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

#
# Ports 60000 - 60100 needs to be reserved in system so that they are always
# free for openr to use. On linux you can do following to reserve.
# > sysctl -w net.ipv4.ip_local_reserved_ports=60000-60100
#

#
# Default OpenR configuration
# Override the ones you need in `/etc/sysconfig/openr` for custom configuration
# on the node
#

# OpenR binary path or command name present on bin paths
OPENR=openr

# Domain. Nodes will not form adjacencies to other nodes with different domain.
# One node can be part of only one domain. Useful when want to run OpenR on
# adjacent nodes but don't want to form adjacencies with them.
DOMAIN=openr

# List of comma separated list of prefixes to announce
# e.g. "face:cafe::1/128,face:b00c::/64"
PREFIXES="60.1.1.1/32,face:cafe::20/128,face:b00c::20/128"

# Used to assign elected address if prefix allocator is enabled
LOOPBACK_IFACE="lo"

# Announce all global addresses of interfaces into the network
REDISTRIBUTE_IFACES=""

# dryrun => Do not program routes in dryrun mode
DRYRUN=false

# IOS-XR IP address for Service Layer Access
IOSXR_SLAPI_IP="127.0.0.1"

# IOS-XR gRPC port for Service Layer Access
IOSXR_SLAPI_PORT="57777"

# Enable RTT metric on links. RTTs are computed dynamically and then used as
# cost for links. If disabled then hop count will be used as a cost of path
ENABLE_RTT_METRIC=true

# Enable v4
ENABLE_V4=true

# Enable health-checker
ENABLE_HEALTH_CHECKER=true
HEALTH_CHECKER_PING_INTERVAL_S=3

# Interface prefixes to perform neighbor discovery on. All interfaces whose
# names start with these are used for neighbor discovery
IFACE_PREFIXES="Gi,enp,Hg,Tg,Tf"

# Logging verbosity
VERBOSITY=2

# PrefixAllocator parameter
ENABLE_PREFIX_ALLOC=false # Enable automatic election of prefixes for nodes
SEED_PREFIX=""            # Master prefix to allocate subprefixes of nodes
ALLOC_PREFIX_LEN=128      # Length of allocated prefix
SET_LOOPBACK_ADDR=false   # Assign choosen prefix for node on loopback
                          # with prefix length as /128 (and address being first
                          # in elected network block
OVERRIDE_LOOPBACK_ADDR=false # Overrides other existing loopback addresses on
                             # loopback interface

# Spark Configuration
# How long to keep adjacency without hearing hello from neighbor
SPARK_HOLD_TIME_S=30
# How often to send hello packeks to neighbors
SPARK_KEEPALIVE_TIME_S=3
# How fast to perform initial neighbor discovery
SPARK_FASTINIT_KEEPALIVE_TIME_MS=100

# Enable in build Fib service handler
ENABLE_NETLINK_FIB_HANDLER=false
ENABLE_IOSXRSL_FIB_HANDLER=true
FIB_HANDLER_PORT=60100

# Enable in built System service handler
ENABLE_NETLINK_SYSTEM_HANDLER=true

# set decision debounce time
DECISION_DEBOUNCE_MIN_MS=10
DECISION_DEBOUNCE_MAX_MS=250

# enable performance measurements
ENABLE_PERF_MEASUREMENT=true

#
# Load custom configuration if any!
#

OPENR_CONFIG="/etc/sysconfig/openr"
source ${OPENR_CONFIG}
if [ $? -eq 0 ]; then
  echo "Using OpenR config parameters from ${OPENR_CONFIG}"
else
  echo "Configuration not found at ${OPENR_CONFIG}. Using default configuration"
fi

#
# Some sanity checks before we start OpenR
#

if [ "${HOSTNAME}" = "localhost" ]; then
  echo "No hostname found for the node, bailing out."
  exit 1
fi

#
# Let the magic begin \m/
#

exec ip netns exec global-vrf ${OPENR} \
  --chdir=/tmp \
  --domain=${DOMAIN} \
  --dryrun=${DRYRUN} \
  --prefix="${PREFIXES}" \
  --node_name="${HOSTNAME}" \
  --iface=${LOOPBACK_IFACE} \
  --redistribute_ifnames=${REDISTRIBUTE_IFACES} \
  --use_rtt_metric=${ENABLE_RTT_METRIC} \
  --enable_v4=${ENABLE_V4} \
  --enable_health_checker=${ENABLE_HEALTH_CHECKER} \
  --health_checker_ping_interval_s=${HEALTH_CHECKER_PING_INTERVAL_S} \
  --ifname_prefix=${IFACE_PREFIXES} \
  --enable_prefix_alloc=${ENABLE_PREFIX_ALLOC} \
  --seed_prefix=${SEED_PREFIX} \
  --alloc_prefix_len=${ALLOC_PREFIX_LEN} \
  --set_loopback_address=${SET_LOOPBACK_ADDR} \
  --override_loopback_global_addresses=${OVERRIDE_LOOPBACK_ADDR} \
  --spark_hold_time_s=${SPARK_HOLD_TIME_S} \
  --spark_keepalive_time_s=${SPARK_KEEPALIVE_TIME_S} \
  --spark_fastinit_keepalive_time_ms=${SPARK_FASTINIT_KEEPALIVE_TIME_MS} \
  --enable_netlink_fib_handler=${ENABLE_NETLINK_FIB_HANDLER} \
  --enable_iosxrsl_fib_handler=${ENABLE_IOSXRSL_FIB_HANDLER} \
  --enable_netlink_system_handler=${ENABLE_NETLINK_SYSTEM_HANDLER} \
  --decision_debounce_min_ms=${DECISION_DEBOUNCE_MIN_MS} \
  --decision_debounce_max_ms=${DECISION_DEBOUNCE_MAX_MS} \
  --enable_perf_measurement=${ENABLE_PERF_MEASUREMENT} \
  --logtostderr \
  --logbufsecs=0 \
  --max_log_size=1 \
  --v=${VERBOSITY} \
  --iosxr_slapi_port=${IOSXR_SLAPI_PORT} \
  --iosxr_slapi_ip=${IOSXR_SLAPI_IP} \
  --fib_agent_port=${FIB_HANDLER_PORT}
  "$@"
