#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

PYTHON3=$(command -v python3)

if [ "$PYTHON3" == "" ]; then
  echo "ERROR: No \`python3\` in PATH"
  exit 1
fi

python3 build/fbcode_builder/getdeps.py build openr
