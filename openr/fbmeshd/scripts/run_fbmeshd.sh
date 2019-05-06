#!/bin/bash

#
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

#
# NOTE
#
# This script is based on the similar run_openr.sh used for OpenR
#

#
# NOTE
#
# Default fbmeshd configuration
# Override the ones you need in `/etc/sysconfig/fbmeshd` for custom
# configuration on the node or pass config name to this script
# e.g. run_fbmeshd.sh /etc/fbmeshd.conf
#

# fbmeshd binary path or command name present on bin paths
FBMESHD=/usr/sbin/fbmeshd

# Keep this list in sorted order
DECISION_REP_PORT=60004
ENABLE_ENCRYPTION=false
ENABLE_MESH_SPARK=true
ENABLE_OPENR_METRIC_MANAGER=true
ENABLE_ROUTING=false
ENABLE_SEPARA=false
ENABLE_SHORT_NAMES=false
ENABLE_WATCHDOG=true
ENCRYPTION_PASSWORD=''
ENCRYPTION_SAE_GROUPS='19'
EVENT_BASED_PEER_SELECTOR=false
FBMESHD_SERVICE_PORT=30303
GATEWAY_CONNECTIVITY_MONITOR_ADDRESSES='8.8.4.4:443'
GATEWAY_CONNECTIVITY_MONITOR_INTERFACE='eth0'
GATEWAY_CONNECTIVITY_MONITOR_INTERVAL_S=1
GATEWAY_CONNECTIVITY_MONITOR_ROBUSTNESS=2
GATEWAY_CONNECTIVITY_MONITOR_SOCKET_TIMEOUT_S=5
KVSTORE_CMD_PORT=60002
KVSTORE_PUB_PORT=60001
LINK_MONITOR_CMD_PORT=60006
MEMORY_LIMIT_MB=300
MESH_CENTER_FREQ1=5805
MESH_CENTER_FREQ2=0
MESH_CHANNEL_TYPE='20'
MESH_FREQUENCY=5805
MESH_ID='mesh-soma'
MESH_IFNAME='mesh0'
MESH_MAC_ADDRESS=''
MESH_MAX_PEER_LINKS=32
MESH_MTU=1520
MESH_RSSI_THRESHOLD=-80
MESH_SPARK_CMD_PORT=60018
MESH_SPARK_NEIGHBOR_HOLD_TIME_S=60
MESH_SPARK_PEER_WHITELIST=''
MESH_SPARK_REPORT_PORT=60019
MESH_SPARK_SYNC_PERIOD_S=20
MESH_TTL=31
MESH_TTL_ELEMENT=31
MONITOR_REP_PORT=60008
NODE_NAME='node1'
PEER_SELECTOR_EVICTION_PERIOD_S=60
PEER_SELECTOR_MAX_ALLOWED=4294967295
PEER_SELECTOR_MIN_GATE_CONNECTIONS=2
PEER_SELECTOR_POLL_INTERVAL_S=1
PREFIX_MANAGER_CMD_PORT=60011
ROUTE_DAMPENER_HALFLIFE=60
ROUTE_DAMPENER_MAX_SUPPRESS_LIMIT=180
ROUTE_DAMPENER_PENALTY=1000
ROUTE_DAMPENER_REUSE_LIMIT=750
ROUTE_DAMPENER_SUPPRESS_LIMIT=2000
SEPARA_BROADCAST_INTERVAL_S=1
SEPARA_DOMAIN_CHANGE_THRESHOLD_FACTOR=1
SEPARA_DOMAIN_LOCKDOWN_PERIOD_S=60
SEPARA_HELLO_PORT=6667
STEP_DETECTOR_ABS_THRESHOLD=500
STEP_DETECTOR_AIRTIME_METRIC_AVG_INTERVAL_S=10
STEP_DETECTOR_AIRTIME_METRIC_SAMPLE_INTERVAL_S=1
STEP_DETECTOR_FAST_WND_SIZE=10
STEP_DETECTOR_HIGH_THRESHOLD=40
STEP_DETECTOR_LOW_THRESHOLD=10
STEP_DETECTOR_MAX_METRIC=1366
STEP_DETECTOR_SLOW_WND_SIZE=30
STEP_DETECTOR_STATIC=false
USERSPACE_MESH_PEERING=true
USERSPACE_PEERING_VERBOSITY=0
VERBOSITY=0
WATCHDOG_INTERVAL_S=20
WATCHDOG_THRESHOLD_S=300

#
# Some sanity checks before we start fbmeshd
#

if [ "${HOSTNAME}" = "localhost" ]; then
  echo "ERROR: No hostname found for the node, bailing out." >&2
  exit 1
fi
NODE_NAME=${HOSTNAME}

#
# Load custom configuration if any!
#
FBMESHD_CONFIG="/etc/sysconfig/fbmeshd"
FBMESHD_ARGS="$*"
if [ -n "$1" ]; then
  if [ "$1" = "--help" ]; then
    echo "USAGE: run_fbmeshd.sh [config_file_path] [openr_flags]"
    echo "If config_file_path is not provided, we will source the one at \
/etc/sysconfig/fbmeshd"
    echo "If fbmeshd_flags are provided, they will be passed along to fbmeshd and \
override any passed by this script"
    exit 1
  fi
  if [ -f "$1" ]; then
    FBMESHD_CONFIG=$1
    FBMESHD_ARGS="${*:2}"
  fi
fi

if [ -f "${FBMESHD_CONFIG}" ]; then
  # shellcheck disable=SC1090
  source "${FBMESHD_CONFIG}"
  echo "Using fbmeshd config parameters from ${FBMESHD_CONFIG}"

  if [ -d "${FBMESHD_CONFIG}.d" ]; then
    for f in "${FBMESHD_CONFIG}.d"/*; do
      echo "Overriding fbmeshd config parameters from ${f}"
      # shellcheck disable=SC1090
      source "${f}";
    done
  fi
else
  echo "Configuration not found at ${FBMESHD_CONFIG}. Using default!"
fi

#
# Let the magic begin. Keep the options sorted except for log level \m/
#

exec ${FBMESHD} \
  --decision_rep_port="${DECISION_REP_PORT}" \
  --enable_encryption="${ENABLE_ENCRYPTION}" \
  --enable_mesh_spark="${ENABLE_MESH_SPARK}" \
  --enable_openr_metric_manager="${ENABLE_OPENR_METRIC_MANAGER}" \
  --enable_routing="${ENABLE_ROUTING}" \
  --enable_separa="${ENABLE_SEPARA}" \
  --enable_short_names="${ENABLE_SHORT_NAMES}" \
  --enable_watchdog="${ENABLE_WATCHDOG}" \
  --encryption_password="${ENCRYPTION_PASSWORD}" \
  --encryption_sae_groups="${ENCRYPTION_SAE_GROUPS}" \
  --event_based_peer_selector="${EVENT_BASED_PEER_SELECTOR}" \
  --fbmeshd_service_port="${FBMESHD_SERVICE_PORT}" \
  --gateway_connectivity_monitor_addresses="${GATEWAY_CONNECTIVITY_MONITOR_ADDRESSES}" \
  --gateway_connectivity_monitor_interface="${GATEWAY_CONNECTIVITY_MONITOR_INTERFACE}" \
  --gateway_connectivity_monitor_interval_s="${GATEWAY_CONNECTIVITY_MONITOR_INTERVAL_S}" \
  --gateway_connectivity_monitor_robustness="${GATEWAY_CONNECTIVITY_MONITOR_ROBUSTNESS}" \
  --gateway_connectivity_monitor_socket_timeout_s="${GATEWAY_CONNECTIVITY_MONITOR_SOCKET_TIMEOUT_S}" \
  --is_openr_enabled="$([ ${ENABLE_ROUTING} == false ] && echo "true" || echo "false")" \
  --kvstore_cmd_port="${KVSTORE_CMD_PORT}" \
  --kvstore_pub_port="${KVSTORE_PUB_PORT}" \
  --link_monitor_cmd_port="${LINK_MONITOR_CMD_PORT}" \
  --memory_limit_mb="${MEMORY_LIMIT_MB}" \
  --mesh_center_freq1="${MESH_CENTER_FREQ1}" \
  --mesh_center_freq2="${MESH_CENTER_FREQ2}" \
  --mesh_channel_type="${MESH_CHANNEL_TYPE}" \
  --mesh_frequency="${MESH_FREQUENCY}" \
  --mesh_id="${MESH_ID}" \
  --mesh_ifname="${MESH_IFNAME}" \
  --mesh_mac_address="${MESH_MAC_ADDRESS}" \
  --mesh_max_peer_links="${MESH_MAX_PEER_LINKS}" \
  --mesh_mtu="${MESH_MTU}" \
  --mesh_rssi_threshold="${MESH_RSSI_THRESHOLD}" \
  --mesh_spark_cmd_port="${MESH_SPARK_CMD_PORT}" \
  --mesh_spark_neighbor_hold_time_s="${MESH_SPARK_NEIGHBOR_HOLD_TIME_S}" \
  --mesh_spark_peer_whitelist="${MESH_SPARK_PEER_WHITELIST}" \
  --mesh_spark_report_port="${MESH_SPARK_REPORT_PORT}" \
  --mesh_spark_sync_period_s="${MESH_SPARK_SYNC_PERIOD_S}" \
  --mesh_ttl="${MESH_TTL}" \
  --mesh_ttl_element="${MESH_TTL_ELEMENT}" \
  --monitor_rep_port="${MONITOR_REP_PORT}" \
  --node_name="${NODE_NAME}" \
  --peer_selector_eviction_period_s="${PEER_SELECTOR_EVICTION_PERIOD_S}" \
  --peer_selector_max_allowed="${PEER_SELECTOR_MAX_ALLOWED}" \
  --peer_selector_min_gate_connections="${PEER_SELECTOR_MIN_GATE_CONNECTIONS}" \
  --peer_selector_poll_interval_s="${PEER_SELECTOR_POLL_INTERVAL_S}" \
  --prefix_manager_cmd_port="${PREFIX_MANAGER_CMD_PORT}" \
  --route_dampener_halflife="${ROUTE_DAMPENER_HALFLIFE}" \
  --route_dampener_max_suppress_limit="${ROUTE_DAMPENER_MAX_SUPPRESS_LIMIT}" \
  --route_dampener_penalty="${ROUTE_DAMPENER_PENALTY}" \
  --route_dampener_reuse_limit="${ROUTE_DAMPENER_REUSE_LIMIT}" \
  --route_dampener_suppress_limit="${ROUTE_DAMPENER_SUPPRESS_LIMIT}" \
  --separa_broadcast_interval_s="${SEPARA_BROADCAST_INTERVAL_S}" \
  --separa_domain_change_threshold_factor="${SEPARA_DOMAIN_CHANGE_THRESHOLD_FACTOR}" \
  --separa_domain_lockdown_period_s="${SEPARA_DOMAIN_LOCKDOWN_PERIOD_S}" \
  --separa_hello_port="${SEPARA_HELLO_PORT}" \
  --step_detector_abs_threshold="${STEP_DETECTOR_ABS_THRESHOLD}" \
  --step_detector_airtime_metric_avg_interval_s="${STEP_DETECTOR_AIRTIME_METRIC_AVG_INTERVAL_S}" \
  --step_detector_airtime_metric_sample_interval_s="${STEP_DETECTOR_AIRTIME_METRIC_SAMPLE_INTERVAL_S}" \
  --step_detector_fast_wnd_size="${STEP_DETECTOR_FAST_WND_SIZE}" \
  --step_detector_high_threshold="${STEP_DETECTOR_HIGH_THRESHOLD}" \
  --step_detector_low_threshold="${STEP_DETECTOR_LOW_THRESHOLD}" \
  --step_detector_max_metric="${STEP_DETECTOR_MAX_METRIC}" \
  --step_detector_slow_wnd_size="${STEP_DETECTOR_SLOW_WND_SIZE}" \
  --step_detector_static="${STEP_DETECTOR_STATIC}" \
  --userspace_mesh_peering="${USERSPACE_MESH_PEERING}" \
  --userspace_peering_verbosity="${USERSPACE_PEERING_VERBOSITY}" \
  --v="${VERBOSITY}" \
  --watchdog_interval_s="${WATCHDOG_INTERVAL_S}" \
  --watchdog_threshold_s="${WATCHDOG_THRESHOLD_S}" \
  --logbufsecs=0 \
  --logtostderr=1 \
  --max_log_size=1 \
  ${FBMESHD_ARGS}
