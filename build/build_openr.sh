#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# shellcheck disable=SC1090
source "$(dirname "$0")/common.sh"

# TODO: Get apt deb installing working - dies in Docker container with libzstd-dev fixed
# "$PYTHON3" "$GETDEPS" --allow-system-packages install-system-deps --recursive openr
# errorCheck "Failed to install-system-deps for openr"

# --src-dir=. builds the Open/R sources present in this checkout (the Dockerfile
# COPYs them into /src and runs this script from there). Without it, getdeps
# clones the canonical repo from the manifest's repo_url at a pinned revision
# and ignores the local source, so local changes never get built.
"$PYTHON3" "$GETDEPS" --allow-system-packages build --src-dir=. --no-tests --install-prefix "$INSTALL_PREFIX" \
--extra-cmake-defines "$EXTRA_CMAKE_DEFINES" openr
errorCheck "Failed to build openr"

# TODO: Maybe fix src-dir to be absolute reference to dirname $0's parent
"$PYTHON3" "$GETDEPS" fixup-dyn-deps --strip --src-dir=. openr _artifacts/linux  --project-install-prefix openr:"$INSTALL_PREFIX" --final-install-prefix "$INSTALL_PREFIX"
errorCheck "Failed to fixup-dyn-deps for openr"
