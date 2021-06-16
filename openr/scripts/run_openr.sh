#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

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
OPENR="openr"

# Keep this list in sorted order
CONFIG=""
# [TO BE DEPRECATED]
ENABLE_BGP_ROUTE_PROGRAMMING=true
ENABLE_SECURE_THRIFT_SERVER=false

# [Logging related config]
LOGGING=""
LOG_FILE=""
MIN_LOG_LEVEL=0
VERBOSITY=1
VMODULE=""

# [TLS related config]
TLS_ACCEPTABLE_PEERS=""
TLS_ECC_CURVE_NAME="prime256v1"
TLS_TICKET_SEED_PATH=""
X509_CA_PATH=""
X509_CERT_PATH=""
X509_KEY_PATH=""

#
# Some sanity checks before we start OpenR
#

if [ "${HOSTNAME}" = "localhost" ]; then
  echo "ERROR: No hostname found for the node, bailing out." >&2
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
  --config=${CONFIG} \
  --enable_bgp_route_programming=${ENABLE_BGP_ROUTE_PROGRAMMING} \
  --enable_secure_thrift_server=${ENABLE_SECURE_THRIFT_SERVER} \
  --logging=${LOGGING} \
  --minloglevel=${MIN_LOG_LEVEL} \
  --node_name=${NODE_NAME} \
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
  ${OPENR_ARGS}"

if [[ -n $LOG_FILE ]]; then
  echo "Redirecting logs to ${LOG_FILE}"
  exec "${OPENR}" ${ARGS} >> ${LOG_FILE} 2>&1
else
  exec "${OPENR}" ${ARGS}
fi
