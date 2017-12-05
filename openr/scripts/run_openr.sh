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

#
# For more info on openr's options please refer to the openr/docs/Runbook.md
#

# OpenR binary path or command name present on bin paths
OPENR=openr

ALLOC_PREFIX_LEN=128
CONFIG_STORE_FILEPATH="/tmp/aq_persistent_config_store.bin"
DECISION_DEBOUNCE_MAX_MS=250
DECISION_DEBOUNCE_MIN_MS=10
DOMAIN=openr
DRYRUN=false
ENABLE_HEALTH_CHECKER=false
ENABLE_NETLINK_FIB_HANDLER=false
ENABLE_NETLINK_SYSTEM_HANDLER=true
ENABLE_PERF_MEASUREMENT=true
ENABLE_PREFIX_ALLOC=false
ENABLE_RTT_METRIC=true
ENABLE_V4=false
FIB_HANDLER_PORT=60100
HEALTH_CHECKER_PING_INTERVAL_S=3
IFACE_PREFIXES="terra,nic1,nic2"
LOOPBACK_IFACE="lo"
OVERRIDE_LOOPBACK_ADDR=false
PREFIXES=""
REDISTRIBUTE_IFACES="lo1"
SEED_PREFIX=""
SET_LOOPBACK_ADDR=false
SPARK_FASTINIT_KEEPALIVE_TIME_MS=100
SPARK_HOLD_TIME_S=30
SPARK_KEEPALIVE_TIME_S=3
VERBOSITY=1

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

exec ${OPENR} \
  --alloc_prefix_len=${ALLOC_PREFIX_LEN} \
  --chdir=/tmp \
  --config_store_filepath=${CONFIG_STORE_FILEPATH} \
  --decision_debounce_max_ms=${DECISION_DEBOUNCE_MAX_MS} \
  --decision_debounce_min_ms=${DECISION_DEBOUNCE_MIN_MS} \
  --domain=${DOMAIN} \
  --dryrun=${DRYRUN} \
  --enable_health_checker=${ENABLE_HEALTH_CHECKER} \
  --enable_netlink_fib_handler=${ENABLE_NETLINK_FIB_HANDLER} \
  --enable_netlink_system_handler=${ENABLE_NETLINK_SYSTEM_HANDLER} \
  --enable_perf_measurement=${ENABLE_PERF_MEASUREMENT} \
  --enable_prefix_alloc=${ENABLE_PREFIX_ALLOC} \
  --enable_v4=${ENABLE_V4} \
  --fib_agent_port=${FIB_HANDLER_PORT}
  --health_checker_ping_interval_s=${HEALTH_CHECKER_PING_INTERVAL_S} \
  --iface=${LOOPBACK_IFACE} \
  --ifname_prefix=${IFACE_PREFIXES} \
  --node_name="${HOSTNAME}" \
  --override_loopback_global_addresses=${OVERRIDE_LOOPBACK_ADDR} \
  --prefix="${PREFIXES}" \
  --redistribute_ifnames=${REDISTRIBUTE_IFACES} \
  --seed_prefix=${SEED_PREFIX} \
  --set_loopback_address=${SET_LOOPBACK_ADDR} \
  --spark_fastinit_keepalive_time_ms=${SPARK_FASTINIT_KEEPALIVE_TIME_MS} \
  --spark_hold_time_s=${SPARK_HOLD_TIME_S} \
  --spark_keepalive_time_s=${SPARK_KEEPALIVE_TIME_S} \
  --use_rtt_metric=${ENABLE_RTT_METRIC} \
  --logbufsecs=0 \
  --logtostderr \
  --max_log_size=1 \
  --v=${VERBOSITY} \
  "$@"
