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
ASSUME_DRAINED=false
CONFIG=""
CONFIG_STORE_FILEPATH="/tmp/aq_persistent_config_store.bin"
# [TO BE DEPRECATED]
DECISION_DEBOUNCE_MAX_MS=250
DECISION_DEBOUNCE_MIN_MS=10
DECISION_GRACEFUL_RESTART_WINDOW_S=-1
DRYRUN=false
ENABLE_BGP_ROUTE_PROGRAMMING=true
ENABLE_FIB_SERVICE_WAITING=true
ENABLE_LFA=false
ENABLE_NETLINK_FIB_HANDLER=true
ENABLE_PERF_MEASUREMENT=true
ENABLE_PLUGIN=false
ENABLE_PREFIX_ALLOC=false
ENABLE_RTT_METRIC=true
ENABLE_SECURE_THRIFT_SERVER=false
ENABLE_V4=false
ENABLE_WATCHDOG=true
FIB_HANDLER_PORT=60100
IFACE_REGEX_EXCLUDE=""
IFACE_REGEX_INCLUDE=""
IP_TOS=192
KEY_PREFIX_FILTERS=""
KVSTORE_KEY_TTL_MS=300000
KVSTORE_SYNC_INTERVAL_S=60
KVSTORE_TTL_DECREMENT_MS=1
KVSTORE_ZMQ_HWM=65536
LOGGING=""
LOG_FILE=""
MEMORY_LIMIT_MB=800
MIN_LOG_LEVEL=0
REDISTRIBUTE_IFACES="lo1"
SET_LEAF_NODE=false
MONITOR_MAX_EVENT_LOG=100
ENABLE_EVENT_LOG_SUBMISSION=true
TLS_ACCEPTABLE_PEERS=""
TLS_ECC_CURVE_NAME="prime256v1"
TLS_TICKET_SEED_PATH=""
VERBOSITY=1
VMODULE=""
X509_CA_PATH=""
X509_CERT_PATH=""
X509_KEY_PATH=""
PLUGIN_ARGS=

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

ARGS="\
  --assume_drained=${ASSUME_DRAINED} \
  --config=${CONFIG} \
  --config_store_filepath=${CONFIG_STORE_FILEPATH} \
  --decision_debounce_max_ms=${DECISION_DEBOUNCE_MAX_MS} \
  --decision_debounce_min_ms=${DECISION_DEBOUNCE_MIN_MS} \
  --decision_graceful_restart_window_s=${DECISION_GRACEFUL_RESTART_WINDOW_S} \
  --dryrun=${DRYRUN} \
  --enable_bgp_route_programming=${ENABLE_BGP_ROUTE_PROGRAMMING} \
  --enable_fib_service_waiting=${ENABLE_FIB_SERVICE_WAITING} \
  --enable_lfa=${ENABLE_LFA} \
  --enable_netlink_fib_handler=${ENABLE_NETLINK_FIB_HANDLER} \
  --enable_perf_measurement=${ENABLE_PERF_MEASUREMENT} \
  --enable_plugin=${ENABLE_PLUGIN} \
  --enable_rtt_metric=${ENABLE_RTT_METRIC} \
  --enable_secure_thrift_server=${ENABLE_SECURE_THRIFT_SERVER} \
  --enable_v4=${ENABLE_V4} \
  --enable_watchdog=${ENABLE_WATCHDOG} \
  --fib_handler_port=${FIB_HANDLER_PORT} \
  --iface_regex_exclude=${IFACE_REGEX_EXCLUDE} \
  --iface_regex_include=${IFACE_REGEX_INCLUDE} \
  --ip_tos=${IP_TOS} \
  --key_prefix_filters=${KEY_PREFIX_FILTERS} \
  --kvstore_key_ttl_ms=${KVSTORE_KEY_TTL_MS} \
  --kvstore_sync_interval_s=${KVSTORE_SYNC_INTERVAL_S} \
  --kvstore_ttl_decrement_ms=${KVSTORE_TTL_DECREMENT_MS} \
  --kvstore_zmq_hwm=${KVSTORE_ZMQ_HWM} \
  --logging=${LOGGING} \
  --memory_limit_mb=${MEMORY_LIMIT_MB} \
  --minloglevel=${MIN_LOG_LEVEL} \
  --node_name=${NODE_NAME} \
  --redistribute_ifaces=${REDISTRIBUTE_IFACES} \
  --set_leaf_node=${SET_LEAF_NODE} \
  --monitor_max_event_log=${MONITOR_MAX_EVENT_LOG} \
  --enable_event_log_submission=${ENABLE_EVENT_LOG_SUBMISSION} \
  --tls_acceptable_peers=${TLS_ACCEPTABLE_PEERS} \
  --tls_ecc_curve_name=${TLS_ECC_CURVE_NAME} \
  --tls_ticket_seed_path=${TLS_TICKET_SEED_PATH} \
  --x509_ca_path=${X509_CA_PATH} \
  --x509_cert_path=${X509_CERT_PATH} \
  --x509_key_path=${X509_KEY_PATH} \
  --logbufsecs=0 \
  --logtostderr \
  --max_log_size=1 \
  --v=${VERBOSITY} \
  --vmodule=${VMODULE} \
  ${PLUGIN_ARGS} \
  ${OPENR_ARGS}"

if [[ -n $LOG_FILE ]]; then
  echo "Redirecting logs to ${LOG_FILE}"
  exec "${OPENR}" ${ARGS} >> ${LOG_FILE} 2>&1
else
  exec "${OPENR}" ${ARGS}
fi
