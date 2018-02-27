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

ADVERTISE_INTERFACE_DB=false
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
ENABLE_SEGMENT_ROUTING=false
ENABLE_V4=false
ENABLE_WATCHDOG=true
FIB_HANDLER_PORT=60100
HEALTH_CHECKER_PING_INTERVAL_S=3
IFACE_PREFIXES="terra,nic1,nic2"
IFACE_REGEX_EXCLUDE=""
IFACE_REGEX_INCLUDE=""
LOOPBACK_IFACE="lo"
OVERRIDE_LOOPBACK_ADDR=false
PREFIXES=""
REDISTRIBUTE_IFACES="lo1"
SEED_PREFIX=""
SET_LOOPBACK_ADDR=false
SPARK_FASTINIT_KEEPALIVE_TIME_MS=100
SPARK_HOLD_TIME_S=30
SPARK_KEEPALIVE_TIME_S=3
STATIC_PREFIX_ALLOC=false
VERBOSITY=1

#
# Some sanity checks before we start OpenR
#

if [ "${HOSTNAME}" = "localhost" ]; then
  echo "No hostname found for the node, bailing out."
  exit 1
fi
NODE_NAME=${HOSTNAME}

#
# Load custom configuration if any!
#
OPENR_CONFIG="/etc/sysconfig/openr"
OPENR_ARGS="$*"
if [ ! -z "$1" ]; then
  if [ "$1" = "--help" ]; then
    echo "USAGE: run_openr.sh [config_file_path] [openr_flags]"
    echo "If config_file_path is not provided, we will source the one at \
/etc/sysconfig/openr"
    echo "If openr_flags are provided, they will be passed along to openr and \
override any passed by this script"
    exit 1
  fi
  if [ -f "$1" ]; then
    OPENR_CONFIG=$1
    OPENR_ARGS="${*:2}"
  fi
fi

if source "${OPENR_CONFIG}"; then
  echo "Using OpenR config parameters from ${OPENR_CONFIG}"
else
  echo "Configuration not found at ${OPENR_CONFIG}. Using default configuration"
fi

#
# Let the magic begin \m/
#

exec ${OPENR} \
  --advertise_interface_db=${ADVERTISE_INTERFACE_DB} \
  --alloc_prefix_len=${ALLOC_PREFIX_LEN} \
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
  --enable_segment_routing=${ENABLE_SEGMENT_ROUTING} \
  --enable_v4=${ENABLE_V4} \
  --fib_handler_port=${FIB_HANDLER_PORT} \
  --health_checker_ping_interval_s=${HEALTH_CHECKER_PING_INTERVAL_S} \
  --loopback_iface=${LOOPBACK_IFACE} \
  --iface_prefixes=${IFACE_PREFIXES} \
  --iface_regex_exclude=${IFACE_REGEX_EXCLUDE} \
  --iface_regex_include=${IFACE_REGEX_INCLUDE} \
  --node_name="${NODE_NAME}" \
  --override_loopback_addr=${OVERRIDE_LOOPBACK_ADDR} \
  --prefixes="${PREFIXES}" \
  --redistribute_ifaces=${REDISTRIBUTE_IFACES} \
  --seed_prefix=${SEED_PREFIX} \
  --set_loopback_address=${SET_LOOPBACK_ADDR} \
  --spark_fastinit_keepalive_time_ms=${SPARK_FASTINIT_KEEPALIVE_TIME_MS} \
  --spark_hold_time_s=${SPARK_HOLD_TIME_S} \
  --spark_keepalive_time_s=${SPARK_KEEPALIVE_TIME_S} \
  --static_prefix_alloc=${STATIC_PREFIX_ALLOC} \
  --enable_rtt_metric=${ENABLE_RTT_METRIC} \
  --enable_watchdog=${ENABLE_WATCHDOG} \
  --logbufsecs=0 \
  --logtostderr \
  --max_log_size=1 \
  --v=${VERBOSITY} \
  ${OPENR_ARGS}
