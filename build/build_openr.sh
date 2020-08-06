#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

GETDEPS="$(dirname "$0")/fbcode_builder/getdeps.py"
PYTHON3=$(command -v python3)

if [ "$PYTHON3" == "" ]; then
  echo "ERROR: No \`python3\` in PATH"
  exit 1
fi

python3 "$GETDEPS" --allow-system-packages install-system-deps --recursive openr
python3 "$GETDEPS" --allow-system-packages build --no-tests openr
