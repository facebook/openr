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
# This script uses the default fbmeshd configuration specified for command line
# flags in the fbmeshd source code.
#
# Override the ones you need to in `/etc/sysconfig/fbmeshd` for custom
# configuration on the node, using this format:
#   CONFIG_fbmeshd_<flag>='<value>'
#   e.g. CONFIG_fbmeshd_node_name='my_new_node_name'
#
# You can also pass an alternate config file path to use, and additional flags
# after the config file path (if present), which will take priority over all
# others.
#   e.g. run_fbmeshd.sh /etc/fbmeshd.conf
#   e.g. run_fbmeshd.sh --node_name=my_new_node_name
#   e.g. run_fbmeshd.sh /etc/fbmeshd.conf --node_name=my_new_node_name
#

# fbmeshd binary path or command name present on bin paths
FBMESHD=/usr/sbin/fbmeshd

#
# Load custom configuration if any
#
FBMESHD_CONFIG="/etc/sysconfig/fbmeshd"
FBMESHD_ARGS="$*"
if [ -n "$1" ]; then
  if [ "$1" = "--help" ]; then
    echo "USAGE: run_fbmeshd.sh [config_file_path] [fbmeshd_flags]"
    echo "If config_file_path is not provided, we will source the one at /etc/sysconfig/fbmeshd"
    echo "If fbmeshd_flags are provided, they will be passed along to fbmeshd and override any passed by this script"
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
# Parse the options into the format that we need for passing to the executable
#
mapfile -t FBMESHD_CONFIG_VARS < <(set -o posix; set | grep '^CONFIG_fbmeshd_' | cut -d '=' -f1)

FBMESHD_CONFIG_ARGS=()
for var in "${FBMESHD_CONFIG_VARS[@]}"
do
  FLAG="${var//CONFIG_fbmeshd_/--}"
  VALUE=$(eval "echo $""$var")
  FBMESHD_CONFIG_ARGS+=("$FLAG=$VALUE")
done

#
# Let the magic begin. Keep the options sorted except for log level \m/
#

exec ${FBMESHD} \
  --logbufsecs=0 \
  --logtostderr=1 \
  --max_log_size=1 \
  "${FBMESHD_CONFIG_ARGS[@]}" \
  "${FBMESHD_ARGS[@]}"
