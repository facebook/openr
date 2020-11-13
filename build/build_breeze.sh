#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

source "$(dirname "$0")/common.sh"

THRIFT_BIN_DIR="/opt/facebook/fbthrift/bin"
if [ ! -d "${THRIFT_BIN_DIR}" ]; then
  echo "No ${THRIFT_BIN_DIR} dir ... exiting ..."
  exit 3
fi

export PATH="${PATH}:${THRIFT_BIN_DIR}"
"$PIP3" --no-cache-dir install --upgrade pip setuptools wheel
"$PIP3" install .
"$PYTHON3" build_thrift.py
