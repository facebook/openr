#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

#
# NOTE
#
# Ports 60000 - 60100 needs to be reserved in system so that they are always
# free for openr to use. On linux you can do following to reserve.
# > sysctl -w net.ipv4.ip_local_reserved_ports=60000-60100
#

#
# NOTE
#
# Default OpenR configuration
# Override the ones you need in `/etc/sysconfig/openr` for custom configuration
# on the node or pass config name to this script
# e.g. run_openr.sh /data/openr.cfg
#

#
# NOTE
#
# Change `CONFIG_STORE_FILEPATH` to path which is persistent across reboot for
# correct functioning of OpenR across reboot (e.g. preserving drain state)
# e.g. CONFIG_STORE_FILEPATH=/data/openr_config_store.bin
#

#
# For more info on openr's options please refer to the openr/docs/Runbook.md
#

# OpenR binary path or command name present on bin paths
OPENR=openr

# Keep this list in sorted order
ADVERTISE_INTERFACE_DB=false
ALLOC_PREFIX_LEN=128
ASSUME_DRAINED=false
CONFIG_STORE_FILEPATH="/tmp/aq_persistent_config_store.bin"
DECISION_DEBOUNCE_MAX_MS=250
DECISION_DEBOUNCE_MIN_MS=10
DOMAIN=openr
DRYRUN=false
ENABLE_SUBNET_VALIDATION=true
ENABLE_FIB_SYNC=false
ENABLE_HEALTH_CHECKER=false
ENABLE_LEGACY_FLOODING=true
ENABLE_LFA=false
ENABLE_NETLINK_FIB_HANDLER=true
ENABLE_NETLINK_SYSTEM_HANDLER=true
ENABLE_OLD_DECISION_MODULE=false
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
IP_TOS=140
KEY_PREFIX_FILTERS=""
LINK_FLAP_INITIAL_BACKOFF_MS=1000
LINK_FLAP_MAX_BACKOFF_MS=60000
LOOPBACK_IFACE="lo"
OVERRIDE_LOOPBACK_ADDR=false
PREFIXES=""
REDISTRIBUTE_IFACES="lo1"
SEED_PREFIX=""
SET_LEAF_NODE=false
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
  echo "ERROR: No hostname found for the node, bailing out." >&2
  exit 1
fi
NODE_NAME=${HOSTNAME}

IPV6_DISABLED=$(sysctl -n net.ipv6.conf.all.disable_ipv6)
if [ "${IPV6_DISABLED}" != "0" ]; then
  echo "WARNING: IPv6 seems to be disabled and OpenR depends on it which may \
lead to incorrect functioning" >&2
fi

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

if [ -f "${OPENR_CONFIG}" ]; then
  source "${OPENR_CONFIG}"
  echo "Using OpenR config parameters from ${OPENR_CONFIG}"
else
  echo "Configuration not found at ${OPENR_CONFIG}. Using default!"
fi

#
# Let the magic begin. Keep the options sorted except for log level \m/
#

exec ${OPENR} \
  --advertise_interface_db=${ADVERTISE_INTERFACE_DB} \
  --alloc_prefix_len=${ALLOC_PREFIX_LEN} \
  --assume_drained=${ASSUME_DRAINED} \
  --config_store_filepath=${CONFIG_STORE_FILEPATH} \
  --decision_debounce_max_ms=${DECISION_DEBOUNCE_MAX_MS} \
  --decision_debounce_min_ms=${DECISION_DEBOUNCE_MIN_MS} \
  --domain=${DOMAIN} \
  --dryrun=${DRYRUN} \
  --enable_subnet_validation=${ENABLE_SUBNET_VALIDATION} \
  --enable_fib_sync=${ENABLE_FIB_SYNC} \
  --enable_health_checker=${ENABLE_HEALTH_CHECKER} \
  --enable_legacy_flooding=${ENABLE_LEGACY_FLOODING} \
  --enable_lfa=${ENABLE_LFA} \
  --enable_netlink_fib_handler=${ENABLE_NETLINK_FIB_HANDLER} \
  --enable_netlink_system_handler=${ENABLE_NETLINK_SYSTEM_HANDLER} \
  --enable_old_decision_module=${ENABLE_OLD_DECISION_MODULE} \
  --enable_perf_measurement=${ENABLE_PERF_MEASUREMENT} \
  --enable_prefix_alloc=${ENABLE_PREFIX_ALLOC} \
  --enable_rtt_metric=${ENABLE_RTT_METRIC} \
  --enable_segment_routing=${ENABLE_SEGMENT_ROUTING} \
  --enable_v4=${ENABLE_V4} \
  --enable_watchdog=${ENABLE_WATCHDOG} \
  --fib_handler_port=${FIB_HANDLER_PORT} \
  --health_checker_ping_interval_s=${HEALTH_CHECKER_PING_INTERVAL_S} \
  --ifname_prefix=${IFACE_PREFIXES} \
  --iface_regex_exclude=${IFACE_REGEX_EXCLUDE} \
  --iface_regex_include=${IFACE_REGEX_INCLUDE} \
  --ip_tos=${IP_TOS} \
  --key_prefix_filters=${KEY_PREFIX_FILTERS} \
  --link_flap_initial_backoff_ms=${LINK_FLAP_INITIAL_BACKOFF_MS} \
  --link_flap_max_backoff_ms=${LINK_FLAP_MAX_BACKOFF_MS} \
  --loopback_iface=${LOOPBACK_IFACE} \
  --node_name="${NODE_NAME}" \
  --override_loopback_addr=${OVERRIDE_LOOPBACK_ADDR} \
  --prefixes="${PREFIXES}" \
  --redistribute_ifaces=${REDISTRIBUTE_IFACES} \
  --seed_prefix=${SEED_PREFIX} \
  --set_loopback_address=${SET_LOOPBACK_ADDR} \
  --spark_fastinit_keepalive_time_ms=${SPARK_FASTINIT_KEEPALIVE_TIME_MS} \
  --spark_hold_time_s=${SPARK_HOLD_TIME_S} \
  --spark_keepalive_time_s=${SPARK_KEEPALIVE_TIME_S} \
  --set_leaf_node=${SET_LEAF_NODE} \
  --static_prefix_alloc=${STATIC_PREFIX_ALLOC} \
  --logbufsecs=0 \
  --logtostderr \
  --max_log_size=1 \
  --v=${VERBOSITY} \
  ${OPENR_ARGS}
