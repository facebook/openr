#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

source "$(dirname "$0")/common.sh"

# "$PYTHON3" "$GETDEPS" --allow-system-packages install-system-deps --recursive fbthrift
# errorCheck "Failed to install-system-deps for fbthrift"

# Set envar to trigger building fbthrift py3 support
export BUILD_THRIFT_PY3="ON"
# "$PYTHON3" "$GETDEPS" --allow-system-packages build --no-tests --install-prefix "$INSTALL_PREFIX" fbthrift
"$PYTHON3" "$GETDEPS" build --no-tests --install-prefix "$INSTALL_PREFIX" fbthrift
errorCheck "Failed to build fbthrift"

# Needed for Open/R thrift server
"$PYTHON3" "$GETDEPS" build --no-tests --install-prefix "$INSTALL_PREFIX" fb303
errorCheck "Failed to build fbthrift"

# TODO: Maybe fix src-dir to be absolute reference to dirname $0's parent
#"$PYTHON3" "$GETDEPS" fixup-dyn-deps --strip --src-dir=. openr _artifacts/linux  --project-install-prefix openr:"$INSTALL_PREFIX" --final-install-prefix "$INSTALL_PREFIX"
#errorCheck "Failed to fixup-dyn-deps for openr"