#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

source "$(dirname "$0")/common.sh"

# TODO: Get apt deb installing working - dies in Docker container with libzstd-dev fixed
# "$PYTHON3" "$GETDEPS" --allow-system-packages install-system-deps --recursive openr
# errorCheck "Failed to install-system-deps for openr"

"$PYTHON3" "$GETDEPS" --allow-system-packages build --no-tests --install-prefix "$INSTALL_PREFIX" \
--extra-cmake-defines "$EXTRA_CMAKE_DEFINES" openr
errorCheck "Failed to build openr"

# TODO: Maybe fix src-dir to be absolute reference to dirname $0's parent
"$PYTHON3" "$GETDEPS" fixup-dyn-deps --strip --src-dir=. openr _artifacts/linux  --project-install-prefix openr:"$INSTALL_PREFIX" --final-install-prefix "$INSTALL_PREFIX"
errorCheck "Failed to fixup-dyn-deps for openr"
